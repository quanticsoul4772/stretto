# Phase 0 — Research Synthesis: Stretto Baseline

**Branch**: `001-stretto-baseline` | **Date**: 2026-07-06 | **Spec**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md)

This document captures the architectural decisions that produced the shipped Stretto synth. Each entry is locked to a tested code path, not speculative; any future change must either preserve the existing decision or justify a deviation through a `docs/*` amendment and a regenerated `golden/`.

## 1. Synthesis methods

### Decision: Karplus-Strong (KS), 2-op FM, Wavetable (WT), Additive (ADD), Subtractive super-saw (SUB), and Drum synthesis.

**Rationale**: Each method covers a distinct timbral family — KS for plucked melody, FM for bell-like / percussive melody hits, WT for animated pad, ADD for organ/strings/brass chord stacks, SUB for fat bass. Drum synthesis is a separate role with its own envelope shape. Together they let the six generative layers express enough variation without exceeding the 11-voice budget.

**Alternatives considered**: Wavetable scanning + LFO morphing was added later as the WT method because single-cycle WAV buffers at 48 kHz × 256 samples × 8 morphs cost ~4 KB in `.rodata` and add no runtime cost beyond a linear interp lookup.

### Reference: `voice.h` (enum `VOICE_KS | VOICE_FM | VOICE_WT | VOICE_ADD | VOICE_SUB | VOICE_DRUM`).

## 2. Generative state machine

### Decision: Six interacting layers — (1) modal-scale degree table, (2) 2nd-order Markov counter-melody, (3) chord-progression Markov over 7 functions every 2 bars, (4) cellular-automaton 7-bit degree mask per substep, (5) L-system over `u U d D r .` with 3 hand-tuned "characters," (6) motif ring buffer of 8 × 4-bar phrases with ±2 diatonic transposition.

**Rationale**: Each layer contributes a different timescale — per-substep (CA), per-beat (L-system), per-2-bar (chord), per-minute (motif recall, section). The interaction is what pushes the output away from "random notes" toward "intentional music" (Constitution IV, X). The 4-section crossfader (intro/body/tension/resolve) then pins which L-system character + chord voice type + chord playback mode (block/arpeggio) and applies an 8-bar per-section bias to random progressions.

**Alternatives considered**: A single pure-random Markov was rejected as failing Principle X (Generative > Random). Cellular automaton was added because it produced noticeably less grid-locked rhythms than Euclidean kicked baseline.

### Reference: `gen.h`, `lsystem.h`, `chord_progression.h`, `motif.h`, `density.h`.

## 3. Audio sample rate + format

### Decision: 48 kHz × 2-channel × 16-bit interleaved PCM, RIFF/WAVE writer.

**Rationale**: 48 kHz is the rate consumers (PulseAudio defaults; Windows waveOut) commonly run natively; 16-bit signed matches `int16_t` directly so no floating-point → fixed-point conversion is needed (keeps the binary tiny). Stereo is mandatory for the per-voice pan LFO and stereo delay.

### Reference: `audio.h`, `wav.c::wav_open` / `wav_write`.

## 4. Memory model

### Decision: Single 128 KB static arena (`arena.c`), 8-byte aligned, no `free`.

**Rationale**: All audio buffers + voice pool + reverb/delay state allocate from the arena at startup. No `malloc()` after `gen_init()` returns. OOM is treated as a programmer error and exits the process (unit-tested). Per-module constant state (Markov tables, ring buffers, wavetable, scale tables) lives in `.bss`/`.rodata`, never on the arena.

### Reference: `arena.h::arena_alloc`, `arena_used`.

## 5. Determinism

### Decision: Single `lcg_next()` PRNG shared by all generative modules; `--seed N` initializes it at startup. No `time()` reads in the audio path.

**Rationale**: A single source of randomness is the only way to guarantee bit-exact reproducibility across runs. The CLI's `--seed N` is parsed strictly (any non-digit exits with usage). Forgetting `--seed` falls back to `time(NULL)` so each launch is a different piece (FR-021). The bit-exact 16-s SHA-256 regression test (`golden/regression_16s.sha256`) gates every PR.

**Alternatives considered**: A Mersenne Twister was rejected — too many bytes of state to maintain bit-exact across platforms. A per-module seedable RNG + global reseed protocol was rejected as over-engineered for a single-process synth.

### Reference: `gen.h::lcg_next`, `gen.h::gen_init`, `tests/test_bitexact.sh`.

