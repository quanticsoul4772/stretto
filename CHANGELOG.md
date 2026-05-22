# Changelog

## Recent: probabilistic chord progressions

- New `chord_progression.c` / `.h` module. Chord function root advances every 2 bars via a Markov chain. All chord triggers within those 2 bars share the same root, so harmonic motion happens at a slow ambient pace rather than per-bar churn.
- Two 7x7 weight tables (~98 B `.rodata`):
  - `CHORD_MARKOV_MAJOR` for Lydian / Mixolydian - biased toward V to I, IV to I, vii to I, ii to V.
  - `CHORD_MARKOV_MINOR` for Dorian / Phrygian / Locrian / Harmonic Minor - modal-friendly VII-i, iv-i, no strict dominant pull.
- Chord trigger rebases each voicing degree: `(pattern[i].degree + current_root) % 7`. Voice-leading octave-shift unchanged.
- Bass tracks the current chord root - its root/fifth alternation now follows the chord function rather than always playing scale degree 0/4.
- One-way coupling: gen.c passes `prng()` output and `cur_scale` to `chord_progression_step()`. The module never reads gen.c state.
- Status row gains `Cr:<n>` field (current chord root 0..6).
- 5 new unit tests in `tests/unit/test_chord_progression.c`. Coverage on the new module: 91.3%.

## Recent: L-system melodic phrase generator

- New `lsystem.c` / `.h` module replacing the Markov walker on the main melody. Counter-melody keeps Markov so the two lines contrast (phrased vs walked).
- 6-symbol alphabet (u, U, d, D, r, .) over scale-degree relative moves + rests.
- 3 hand-tuned characters (stepwise, leaping, sparse) selected by mutation. Each character has a 6-rule production table.
- 3-generation rewrite into a 256-byte output buffer per `lsystem_reset()`. Walker reads sequentially, snaps each pointer position to the nearest in-mask degree, returns `LSYSTEM_REST` for the rest symbol so the caller skips the trigger (breathing).
- `mutate()` calls `lsystem_mutate()` with ~33% probability per event. Mutation re-rolls one rule's RHS (50%), cycles character (25%), or swaps an axiom symbol (25%) - drift the melodic style across the piece.
- Memory cost: ~410 B static state.
- 6 new unit tests in `tests/unit/test_lsystem.c`. Coverage on the new module: 93.1%.

## Recent: testing + CI

- Hand-rolled unit-test framework at `tests/unit/test.h` (~130 LOC, `TEST(name) {...}` registration via constructor attributes plus assertion macros).
- 49 unit tests across `tests/unit/test_arena.c`, `test_voice.c`, `test_gen.c`. Coverage: `arena.c` ~80%, `voice.c` ~98%, `gen.c` ~97%.
- `tests/test_multi_seed.sh` renders 4 seeds, asserts determinism, distinct sha256s, audio characteristics within bounds, plus golden-hash regression in `golden/regression_multiseed.sha256.txt`.
- `tests/test_smoke_live.sh` spawns `./synth --no-ui` under a 2 s timeout, accepts clean exit / SIGTERM, fails on segfault, auto-skips without PulseAudio.
- New Makefile targets: `test-unit`, `test-multiseed`, `test-smoke`, `coverage`, `golden-multiseed`.
- GitHub Actions CI (`.github/workflows/ci.yml`) runs the full suite on push and pull-request to `main`, plus Windows cross-compile. Uploads `stretto.packed.exe` and coverage log as build artifacts. Per-file coverage gate at 80%.

Two bug fixes uncovered by the new tests:

- `gen_seed(0)` and `gen_seed(1)` produced identical state. The old ternary `hash32(seed ? seed : 1u)` collapsed both to `hash32(1)`. Fix: XOR the seed with `0xDEADBEEFu` before hashing so all seeds map to distinct chains.
- `tcgetattr` exited synth with code 1 when stdin had no TTY (script invocation, smoke test). `--no-ui` now skips `term_raw_mode` entirely; live mode without TTY runs cleanly until SIGTERM.

## Recent: cleanup pass

- Removed dead `drum_attack` / `drum_release` placeholder arrays in `voice_pool_trigger_drum` that were marked `(void)` for-future. The actual per-drum-type release table is in `env_step`; per-drum attack is just `role_attack[ROLE_DRUM]`.
- Pruned Makefile `clean` target of legacy experiment artifacts (synth.upx, synth.test, synth.orig, synth.unpacked, synth.xz, synth.lto.o, synth_xz.h, start.c, stub.c) that haven't existed for many PRs.
- Reorganized `.gitignore` into labeled groups; added Windows artifacts (stretto.exe, stretto.packed.exe, *.win.o) that were previously showing as untracked.

