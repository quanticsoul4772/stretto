# Implementation Plan: MIDI Input (003-midi-input)

**Branch**: `003-midi-input` | **Date**: 2026-07-06 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/003-midi-input/spec.md` (post-clarify: 4 Q/A recorded 2026-07-06 — polyphonic 11-voice w/ voice stealing, callback-only threading, no auto-reconnect, CC=0 at startup).

**Note**: This template is filled in by the `/speckit-plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

Add USB-MIDI keyboard input as a first-class control surface for the live synth, mirroring the existing `keys.c` keyboard dispatcher. A new `audio_midi.{c,h}` module owns the SPSC ring buffer + Note/CC → voice pool / live-parameter routing; platform backends `audio_midi_linux.c` (ALSA sequencer) and `audio_midi_winmm.c` (Win32 midiInProc) enqueue events to that ring buffer; the audio thread drains the buffer at the start of every `render_chunk` and dispatches. Polyphonic 11 voices (`N_VOICES` in `voice.h:8`) with voice stealing into the existing voice pool. New CLI surface: `--midi [N]`, `--midi-list-devices`, `--midi-channel N`, `--no-midi`. No audio engine refactor (existing 11-voice role-scoped engine is preserved verbatim; only additive new entry points `voice_pool_trigger_midi` / `voice_pool_release_midi` are introduced); no determinism regression in the `--no-midi` path.

## Technical Context

**Language/Version**: C99 (per Constitution II; gcc default dialect gnu11, used compatibly with `__atomic_*` built-ins for SPSC ring buffer)

**Primary Dependencies**:
- Linux: libasound (libasound2-dev, ALSA sequencer API: `snd_seq_*` — `snd_seq_open()`, `snd_seq_event_input()`, `snd_seq_connect_from()`; one `pthread` worker created via `pthread_create()` — no `snd_seq_create_thread()`, which is not a stock libasound API; preflight correction 2026-07-06 documented in `research.md` D1)
- Windows: winmm (already linked for `waveOut`; new use: `midiInOpen()`, `midiInStart()`, `midiInProc` callback, `midiInGetDevCaps()` for enumeration)
- libc only otherwise

**Storage**: N/A (in-memory only; 256-event ring buffer from the 128 KB static arena per FR-040)

**Testing**:
- `tests/unit/test_midi.c` (new; per-file coverage gate ≥90% per FR-054)
- `tests/unit/test_keys.c` (unchanged; gates **SC-007** that `--midi` doesn't break live keystroke handling — both code paths share `keys_dispatch`-style state updates and the existing test verifies the keys path is unaffected by the new MIDI entries; **M3 fix**: explicitly enumerated in plan because the analyze pass flagged SC-007's coverage as a missing reference here)
- `tests/test_smoke_live.sh` extended with a virtual-MIDI-loopback step on Linux (snd-seq-dummy + `amidi` send; auto-skip if libasound or loopback unavailable — mirrors the existing PulseAudio auto-skip pattern)
- `tests/test_bitexact.sh` (existing) gates `--no-midi` byte-identity per FR-053
- `tests/test_multi_seed.sh` (existing) catches multi-seed drift

**Target Platform**: Linux glibc (Ubuntu-latest CI) + Windows winmm cross-compile to native via MinGW; both little-endian x86 per Constitution III v1.0.1

**Project Type**: Native CLI (live + render mode) — extends the existing `synth` binary

**Performance Goals**:
- Note On → audible output within 1 audio buffer (~21 ms @ 48 kHz / 1024 frames) per SC-001 / FR-010
- MIDI thread / callback MUST NOT block the audio thread (FR-033)
- Zero CPU cost when `--no-midi` is set (FR-005)

