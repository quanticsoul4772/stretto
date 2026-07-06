# Feature Specification: Stretto Baseline

**Feature Branch**: `001-stretto-baseline`

**Created**: 2026-05-23

**Last Updated**: 2026-05-24

**Status**: Active

**Input**: User description: "Spec what's been developed so we can evolve it."

## Overview

Stretto is a tiny native generative ambient music synthesizer. The listener launches it and hears continuous, deterministic-but-evolving music; optionally records a reproducible WAV file; optionally shapes the texture in real time via keyboard. This spec describes the capability shipped through ~90 PRs and serves as the living baseline for future evolution. It has been updated past the original ~80-PR baseline to cover the voice-architecture expansion (wavetable, additive, super-saw subtractive, portamento) and the per-section voice-palette work (chord voice type, arpeggio, voice-family masking).

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Listen to evolving generative music (Priority: P1)

The listener launches the synth and hears continuous music that develops over minutes without manual input. Each launch produces a different piece; the piece holds together as music rather than sounding like random noise.

**Why this priority**: This is the entire product. Without it nothing else matters.

**Independent Test**: Run `./synth` (Linux) or `stretto.exe` (Windows) with no arguments. Listen for at least 5 minutes. The piece should: (a) start playing within 1 second; (b) exhibit audible structure on multiple timescales (per-beat rhythm, per-bar chord motion, per-minute section changes); (c) not crash, drift to silence, or saturate to clipping.

**Acceptance Scenarios**:

1. **Given** the synth has been built, **When** launched with no arguments, **Then** audio begins playing within 1 second on a default PulseAudio (Linux) or WASAPI-default (Windows) output.
2. **Given** the synth is playing, **When** 10 minutes have elapsed, **Then** the listener perceives at least one full song-section cycle (intro → body → tension → resolve) and at least one recurring melodic motif.
3. **Given** the synth is playing, **When** the listener observes the terminal status row, **Then** they see live state (current scale, chord root, section name, density tension, filter parameters, motif capture/replay state).

---

### User Story 2 - Render reproducible audio (Priority: P2)

The user generates a WAV file of fixed length from a seed. Re-running the same seed produces a byte-identical file. Different seeds produce distinctly different pieces.

**Why this priority**: Reproducibility is the foundation for the test suite, for sharing interesting renders, and for future evolution (any change can be validated against the bit-exact regression).

**Independent Test**: `./synth --render 16 /tmp/a.wav --seed 0 && ./synth --render 16 /tmp/b.wav --seed 0 && cmp /tmp/a.wav /tmp/b.wav` — files MUST be byte-identical. `./synth --render 16 /tmp/c.wav --seed 1 && cmp /tmp/a.wav /tmp/c.wav` — files MUST differ.

**Acceptance Scenarios**:

1. **Given** any non-negative integer N, **When** the user runs `./synth --render <secs> <out.wav> --seed N` twice, **Then** the two output files are byte-identical.
2. **Given** two different seeds, **When** rendered with the same parameters, **Then** the resulting files differ (different sha256).
3. **Given** a render request, **When** `secs` is outside the range `[1, 3600]`, **Then** the synth exits with an error and a usage message.
4. **Given** a render request, **When** the output path cannot be opened for writing, **Then** the synth exits with an error referencing the path.

---

### User Story 3 - Shape the texture live (Priority: P3)

While listening, the user adjusts tempo, gate density, filter cutoff/resonance, scale, reverb/delay wet, and other parameters via single-keystroke commands. The status row and audio reflect the change immediately.

**Why this priority**: Listening is passive; live shaping turns the synth into an instrument. The live-mode key handler exists today and is tested via `test_keys.c`.

**Independent Test**: Launch `./synth` (no `--no-ui`). Press `?` — help overlay appears. Press any other key — overlay dismisses. Press `+`, observe tempo speed up (status row `M:` advances faster). Press `r`/`R`, observe `R:` decrease/increase. Press `q` — synth shuts down cleanly within 1 second.

**Acceptance Scenarios**:

1. **Given** the synth is playing in live mode, **When** the user presses `+` or `-`, **Then** the tempo (samples per substep) decreases or increases respectively, and the status row's parameter values continue updating at the new rate.
2. **Given** the synth is playing, **When** the user presses `c` / `C`, `n` / `N`, `m` / `M`, **Then** the filter cutoff, resonance, and LFO depth update by their documented step size and clamp at the documented bounds.
3. **Given** the synth is playing, **When** the user presses `s`, **Then** the active scale cycles to the next of D / L / P / l / H / M and subsequent triggers use the new degree-to-MIDI mapping.
4. **Given** the synth is playing, **When** the user presses `q`, **Then** the audio stream drains, the terminal restores to cooked mode, and the process exits 0.
5. **Given** the user launched with `--no-ui`, **When** the synth runs, **Then** no terminal setup is attempted (so the program can run in scripts / CI without a TTY).

---

### Edge Cases

- **OOM in the arena**: arena_alloc exits with code 1 and an error to stderr — programmer error, not a runtime condition. Test coverage gate is met.
- **Active scale-degree mask collapses to zero**: gen.c forces the mask to `0x01` (tonic only) so no voice starves.
- **Cellular automaton collapses to all-zero**: `ca_row` is reseeded to `0x12345678`, `ca_harm` to `0x87654321`.
- **Multi-seed render with seed `0` and seed `1`**: distinct outputs (a previous bug where the hash function collapsed both to identity is fixed and regression-tested).
- **`--no-ui` invocation under script / no-TTY**: `tcgetattr` is not called; the synth runs the audio loop until SIGTERM / SIGINT (test_smoke_live verifies).
- **`make coverage` followed by `make test-unit`**: works without `make clean` thanks to `build_cov/` isolation.

## Requirements *(mandatory)*

### Functional Requirements

#### Audio engine
- **FR-001**: System MUST maintain a pool of 11 voice slots: 2 bass, 3 chord, 3 melody, 3 drum (kick / snare / hihat). Synthesis method per role: bass = super-saw subtractive (VOICE_SUB); chord = section-selected wavetable / additive / FM (see FR-014); melody = Karplus-Strong + FM alternating; drum = synthesized kick / snare / hihat. The number of *audible* families per bar is gated by the active section's voice mask (see FR-017), so fewer than 11 slots may sound at once (notably in INTRO).
- **FR-002**: System MUST stream interleaved 16-bit stereo PCM at 48 000 Hz to the selected output (PulseAudio on Linux, Win32 waveOut on Windows).
- **FR-003**: System MUST apply, in order: per-voice ADSR + SVF filter → voice mix → Schroeder reverb → stereo delay → soft cubic saturation → feed-forward compressor → brickwall limiter (the compressor + limiter stage is specified in detail by `specs/002-master-limiter-compressor`).
- **FR-004**: System MUST clamp every output sample to int16 range via the shared `sat16()` helper.

#### Voice synthesis methods
- **FR-005**: Karplus-Strong (VOICE_KS) MUST pluck a noise-seeded delay line with an averaging-filter feedback for melody timbres.
- **FR-006**: 2-op FM (VOICE_FM) MUST synthesize a carrier modulated by one operator at a per-role carrier:modulator ratio, with the pan LFO detuning both operators together for slow chorus motion.
- **FR-007**: Wavetable (VOICE_WT) MUST read from a build-time table of 8 morphed single-cycle waveforms (256 samples each), linearly interpolating between adjacent waveforms at a "position" swept by the per-voice pan LFO for an animated pad.
- **FR-008**: Additive (VOICE_ADD) MUST sum 8 sinusoidal partials at integer multiples of the fundamental, each weighted by one of several drawbar-style amplitude profiles, for organ / strings / brass character.
- **FR-009**: Subtractive super-saw (VOICE_SUB) MUST sum 3 detuned band-limited saw oscillators (≈±0.78 % detune, reusing the wavetable saw) and feed the result through the existing per-voice SVF.
- **FR-009a**: Portamento (glide) — when a SUB bass note re-triggers while the previous note's amplitude envelope is still above half the sustain level, the oscillator increment MUST slide linearly to the new note over ~50 ms instead of jumping, preserving phases and envelope (legato). Determinism is preserved (no extra PRNG draws).