## Recent: filter controls

- Runtime cutoff and resonance, live-tunable via `c`/`C` and `n`/`N`.
- Per-role cutoff/Q offset tables (bass darker, melody open, drums heavily damped).
- Per-voice cutoff LFO modulation, reusing the existing per-voice pan LFO at zero new state. Depth adjustable live via `m`/`M`.
- Multi-mode filter: LP / HP / BP / notch, cycled via `t`. SVF natively computes all four outputs; `voice_step` selects.
- Chord-voice filter envelope: separate ADSR feeds cutoff modulation only for chord voices. Each chord trigger sweeps the filter open then closed.
- `mutate()` drifts cutoff and resonance ~50% of the time it fires, so the timbre evolves over long timescales.
- Status row gains `F:<cutoff> N:<resonance> L:<lfo_depth> T:<mode>` fields.
- User base ranges deliberately tighter than effective-value clamps so LFO + filter-envelope modulation always lands without losing swing at the top of the dial:
  - `svf_f_base`: user [30, 180], effective clamp [20, 230].
  - `svf_q_base`: user [0, 180], effective clamp [0, 220].

## Recent main-branch work (since stereo)

### Windows port
- Native Windows binary `stretto.exe` via MinGW cross-compile (`make win`). No WSL involved at runtime.
- Win32 `waveOut` audio backend on Windows (4 cycling buffers of 1024 frames, CALLBACK_EVENT, links `winmm.lib`). Bypasses WSLg's broken RDP audio pipe entirely.
- Platform-abstracted terminal layer (`term_get_size` / `term_read_key` / `term_raw_mode` / `term_restore_mode`) so the oscilloscope + status row + key handler work the same on Linux and Windows. Windows uses `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING` for ANSI escapes and `_kbhit`/`_getch` for non-blocking key input.
- Size-optimized Windows build: 529 KB → 30 KB packed via `-Os -flto + section gc-sections + strip + UPX`. `make winpack` packs in one step.

### Audio quality
- 48 kHz native sample rate (was 44.1 kHz). WSL/PulseAudio's 44.1 → 48 resampler was broken on the user's machine; switching native eliminates that path. All sample-count constants (envelope timings, reverb delays, samples per substep, LFO increments) rescaled to preserve identical musical timing at the new rate.
- Stereo output with per-voice panning. Voices have role-based base pan positions plus a slow per-voice LFO for continuous motion in the stereo field. Linear pan law, applied at mix time.
- Per-voice peak normalization. 50 ms measurement window after each trigger; gain scales the voice output toward a common peak target (16000) with a 4× cap. Equalizes perceived loudness between bass (FM 1:1, soft) and chord (FM 2:1, bright).
- LFO pitch detune on FM voices (~5 cents peak chorus). Reuses the existing pan LFO at zero state cost.
- Master-bus stereo delay (250 ms, two independent buffers per channel) with feedback (default ~0.55).
- Master-bus Schroeder reverb. 4 parallel comb filters → 2 series all-pass per channel, with prime delays slightly different L/R so the tail keeps stereo separation. RT60 ~1.5 s.
- Soft cubic saturation `y = x - x^3 / 2^31` on the master bus. Transparent at typical levels; smoothly compresses peaks.

### Generative
- Time-seeded PRNG by default — every launch sounds different. `--seed N` overrides for reproducible runs (used by the regression test).
- Dynamic mutation rate. Triangle LFO sweeps the interval between 1 bar (busy) and 16 bars (calm) over a ~4.3-min cycle so the piece alternates between dense and sparse sections.
- True 3-against-4 polyrhythm via 48-substep bar (was 16-step). Bass 3 events vs chord 4 events fit cleanly. LCM(3, 4, 16) = 48.
- Bouncing bass — 4 events per bar at substeps 0, 18, 24, 42 (long-short alternating). Beats 1 and 3 anchor tempo; offbeats anticipate. Alternates root/fifth with bar parity swap.
- Voice leading on chord triggers. Each new chord pitch octave-shifts toward the running chord centroid for stepwise motion instead of leaps.
- Counter-melody: second melodic line on its own Euclidean rhythm + Markov walk, transposed +12 semitones to occupy a different register.
- Chord voicing variety: triad / seventh / sus4 / sus2 / inv1 / inv2 rotating per bar.
- Probability gates on Euclidean hits. `gate_prob` (default 200/256) drops some hits for melodic breath; drifts via mutation; live-tunable with `g`/`G`.