**Constraints**:
- ≤48 KB UPX-packed Windows .exe (Constitution I hard limit; current 32 KB packed; budget 16 KB headroom)
- ≤30 KB UPX-packed Linux binary (Constitution I; added 2026-07-08 per v1.2.0 amendment; `Makefile size` warns above PACK_TARGET = 30720 per Makefile; closes the implicit-cap loophole pre-#121 Makefile `PACK_TARGET = 12288` was half of reality vs. the ~25 KB measured post-#117)
- ≤50 KB stripped Linux binary (Constitution I; bumped 2026-07-08 from prior ≤24 KB soft target per v1.1.0; `Makefile size` warns above STRIP_TARGET = 51200 per Makefile; principled ~19 KB cost of 003 MIDI-input chain is the documented reason for the raise)
- No `malloc` (Constitution Memory model; 256-event ring buffer from arena)
- No `time()` in any audio or MIDI path (Constitution III v1.0.1; FR-042)
- C99 dialect; gcc `__atomic_*` built-ins for SPSC ring buffer head pointer (C99-compatible gcc extension)
- ALSA: dynamic link via `-lasound` (mirrors existing `-lpulse` pattern)
- Linux CI runner: libasound2-dev is preinstalled on `ubuntu-latest`

**Scale/Scope**:
- ~4 new source files: `audio_midi.h`, `audio_midi.c`, `audio_midi_linux.c`, `audio_midi_winmm.c`
- ~1 new test file: `tests/unit/test_midi.c`
- ~30-50 lines added to `main.c` for CLI parsing
- ~1 new entry point on voice pool: `voice_pool_trigger_midi()` + small `trigger_key` / `trigger_channel` fields on `Voice`
- ~10 lines added to `Makefile` (new OBJS, `-lasound` link, smoke-test auto-skip)
- ~30 lines added to `tests/test_smoke_live.sh` (loopback step)
- Estimated total new code: ~1,100-1,500 lines (source + tests + smoke)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Note |
|---|---|---|
| I. Tiny Native Binary (NON-NEGOTIABLE) | ✅ PASS (estimated) | ALSA: dynamic link (no binary size impact); Win32: midiIn* via already-linked winmm. **Estimated add: ~3-5 KB stripped Linux; ~4-6 KB packed Windows** (**M2 fix** — original "<2 KB" estimate undercounted `audio_midi.c`'s dispatch weight), broken down as: `audio_midi.c` ~300 lines of dispatch (~2-3 KB stripped); `audio_midi_linux.c` ~150 lines ALSA (~1-1.5 KB stripped); `audio_midi_winmm.c` ~100 lines (~0.5-1 KB stripped); `voice.c` `*_midi` additions (~1 KB stripped); `main.c` CLI parsing (~0.5 KB stripped); static `CC_MAP[128]` table (256 B `.rodata`; amended 2026-07-12, 077 pack - see data-model Entity 4); `Voice` struct +2 fields (+22-44 B BSS for the 11-voice pool — magnitude negligible against the ~1 KB per-voice existing struct). Still well within the 50 KB (Linux, per Constitution v1.1.0 amendment) / 48 KB (Windows hard limit) budgets. Post-#117 actuals: linux_synth_stripped ~43 KB / 43 944 B (≈7 KB headroom under 50 KB per `make size` artifact); windows_stretto_exe_packed ~38 KB / 38 912 B (≈10 KB headroom under 48 KB per same artifact). Verify exact `make size` numbers in PR. |
| II. C99 Only | ✅ PASS | Pure C99 source; `__atomic_*` built-ins are gcc C99-compatible extensions; no C11/C23 features. libasound + winmm only as runtime deps. |
| **III. Deterministic (NON-NEGOTIABLE, v1.0.1)** | ✅ PASS | `--no-midi` is a zero-cost branch: no MIDI thread spawned, no ring buffer touched, no callback registered. Baseline 16-s SHA-256 golden continues to pass (FR-053). MIDI callback never reads `time()` (FR-042). |
| IV. Ambient + Algorithmic Aesthetic | ✅ PASS (N/A) | MIDI is an input control surface; doesn't alter the generative aesthetic. |
| V. Cleanly Modular | ✅ PASS | New module: `audio_midi.{c,h}` (cross-platform dispatch) + 2 platform backends. One-way deps: `audio_midi.c → {voice, effects, arena}`; `audio_midi_linux.c → audio_midi.c`; `audio_midi_winmm.c → audio_midi.c`. Mirrors the existing `audio_pulse` / `audio_winmm` split (Constitution V explicitly allows this shape). |
| VI. Test Discipline (NON-NEGOTIABLE) | ✅ PASS | New `tests/unit/test_midi.c`; per-file coverage ≥90% on `audio_midi.c` (FR-054). New smoke test step for virtual-MIDI loopback. `--no-midi` path verified byte-identical via existing `test_bitexact.sh` (FR-050 / FR-053). `audio_midi_linux.c` + `audio_midi_winmm.c` join `audio_pulse.c` in the INTERACTIVE set (require MIDI device or loopback) — excluded from coverage measurement with rationale. |
| VII. No Partial Features | ✅ PASS | Single PR ships: Note On/Off routing, CC mapping, device listing, channel filter, unit tests, smoke test. No `TODO` markers; no placeholder paths. |
| VIII. Document Why, Not What | ✅ PASS | This plan + research.md + data-model.md document the **why** (architectural decisions, tradeoff rationale); the **what** lives in the implementation PR comments + `audio_midi.h` doxygen-style comments. |
| IX. Cross-Platform From Day One | ✅ PASS | Linux + Windows backends both shipped in the single PR; same `midi_event_t` representation; same producer-side enqueue pattern; same `render_chunk` consumer (FR-030..FR-033). CI runs both Linux build + Windows cross-compile + UPX pack. |
| X. Generative > Random | ✅ N/A | MIDI is an input control surface, not a generative feature. |

## Project Structure

### Documentation (this feature)

```text
specs/003-midi-input/
├── spec.md              # /speckit-specify output (with 4 clarifications)
├── plan.md              # This file (/speckit-plan output)
├── research.md          # Phase 0 output (/speckit-plan output) — D1..D7 decisions
├── data-model.md        # Phase 1 output (/speckit-plan output) — midi_event_t, queue, cc_map, voice_pool ext
├── quickstart.md        # Phase 1 output (/speckit-plan output) — CLI + key map + build + platform notes
├── contracts/
│   └── cli.md           # Phase 1 output — exact argv grammar + exit codes + error messages
├── checklists/
│   └── requirements.md  # /speckit-specify output (no changes; already passed)
└── tasks.md             # /speckit-tasks output (NOT generated by /speckit-plan)
```

### Source Code (repository root)

```text
# New files for 003-midi-input:
audio_midi.h                  # Cross-platform interface: midi_event_t, midi_init/shutdown/drain/list_devices
audio_midi.c                  # Ring buffer + dispatch (Note/CC → voice_pool / live params)
audio_midi_linux.c            # ALSA sequencer backend: snd_seq_open + pthread_create worker looping on snd_seq_event_input + per-event parse → audio_midi_enqueue (preflight correction 2026-07-06)
audio_midi_winmm.c            # Win32 backend: midiInOpen + midiInProc callback + midiInGetDevCaps enumeration
tests/unit/test_midi.c        # Unit tests: scale-degree map, velocity scale, CC map, ring buffer, channel filter, --no-midi byte-identity

# Modified files for 003-midi-input:
main.c                        # CLI parsing: --midi [N], --midi-list-devices, --midi-channel N, --no-midi
voice.h                       # +1 entry point: voice_pool_trigger_midi(note, vel); +1 entry point: voice_pool_release_midi(key, channel); +2 fields on Voice: trigger_key, trigger_channel
voice.c                       # voice_pool_trigger_midi: non-role-scoped voice allocation with voice-stealing rules from spec Q1; voice_pool_release_midi: match by (key, channel) and request release
mixer.c                       # At the start of render_chunk, call audio_midi_drain() to dispatch queued events
Makefile                      # OBJS += audio_midi_linux.o; WIN_OBJS += audio_midi_winmm.win.o; synth link adds -lasound; synth_cov link adds -lasound; COV_SRCS_INTERACTIVE += audio_midi_linux.c
CLAUDE.md                     # Agent context pointer updated to reference this plan
tests/test_smoke_live.sh      # New step: open snd-seq-dummy loopback, send Note On via amidi, verify non-zero audio within 1s; auto-skip if libasound or snd-seq-dummy unavailable
```

**Structure Decision**: Single project (Option 1 in the template, which the Constitution implicitly assumes — Stretto is one binary, one Makefile, one coverage flow). No new top-level directory.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| (none) | All 10 principles PASS. | — |

The two new runtime dependencies (libasound on Linux, winmm-already-linked on Windows) are not Constitution violations: libasound joins the existing `-lpulse` dynamic-link pattern; winmm is already linked for `waveOut`. The dynamic-link choice keeps the binary within the 48 KB / 50 KB budget (was 24 KB / 48 KB pre-2026-07-08 amendment per Constitution v1.1.0); static linking libasound (~1 MB) would blow the budget.

## Amendment history

- **2026-07-12** — 077 size reclaim: `cc_map_entry_t` packed to 2 B/entry (`target` stored as `uint8_t`; all 11 `cc_target_t` values fit); `CC_MAP[128]` is 256 B `.rodata`, inside this plan's original 512 B estimate (the shipped int-enum layout had actually been 8 B/entry = 1024 B until the pack). Constitution-Check Principle I row figure refreshed to match.
- **2026-07-08** — Constitution v1.0.x → v1.1.0: Principle I Linux stripped binary budget bumped ≤24 KB → ≤50 KB per PR #116 (019-realign-strip-target). The Makefile `STRIP_TARGET` bumped 24576 → 51200 (~50 KB). The principled cost was the 003 MIDI-input chain's cross-platform ALSA + winmm sequencer workers + SPSC ring + CC dispatch, which measured ~19 KB stripped growth = ~43 KB post-#117 per PR #117 `binary-sizes` artifact = ~7 KB headroom under the 50 KB cap (within the Constitution Principle I 14-21 % headroom pattern, principled acceptable per the rationale of the amendment). This doc's ≤24 KB → ≤50 KB bullet refresh + trailing sentence "48 KB / 50 KB budget" + Constitution Check Principle I row's budget reference refresh are the catchup content. (PR #117's v1.1.0 spec-doc propagation did not refresh on disk for the active 003 spec; PR #124 / this PR closes the catchup.)

- **2026-07-08** — Constitution v1.1.0 → v1.2.0: Principle I Linux UPX-packed binary budget added ≤30 KB per PR #121. The current cap is ≤30 KB per `.specify/memory/constitution.md` Principle I amendment paragraph; Makefile `PACK_TARGET = 30720` enforces post-PR #121; PR #119 added a CI gate (`Binary size budget gate` step in `.github/workflows/ci.yml`) which hard-fails (exit 1) the workflow when `linux_synth_packed` exceeds PACK_TARGET. (Note: this file is the active 003 spec; PR #117's v1.1.0 amendment did not refresh on disk at landing time. The catchup landed in PR #124 / this PR — the ≤50 KB Linux stripped bullet above + the trailing sentence "48 KB / 50 KB budget" + the Constitution Check Principle I row's budget reference refresh are the catchup content; the original v1.1.0 disclosure is preserved here as historical audit trail.)