#### Generative state
- **FR-010**: System MUST advance one of six modal scales (Dorian / Lydian / Phrygian / Locrian / Harmonic Minor / Mixolydian) at the user's command and never auto-rotate scale.
- **FR-011**: System MUST drive the main melody by an L-system grammar over 6 symbols (`u U d D r .`), with 3 hand-tuned characters selectable by mutation.
- **FR-012**: System MUST drive the counter-melody by a 2nd-order Markov chain (table indexed by previous two degrees), biased away from unison with the most recent main-melody degree and toward 3rd/6th consonances.
- **FR-013**: System MUST advance a chord-progression Markov chain over the 7 scale-degree functions every 2 bars; bass plays root and fifth of the current chord, with a one-step diatonic approach at every chord change.
- **FR-014**: System MUST cycle through 4 song sections (intro / body / tension / resolve) on a fixed 96-bar period, crossfading gate / cutoff / reverb / mutation-interval biases over 8 bars at each boundary and pinning per section: kick pattern, L-system character, chord voice type (INTRO = wavetable, BODY = additive, TENSION = FM, RESOLVE = wavetable), and chord playback mode (TENSION = arpeggio, others = block chords).
- **FR-015**: System MUST capture the last 8 four-bar main-melody phrases into a ring buffer and, every 30+ bars with ~25% probability, replay one (verbatim or ±2 diatonic degrees transposed) instead of generating new material.
- **FR-016**: System MUST drift a single Markov-table cell, one CA bit, Euclidean k values, gate, filter, and L-system character at a mutation interval that varies (1–16 bars) under a 128-bar LFO plus section bias.
- **FR-017**: System MUST gate which voice families play per section via a voice-family mask. BODY and TENSION play the full ensemble; RESOLVE is drumless (no kick/snare/hihat); INTRO plays a randomized 1–3-voice subset chosen once per 96-bar cycle from a curated set of 8 combos. The INTRO combo selection MUST be seed-deterministic. Masking only silences trigger output — it MUST NOT alter the generative trajectory (PRNG / L-system / Markov / motif state advance identically whether or not a family is audible).

#### Determinism
- **FR-020**: Given `--seed N`, the system MUST produce byte-identical output across runs and across the supported build targets (Linux glibc + Windows winmm, both little-endian x86).
- **FR-021**: Without `--seed`, the system MUST derive its seed from `time(NULL)` at startup so each launch is a different piece.
- **FR-022**: No generative module MAY read the wall clock during audio generation.

#### CLI surface
- **FR-030**: System MUST accept `--seed N` (unsigned integer; must be parseable in full or exit with an error).
- **FR-031**: System MUST accept `--render <seconds> <out.wav>` where seconds is `[1, 3600]`.
- **FR-032**: System MUST accept `--no-ui` to suppress all terminal interaction.
- **FR-033**: When `--render` is absent, the system MUST enter live-audio mode.
- **FR-034**: System MUST exit with usage on any unrecognized argument combination.

#### Live-mode UI
- **FR-040**: System MUST render an oscilloscope + status row using ANSI escapes at every audio buffer boundary unless `--no-ui` is set.
- **FR-041**: System MUST respond to the documented key map (`?`, `q`, `+`/`-`, `[`/`]`, `s`, `g`/`G`, `d`/`D`, `f`/`F`, `r`/`R`, `c`/`C`, `n`/`N`, `m`/`M`, `t`) by invoking the corresponding setter and reflecting the change in subsequent audio + status row.
- **FR-042**: System MUST gracefully restore terminal state on exit (cooked mode, cursor visible).

#### Memory + budget
- **FR-050**: System MUST allocate all dynamic-sized buffers from a single 128 KB static arena via `arena_alloc`. No use of `malloc` or `free`.
- **FR-051**: System MUST package its Windows binary to ≤48 KB via UPX `--ultra-brute` (CI-enforced budget; last measured packed ~37 KB).
- **FR-052**: System SHOULD strip its Linux binary toward a 24 KB soft target (Makefile `size` warns past it; not CI-failing). The libpulse-linked binary is currently ~40 KB.

#### Testing
- **FR-060**: System MUST pass a bit-exact 16-second SHA-256 regression against `golden/regression_16s.sha256`.
- **FR-061**: System MUST pass a 4-seed multi-render integration that checks each output is deterministic, all four hashes are distinct, and per-render audio characteristics fall within bounds: peak in [500, 32767), clip count ≤ 100, at least half the samples non-silent, spectral centroid in [100, 5000] Hz, and zero-crossing rate in [0.01, 0.30]. (RMS is measured and reported but not hard-gated, since the randomized INTRO palette legitimately varies loudness.)
- **FR-062**: System MUST pass a live-mode smoke test that spawns under a 2-second timeout and exits with code 0, 124, or 143 (no segfault).
- **FR-063**: System MUST maintain per-module line-coverage gates on each measured pure-synth module (per Constitution Principle VI): 95 % for arena / effects / voice / section / density / motif / mixer; 90 % for gen / lsystem / chord_progression / wav / main.

