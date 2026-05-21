# Changelog

## Unreleased

(none currently — everything below is on `main`.)

## Recent main-branch work

### Code quality / cleanup
- Remove dead `voice_pool_trigger` (non-role variant); only the role-aware API is used. Fix two `-Wshadow` warnings in `draw_oscilloscope` by renaming locals (`t`→`thresh`, `s`→`samp`).
- Inline rationale comments added at four points that previously required reading commit history: SVF int32 widening, `MUTATE_BARS` deviation from PLAN.md, Markov weights, Rule 110 + Rule 30 pairing.
- ALSA underrun recovery: `snd_pcm_recover` on `snd_pcm_writei` failure handles EPIPE / ESTRPIPE / EINTR; only truly unrecoverable errors exit.
- `--render <seconds>` validates input via `strtol`; rejects non-numeric, negative, zero, and > 3600.
- Makefile housekeeping: removed unused `SMOL` variable, split stale `SIZE_TARGET` into `STRIP_TARGET` and `PACK_TARGET`, added `UPX_BIN` / `UPX_FLAGS` variables and `make pack` target, expanded `make clean` to remove abandoned experiment artifacts.
- main.c warnings: cast `write()` returns to `(void)!`, remove dead `peak` computation, add fail-loud error path for `fcntl` failures. Hoist SVF `f` / `q` to file-scope `SVF_F` / `SVF_Q` macros.

### Synth features
- True Bjorklund Euclidean rhythm patterns replace the floor-distributed approximation. `gen_euclid_table.c` emits the canonical tresillo, cinquillo, etc. masks. Popcount of each E(k,16) mask verified equal to k.
- Multi-scale rotation: D Dorian, D Lydian, D Phrygian. Auto-rotates every 32 bars; `s` key cycles manually. Status row shows current scale.
- Per-voice roles: bass (slots 0–1), chord (slots 2–4), melody (slots 5–7). Each role has its own envelope timings, FM parameters, and pitch offset. Voice stealing constrained to a role's range.
  - Bass: FM 1:1 ratio, mod_depth 200, attack 50 ms, release 1 s, pitch −12.
  - Chord: triad on degrees 0/2/4 from active mask, fired at steps 0 and 8.
  - Melody: existing Euclidean rhythm; KS/FM alternation by step parity.

### Audio fixes
- SVF state widened from `int16_t` to `int32_t` with int16 saturation at output. Eliminates resonance-wrap clicks (measured: 395 → 0 clicks per 16 s render).
- Default FM `mod_depth` reduced from 6000 to 1500. Cuts FM aliasing; HF energy >8 kHz dropped 45% → 12%.
- Realtime `mod_depth` keys (`[` / `]`) added to live mode for ear-tuning.

### Docs
- README, ARCHITECTURE, CHANGELOG, MIT LICENSE.

## fix-wsl-libasound (PR #6, merged)

- Reverted Phase 4-finish's direct ioctl path back to libasound. The ioctl path required `/dev/snd/pcmC0D0p`, which WSL does not expose. Live audio now routes through libasound → pulse plugin → WSLg on WSL, and through libasound → ALSA hardware on native Linux.
- Threshold (c) "no libasound" from the original PLAN.md Phase 4 is explicitly reverted in favor of WSL compatibility.

## Phase 5 (PR #4, merged)

- Terminal UI: ASCII oscilloscope, raw stdin via termios, atexit termios restore.
- Live keyboard controls: SPACE (force mutate), `+`/`-` (tempo ±10%), `q` (quit).
- `gen_force_mutate()` and `gen_set_tempo(int delta_pct)` exposed in `gen.h`.
- Packed binary: 12,032 bytes.

## Phase 4-p3 (PR #3, merged)

- Restored `--build-id` in LDFLAGS so UPX could pack the binary without a runtime crash. UPX `--ultra-brute` produces a 10,984-byte packed binary that decompresses in-memory (no `/tmp` writes).
- The `synth.memfd` C-based extractor and the `synth.packed` XZ+shell extractor were both abandoned in favour of UPX.

## Phase 4-finish

- Replaced libasound calls with direct `ioctl(SNDRV_PCM_IOCTL_*)` on `/dev/snd/pcmC0D0p`. Met PLAN.md threshold (c). Live audio worked on native Linux but failed in WSL because WSL lacks `/dev/snd`.

## Phase 4-complete

- 8-voice polyphony (up from 4). Per-voice 2-pole Chamberlin SVF lowpass. Second CA layer (`ca_harm`, Rule 30) added to `gen.c`.
- Two compression variants explored:
  - `synth.packed` (XZ + shell self-extractor): 5,892 bytes but used `/tmp`.
  - `synth.memfd` (C memfd_create + execveat): 20,048 bytes, no `/tmp`.

## Phase 4 (PR #3 initial slice, merged)

- Stripped binary size reduced via UPX from 14,528 to 5,892 bytes.

## Phase 3 (PR #2, merged)

- Replaced the Phase 2 hard-coded arpeggio with the generative MVP per PLAN.md section I:
  - Rule 110 CA evolves the active-degree mask per bar.
  - First-order Markov chain over 7 D-Dorian degrees picks the next note within the active mask.
  - Two parallel Euclidean rhythm masks combined for per-step trigger pattern.
  - Mutation every 4 bars (deviation from PLAN.md's 16 bars; documented in commit and now in inline comment).
- Build adds `gen_euclid_table.c` as a fourth build-time generator.

## Phase 2 (PR #1, merged)

- Static `pool[65536]` arena with 8-byte-aligned bump allocator. All runtime state allocated from it.
- Voice struct unioning Karplus-Strong and 2-op FM variants. ADSR envelope per voice.
- 4-voice polyphony, hard-coded C-major arpeggio for testing.
- Build-time generators added for envelope curve and per-MIDI-note tables.

## Phase 1 (PR #1, merged)

- Hard-coded 440 Hz sine via 1024-entry int16 LUT.
- ALSA `snd_pcm_writei` live playback at 44.1 kHz S16_LE mono.
- `--render <seconds> <out.wav>` mode for offline output and regression tests.
- Bit-exact regression test (`tests/test_bitexact.sh`, `golden/regression_16s.sha256`).

## Initial commit

- `PLAN.md` design document and project skeleton.