### Scales
- Six modes: D Dorian, D Lydian, D Phrygian, D Locrian, D Harmonic Minor, D Mixolydian. The `s` key cycles between them in live mode. Scale never changes automatically. Status row shows current scale.

### Drums
- 11-voice pool (was 8): added kick (slot 8), snare (9), hihat (10) as new `VOICE_DRUM` type with `ROLE_DRUM`.
- Synthesis: kick is a sine pitch-sweep + 5 ms noise attack click; snare is noise-dominant (90/10) with a 200 Hz body; hihat is pure white noise. Per-drum-type linear envelope releases (kick 150 ms / snare 100 ms / hihat 30 ms).
- Rotating pattern banks: kick 4 patterns, snare 3, hihat 5. Coprime sizes → LCM(4, 3, 5) = 60 bars before exact repeat.
- Per-drum-type post-normalization gain (kick 3×, snare 2.5×, hihat 1.5×) so drums sit on top of the harmonic mix.

### UI
- Color heat-map oscilloscope (silence dim gray → blue → cyan → green → yellow → magenta → red peak), single `write()` syscall per frame.
- Expanded status row: M (mod_depth), S (scale), V (11 activity dots colored by role), G (gate), R (reverb wet), D (delay wet/feedback), deg (Markov walk position), act (active scale-degree mask), chord (current voicing name).
- Help screen toggled by `?` listing all live keys.

### Polish
- ALSA → libpulse-simple → pa_stream + threaded mainloop on Linux. Final libpulse architecture matches paplay's exactly.
- ALSA latency bumped to 300 ms while the WSLg crackling investigation was ongoing; retained as the PulseAudio buffer target.

## Pre-stereo work (recent main-branch)

### Code quality / cleanup
- Removed dead `voice_pool_trigger` (non-role variant); only the role-aware API is used.
- Inline rationale comments at four points that previously required reading commit history: SVF int32 widening, `MUTATE_BARS` deviation from PLAN.md, Markov weights, Rule 110 + Rule 30 pairing.
- ALSA underrun recovery via `snd_pcm_recover` (later replaced when moving to libpulse, then to waveOut on Windows).
- `--render <seconds>` input validation via `strtol`.
- Makefile housekeeping: `STRIP_TARGET` / `PACK_TARGET`, `UPX_BIN` / `UPX_FLAGS`, `make clean` removes abandoned experiment artifacts.

### Earlier synth features
- True Bjorklund Euclidean rhythm masks replace the floor-distributed approximation.
- Per-voice roles: bass / chord / melody with role-scoped envelope and synth parameters.

### Earlier audio fixes
- SVF state widened `int16_t` → `int32_t` with int16 saturation at output. Eliminated resonance-wrap clicks (395 → 0 per 16 s render).
- Default FM `mod_depth` reduced 6000 → 1500 to cut aliasing (HF energy >8 kHz dropped 45% → 12%).

## Phase 5 (PR #4)

- Terminal UI: ASCII oscilloscope, raw stdin via termios, atexit restore.
- Live keyboard controls: SPACE (force mutate), `+`/`-` (tempo ±10%), `q` (quit).

## Phase 4 (PR #3 series)

- UPX `--ultra-brute` packing; binary fits demoscene-tier sizes.
- Direct ioctl path on `/dev/snd/pcmC0D0p` explored, then reverted to libasound for WSL compatibility (PR #6).

## Phase 3 (PR #2)

- Generative MVP per PLAN.md section I:
  - Rule 110 CA evolves active-degree mask per bar.
  - First-order Markov chain over 7 D-Dorian degrees.
  - Two parallel Euclidean rhythm masks combined for melody triggers.
  - Mutation every 4 bars (deviation from PLAN.md's 16; documented).
- Build adds `gen_euclid_table.c` as a fourth build-time generator.

## Phase 2 (PR #1)

- Static `pool[65536]` arena with 8-byte-aligned bump allocator.
- Voice struct unioning Karplus-Strong and 2-op FM variants. ADSR per voice.
- 4-voice polyphony, hard-coded C-major arpeggio for testing.
- Build-time generators for envelope curve and per-MIDI-note tables.

## Phase 1 (PR #1)

- Hard-coded 440 Hz sine via 1024-entry int16 LUT.
- ALSA `snd_pcm_writei` live playback at 44.1 kHz S16_LE mono.
- `--render <seconds> <out.wav>` mode.
- Bit-exact regression test (`tests/test_bitexact.sh`, `golden/regression_16s.sha256`).

## Initial commit

- `PLAN.md` design document and project skeleton.