### Key Entities

- **Voice**: One of 11 slot-allocated audio sources (bass / chord / melody / drum). Holds type tag (KS / FM / DRUM / WT / ADD / SUB), envelope phase, SVF state, peak normalizer gain, pan, LFO phase, glide state (`inc_target` / `glide_remain` for SUB portamento), and a per-type union of synthesis state. Triggered by `voice_pool_trigger_role` or `voice_pool_trigger_drum`. Mixed by `voice_pool_mix`.

- **Scale**: One of six 7-tone modal tables in `SCALES[6][7]`, mapping degree index (0..6) to MIDI note. Selected by `cur_scale`; cycled by user.

- **Chord function**: A scale-degree-function root (0..6) advancing through a Markov chain every 2 bars. Bass + chord voicings rebase onto this root.

- **Active mask**: A 7-bit field combining `ca_row` and `ca_harm` per substep; allowed scale degrees for the current beat. Forced non-empty.

- **Section**: One of four (INTRO / BODY / TENSION / RESOLVE), advanced per bar, biasing gate / cutoff / reverb / mutation-interval and pinning kick pattern, L-system character, chord voice type, chord playback mode (block / arpeggio), and a voice-family mask (which families are audible; INTRO's mask is a per-cycle randomized 1–3-voice combo).

- **Motif phrase**: A 64-slot array of degrees (with `MOTIF_NO_NOTE` for empty positions), capturing one 4-bar main-melody phrase. Ring buffer of 8.

- **Render request**: A `(seconds, path)` pair invoked via `--render`. Produces a 16-bit stereo PCM WAV with a RIFF header.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Listener launches `./synth` and hears continuous, evolving music within 1 second; the music does not silence, crash, or saturate within 30 minutes of unattended runtime.
- **SC-002**: Two `--seed N` renders of equal length produce byte-identical sha256 hashes.
- **SC-003**: A 4-second render's peak amplitude is between 500 and 32 767 (exclusive of full-scale), clip count is below 100 samples, spectral centroid is within [100, 5000] Hz, and zero-crossing rate is within [0.01, 0.30]. (RMS varies by INTRO palette — masked intros measure ~500–700, full sections ~1000–1300 — and is reported rather than gated.)
- **SC-004**: The packed Windows binary is ≤48 KB on every CI run (last measured ~37 KB; headroom maintained for future features).
- **SC-005**: 100 % of merged PRs pass CI's bit-exact regression, multi-seed integration, smoke test, per-file coverage gates, and Windows cross-compile + UPX pack.
- **SC-006**: A new listener who plays `./synth` for at least 10 minutes can identify at least one section change (audibly) and at least one repeated melodic phrase (audibly) without instruction.
- **SC-007**: Live-mode key responses are perceptible within one buffer (~21 ms) of the keypress.

## Assumptions

- Linux PulseAudio is reachable on the listener's machine for live mode (or `--no-ui` + headless capture is used).
- Modern terminal supports ANSI escapes (Linux native; Windows 10+ via `ENABLE_VIRTUAL_TERMINAL_PROCESSING`).
- gcc, GNU Make, UPX, and (on Linux) libpulse-dev are available for building.
- The listener's playback device runs at 48 kHz natively, or resamples without dropouts.
- WSL2 audio is best-effort; the Windows native binary is the supported path for Windows users.
- Stretto remains a single-binary, single-process program. No daemon, no plugin host, no shared library form.

## Out of Scope

The following are explicitly outside the baseline. Future specs may add any of them:

- MIDI input or output.
- Real-time spectral analysis / FFT.
- VST / AU / LV2 plugin form factor.
- Microphone input as a generative control source.
- Neural-network priors for note selection.
- Microtonal / non-12-EDO tuning systems.
- Save / load of "preset" snapshots beyond what `--seed N` reproduces.
- A graphical (non-terminal) UI.
- Multi-track mix-down or stem export beyond the existing stereo WAV writer.
