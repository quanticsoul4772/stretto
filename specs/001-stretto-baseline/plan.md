# Implementation Plan: Stretto Baseline

**Branch**: `001-stretto-baseline` | **Date**: 2026-07-06 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/001-stretto-baseline/spec.md`

**Note**: This plan captures the architectural decisions that produced the shipped generative ambient music synth (Stretto) through ~90 merged PRs, serving as the baseline for future specs (e.g., `002-master-limiter-compressor`). All design choices here were validated against `regression_16s.sha256` and the multi-seed integration test.

## Summary

Build a tiny native C99 generative ambient music synthesizer. A single binary producing continuous, deterministic-but-evolving music over minutes; reproducible across `--seed N`; live-modifiable via keyboard; recorded to 16-bit 48 kHz stereo WAV on demand. Architecture is a synth pipeline (per-voice synthesis → voice mix → reverb → delay → saturation → compressor → limiter) fed by a six-layer generative state machine (modal-scale Markov, L-system melody, 2nd-order Markov counter, chord-progression Markov, cellular-automaton degree mask, motif-recall ring buffer), gated by a 4-section song-form crossfader. Single 128 KB arena allocator; no `malloc` after startup.

## Technical Context

**Language/Version**: C99 (gcc). No C++; no external runtime beyond libc + libpulse (Linux) / winmm (Windows).

**Primary Dependencies**:
- libc (always)
- libpulse (Linux live audio); winmm (Windows live audio)
- UPX (Windows release pack target)
- gcc / MinGW cross-toolchain (Windows cross-compile)
- gcov + bash (coverage)
- python3 + numpy (multi-seed audio analysis)

**Storage**: N/A at runtime; single 128 KB static arena (`arena.c`); all `.bss` Markov tables, ring buffers, wavetable, scale tables built at compile time by `gen_*_table.c` programs.

**Testing**:
- `make test` — bit-exact 16-s SHA-256 (`golden/regression_16s.sha256`)
- `make test-unit` — per-module unit (`tests/unit/test_*.c`)
- `make test-multiseed` — 4-seed audio-characteristic bounds (peak, RMS, clip, centroid, ZCR)
- `make test-smoke` — live-mode 2-second timeout, no TTY
- `make coverage` — per-file gates via gcov
- `make win && make winpack` — Windows cross-compile + UPX

**Target Platform**: Linux (PulseAudio default output, fallback scripted `--no-ui`) and Windows 10+ (`ENABLE_VIRTUAL_TERMINAL_PROCESSING`, waveOut). Modern terminals with ANSI support.

**Project Type**: Native CLI / interactive generative-music tool. Single-binary, single-process. No daemon, no library form, no plugin host.

**Performance Goals**:
- 48 kHz × 2-ch × 16-bit interleaved PCM streaming
- < 1 s cold-launch to first audio buffer
- Live-mode key response ≤ one audio buffer (~21 ms)
- Uptime ≥ 30 min unattended, no clipping, no drift

**Constraints**:
- Packed Windows `.exe` ≤ 48 KB (CI-failing past this; last ~37 KB)
- Stripped Linux binary soft-target 24 KB (~40 KB with libpulse; Makefile warns past target)
- Single-binary, no shared library form
- All dynamic allocation from a 128 KB arena (no `malloc`, no `free`)
- Audio generation MUST NOT read `time()` (determinism)

**Scale/Scope**: One repo, one binary, one feature (the synth) serving as the baseline against which future specs are validated. ~90 PRs already merged; future changes are bounded by `--seed N` reproducibility.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Evidence |
|-----------|--------|----------|
| **I. Tiny Native Binary (NON-NEGOTIABLE)** | ✅ PASS | CI on every PR enforces ≤48 KB UPX-packed Windows; `Makefile` `size` warns on Linux 24 KB target. Last packed ~37 KB. No library form. |
| **II. C99 Only** | ✅ PASS | Build is pure `gcc -std=c99`. No C++ symbols. Build-time utilities (`gen_*_table.c`) also C99. |
| **III. Deterministic (NON-NEGOTIABLE)** | ✅ PASS (v1.0.1) | `--seed N` produces byte-identical output on supported build targets (Linux glibc + Windows winmm, both little-endian x86). Runtime engine is integer-only (int16/int32/int64); build-time tables round doubles to integers committed to headers; WAV writer is native-endian RIFF. Bit-exact 16-s SHA-256 + 4-seed multi-render integration tests gate the Linux runner; cross-platform WAV byte-identity is by code construction (no Windows runner in CI). |
| **IV. Ambient + Algorithmic Aesthetic** | ✅ PASS | 6-scale modal Markov; 2nd-order Markov counter; chord-progression Markov every 2 bars; L-system over `u U d D r .`; 96-bar section crossfader; ring-buffer motif recall; cellular-automaton degree mask. |
| **V. Cleanly Modular** | ✅ PASS | One `.c`/`.h` per concern; one-way dependency direction documented in `ARCHITECTURE.md`. No externs across module boundaries. |
| **VI. Test Discipline (NON-NEGOTIABLE)** | ✅ PASS | `tests/unit/test_*.c` per pure-synth module; per-file coverage gates: 95 % (arena/effects/voice/section/density/motif/mixer), 90 % (gen/lsystem/chord_progression/wav/main). Interactive modules (`ui.c`, `keys.c`, `audio_pulse.c`) excluded with rationale. |
| **VII. No Partial Features** | ✅ PASS | Zero `TODO` in committed code; zero placeholder `(void)` casts; every PR ships a bit-exact golden (regenerated in the same PR when intentional). |
| **VIII. Document Why, Not What** | ✅ PASS | Comments capture invariants (FR-009a glide determinism), magic numbers (reverb coefficients, detune cents), and workaround context. `README.md` / `ARCHITECTURE.md` / `CHANGELOG.md` refreshed in dedicated `docs/*` PRs. |
| **IX. Cross-Platform From Day One** | ✅ PASS | PulseAudio + winmm are first-class. CI on every PR runs Linux + Windows cross-compile + UPX. Platform code isolated in `audio_pulse.c` / `audio_winmm.c` and `#ifdef _WIN32` blocks (see `ui.c`). |
| **X. Generative > Random** | ✅ PASS | Six interacting layers (scale Markov → chord Markov → CA mask → L-system melody → 2nd-order Markov counter → motif ring buffer) plus 96-bar section state machine. Pure randomness is a tool, not a goal. |

All gates pass. No violations require justification.

## Project Structure

### Documentation (this feature)

```text
specs/001-stretto-baseline/
├── spec.md              # Feature specification (this baseline)
├── plan.md              # This file
├── research.md          # Phase 0 — synthesis-of-existing-research, with decision/rationale tables
├── quickstart.md        # Phase 1 — minimal usage examples (CLI flags, common tasks)
└── tasks.md             # Phase 2 — task inventory stub (regenerated by /speckit-tasks as needed)

# data-model.md + contracts/ deliberately omitted — see below.
```

`data-model.md` was not generated because every entity (`Voice`, `Scale`, `Chord function`, `Active mask`, `Section`, `Motif phrase`, `Render request`) is already fully enumerated in `spec.md → Key Entities` with field-level detail and the canonical C types live in the respective module headers (`voice.h`, `gen.h`, `chord_progression.h`, `section.h`, `motif.h`, `wav.h`). Generation would be pure duplication.

`contracts/` was not populated because Stretto's only "interface" is its CLI + the on-disk `--seed N → WAV` reproducibility contract (already specified under FR-030..FR-034 and SC-002 in `spec.md`). There are no library APIs, plugin ABIs, network endpoints, or interchange formats to contract.

### Source Code (repository root)

```text
D:\Projects\stretto\
├── main.c                       # CLI parsing, render vs live dispatch, exit codes
├── arena.{c,h}                  # 128 KB static arena, arena_alloc / arena_used
├── audio.{h}, audio_pulse.c, audio_winmm.c   # platform audio streaming
├── voice.{c,h}                  # voice pool + 6 synthesis methods (KS/FM/DRUM/WT/ADD/SUB)
├── gen.{c,h}                    # generative state (PRNG, scale, CA, Markov-chord, motif dispatch)
├── lsystem.{c,h}                # 6-symbol melody grammar + 3 hand-tuned characters
├── chord_progression.{c,h}      # 7-function Markov chain, every 2 bars
├── section.{c,h}                # 4-section state, 96-bar period, 8-bar crossfade
├── density.{c,h}                # adaptive per-bar density bias
├── motif.{c,h}                  # 8-slot ring buffer, ±2 diatonic transposition
├── effects.{c,h}                # reverb, delay, saturation, compressor, limiter, sat16
├── mixer.{c,h}                  # voice pool mix → effects chain
├── keys.{c,h}                   # live-mode key handler
├── ui.{c,h}                     # ANSI oscilloscope + status row
├── wav.{c,h}                    # RIFF/WAVE writer, --render exit path
├── config.h                     # tunables (constants only, no runtime config)
├── gen_sin_table.c              # build-time: sin lookup table
├── gen_env_table.c              # build-time: envelope coefficients
├── gen_euclid_table.c           # build-time: Euclidean rhythm patterns
├── gen_note_table.c             # build-time: note → frequency conversion
├── gen_wavetable.c              # build-time: 256-sample × 8 waveform wavetable
├── Makefile                     # GNU Make + per-target rules
├── ARCHITECTURE.md              # one-way dependency map, regenerate-manifest contract
├── CLAUDE.md                    # speckit agent-context marker (points to plan.md)
├── CHANGELOG.md                 # per-PR summary, refreshed in docs/* PRs
├── README.md                    # listener-facing quick-start
└── tests/
    ├── test_bitexact.sh         # 16-s SHA-256 regression
    ├── test_multi_seed.sh       # 4-seed audio bounds (python3+numpy)
    ├── test_smoke_live.sh       # 2-s timeout live-mode launch
    ├── unit/
    │   ├── test.h
    │   └── test_arena.c test_chord_progression.c test_density.c
    │       test_effects.c test_gen.c test_keys.c test_lsystem.c
    │       test_mixer.c test_motif.c test_section.c test_voice.c test_wav.c
    └── golden/
        ├── regression_16s.sha256
        └── regression_multiseed.sha256.txt

pre-commit / CI hooks live in `.github/workflows/ci.yml`.
```

**Structure Decision**: Single fixed-binary layout — `Option 1: Single project` from `plan-template.md`. The dependency direction (per Constitution V) is:

```
main → {wav, audio, ui, gen, effects, voice, arena}
audio_* → {mixer, ui, keys, arena}
wav → {mixer, arena}
mixer → {gen, voice, effects}
keys → {ui, gen, voice, effects}
ui → {voice, gen, effects}
gen → {voice, lsystem, chord_progression, section, density, motif, effects}
voice → {arena, effects}
```

## Complexity Tracking

> No Constitution violations — table intentionally empty.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| — | — | — |

The architecture stays well under the binary size budget (37 KB packed vs 48 KB ceiling) with 11 KB of headroom for future features, and well under the 128 KB arena (last measured ~25 KB peak). No principle is bent.

## Amendment history

- **2026-07-08** — Constitution v1.0.1 → v1.1.0: Principle I stripped-Linux binary budget amended ≤24 KB → ≤50 KB per PR #116. References to ≤24 KB / 24 KB / 24576 bytes for the Linux stripped binary size in this file are preserved as the spec-at-time-of-authoring claim; the current Constitution target is ≤50 KB per `.specify/memory/constitution.md` Principle I amendment paragraph (the 003 MIDI-input chain's principled ~19 KB cost is the documented reason for the raise).