## 6. Effects chain ordering

### Decision: per-voice ADSR + SVF → voice mix → Schroeder reverb → stereo delay → soft cubic saturation → feed-forward compressor → brickwall limiter → `sat16()`.

**Rationale**: Saturate-then-compress-then-limit is the established electronic-music chain. The compressor + limiter together prevent clipping on dense sections while preserving the soft-saturator's harmonic texture on quieter passages. The shared `sat16()` is a one-line final clamp covering any micro-clipping that escaped the limiter. Detailed compressor/limiter specification is referenced by `specs/002-master-limiter-compressor`.

### Reference: `effects.c::effects_chain`, `mixer.c::mixer_run`.

## 7. Cross-platform audio

### Decision: `audio_pulse.c` for Linux (libpulse) + `audio_winmm.c` for Windows (winmm). Same `audio.h` surface.

**Rationale**: Both code paths implement the same callback shape so the synth engine is platform-independent. CI runs both on every PR. `--no-ui` case drops terminal setup entirely so the synth can run under CI / headless capture. The `ENABLE_VIRTUAL_TERMINAL_PROCESSING` path on Windows 10+ is one `tcgetattr`-equivalent line in `ui.c` behind `#ifdef _WIN32`.

### Reference: `audio.h::audio_open / audio_close / audio_write_cb`.

## 8. Section state machine

### Decision: 4 sections (`INTRO / BODY / TENSION / RESOLVE`) on a fixed 96-bar period; 8-bar crossfade window for gate / cutoff / reverb / mutation-interval biases; per-section pinning of kick pattern, L-system character, chord voice type, chord playback mode (TENSION = arpeggio, others = block), and voice-family mask.

**Rationale**: The 96-bar period + 8-bar crossfade gives a perceivable song arc within a 4–6 minute listen, satisfying SC-006 (a new listener can identify a section change audibly within 10 min). The 1–3-voice randomized INTRO palette (FR-017) keeps the opening sparse enough that the BODY section lands with perceptible density arrival.

### Reference: `section.h::section_advance`, `section.h::section_apply_mask`.

## 9. Live-mode UI

### Decision: ANSI oscilloscope + status row, redrawn at every audio buffer boundary. Single-keystroke key map (`? q + - [ ] s g G d D f F r R c C n N m M t`).

**Rationale**: ANSI escapes work natively on Linux terminals and (with `ENABLE_VIRTUAL_TERMINAL_PROCESSING`) on Windows 10+. A redraw-per-buffer update interval (~21 ms) is below the perceptual latency threshold for parameter changes (SC-007). Single-keystroke dispatch avoids a readline-style dependency that would bloat the binary.

### Reference: `ui.h::ui_render`, `keys.h::keys_poll`.

## 10. Voice-family mask + INTRO randomness

### Decision: Section-level 7-bit active-family mask. INTRO = one of 8 curated combos (each 1–3 voices) randomized once per 96-bar cycle. Masking MUST NOT alter PRNG/L-system/Markov/motif state advance.

**Rationale**: Per-section voice masking lets the listener perceive the song arc as a population change, not just a parameter change. Crucially, "no PRNG draw leak when silenced" preserves determinism — a re-render with the same seed produces the same mask + the same musical state, just with some voices routed to zero.

### Reference: `section.h::section_apply_mask`, `gen.c::gen_init_intro_combo`.

## 11. Coverage gates

### Decision: Per-file coverage in CI: 95% for `arena / effects / voice / section / density / motif / mixer`; 90% for `gen / lsystem / chord_progression / wav / main`. Interactive modules (`ui.c`, `keys.c`, `audio_pulse.c`) measured-only with rationale.

**Rationale**: The gate is calibrated to the highest level where it doesn't push coders into hiding meaningful branches behind trivial lines. The interactive-module exclusion is documented in CI (a comment in the workflow) because those modules' branch coverage requires a real audio device + TTY.

### Reference: `.github/workflows/ci.yml`, `Makefile::coverage`.

## 12. Tests not generated

### Decision (negative): No fuzz tests, no property-based tests, no perf-benchmark tests in CI.

**Rationale**: Fuzz testing on a deterministic synth would by definition not find new bugs each run (the seed controls what's reachable). Property-based tests would have to be re-encoded against the seed system. Perf-benchmarks would only catch regressions that the size budget already catches indirectly. SC-005 / FR-060..063 already cover what fuzz/property tests would have added without the cost.
