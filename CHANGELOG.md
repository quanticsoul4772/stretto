# Changelog

## Unreleased

- Realtime `mod_depth` keys (`[` / `]`) added to live mode.
- Status row added at top of live terminal output: `M:<mod_depth> V:<8 voice activity dots>`.
- Oscilloscope amplitude thresholds rescaled from 10000..30000 down to 1000..7000 to match actual signal range.
- SVF state widened from `int16_t` to `int32_t` with int16 saturation at output. Removes click events caused by resonance wrapping the filter state (measured: 395 clicks per 16 s → 0).
- Default FM `mod_depth` reduced from 6000 to 1500. Cuts FM aliasing; high-frequency energy dropped from 45% to ~12% above 8 kHz.
- README.md, ARCHITECTURE.md, CHANGELOG.md added.

## fix-wsl-libasound (unmerged)

- Reverted Phase 4-finish's direct ioctl path back to libasound. The ioctl path required `/dev/snd/pcmC0D0p`, which WSL does not expose. Live audio now routes through libasound → pulse plugin → WSLg on WSL, and through libasound → ALSA hardware on native Linux.
- Threshold (c) "no libasound" from the original PLAN.md Phase 4 is explicitly reverted in favor of WSL compatibility.

## Phase 5 (merged to main as PR #4)

- Terminal UI: ASCII oscilloscope, raw stdin via termios, atexit termios restore.
- Live keyboard controls: SPACE (force mutate), `+`/`-` (tempo ±10%), `q` (quit).
- `gen_force_mutate()` and `gen_set_tempo(int delta_pct)` exposed in `gen.h`.
- Packed binary: 12,032 bytes.

## Phase 4-p3 (merged to main as PR #3)

- Restored `--build-id` in LDFLAGS so UPX could pack the binary without a runtime crash. UPX `--ultra-brute` produces a 10,984-byte packed binary that decompresses in-memory (no `/tmp` writes).
- The `synth.memfd` C-based extractor and the `synth.packed` XZ+shell extractor were both abandoned in favour of UPX.

## Phase 4-finish

- Replaced libasound calls with direct `ioctl(SNDRV_PCM_IOCTL_*)` on `/dev/snd/pcmC0D0p`. Met PLAN.md threshold (c). Live audio worked on native Linux but failed in WSL because WSL lacks `/dev/snd`.

## Phase 4-complete

- 8-voice polyphony (up from 4). Per-voice 2-pole Chamberlin SVF lowpass. Second CA layer (`ca_harm`, Rule 30) added to `gen.c`.
- Two compression variants explored:
  - `synth.packed` (XZ + shell self-extractor): 5,892 bytes but used `/tmp`.
  - `synth.memfd` (C memfd_create + execveat): 20,048 bytes, no `/tmp`.

## Phase 4 (initial size-optimization slice; merged to main as PR #3)

- Stripped binary size reduced via UPX from 14,528 to 5,892 bytes.

## Phase 3 (merged to main as PR #2)

- Replaced the Phase 2 hard-coded arpeggio with the generative MVP per PLAN.md section I:
  - Rule 110 CA evolves the active-degree mask per bar.
  - First-order Markov chain over 7 D-Dorian degrees picks the next note within the active mask.
  - Two parallel Euclidean rhythm masks combined for per-step trigger pattern.
  - Mutation every 4 bars (deviation from PLAN.md's 16 bars; documented in commit).
- Build adds `gen_euclid_table.c` as a fourth build-time generator.

## Phase 2 (merged to main as PR #1)

- Static `pool[65536]` arena with 8-byte-aligned bump allocator. All runtime state allocated from it.
- Voice struct unioning Karplus-Strong and 2-op FM variants. ADSR envelope per voice.
- 4-voice polyphony, hard-coded C-major arpeggio for testing.
- Build-time generators added for envelope curve and per-MIDI-note tables.

## Phase 1 (merged to main as PR #1)

- Hard-coded 440 Hz sine via 1024-entry int16 LUT.
- ALSA `snd_pcm_writei` live playback at 44.1 kHz S16_LE mono.
- `--render <seconds> <out.wav>` mode for offline output and regression tests.
- Bit-exact regression test (`tests/test_bitexact.sh`, `golden/regression_16s.sha256`).

## Initial commit

- `PLAN.md` design document and project skeleton.
