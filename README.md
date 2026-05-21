# stretto

A small generative music synthesizer for Linux. Sub-13 KB packed binary. C99. No malloc.

## Build

```
make
```

Produces a stripped `synth` ELF (~19 KB). To pack to the ≤12.5 KB target:

```
make pack
```

(uses UPX). Override the UPX path with `make UPX_BIN=/path/to/upx pack` if it is not on `$PATH`.

Requires `gcc`, `make`, `libasound2-dev`, and (for packing) `upx`.

## Run

### Live mode

```
./synth
```

Plays generative audio out the default ALSA device and draws an ASCII oscilloscope sized to the terminal.

Status row at the top of the terminal:

```
M:<mod_depth> S:<scale> V:<8 voice activity dots>
```

- `M` — current FM modulation depth (100–8000, default 1500).
- `S` — current scale: `D` Dorian, `L` Lydian, `P` Phrygian. Auto-rotates every 32 bars (~64 s).
- `V` — eight characters, one per voice. `*` means the voice is firing or sustaining; `.` means it is silent.

### Live keyboard controls

| Key | Action |
|---|---|
| `SPACE` | Force a mutation (re-roll one Markov weight, flip one CA bit, change one Euclidean k) |
| `+` | Tempo up 10% |
| `-` | Tempo down 10% |
| `[` | FM `mod_depth` down by 200 (cleaner) |
| `]` | FM `mod_depth` up by 200 (more harmonics) |
| `s` | Cycle scale (Dorian → Lydian → Phrygian → …) |
| `q` | Quit (restores terminal state) |
| `Ctrl-C` | Same as `q` via atexit handler |

ALSA underruns and PM suspends are recovered automatically via `snd_pcm_recover`; only truly unrecoverable errors stop the program.

### Render mode

```
./synth --render <seconds> <output.wav>
```

Writes a mono 16-bit 44.1 kHz PCM WAV. No audio device opened. Used for regression tests and offline analysis. Seconds must be an integer in 1..3600.

## Architecture

### Voice roles

The 8-voice pool is split into three role groups, each with its own envelope shape, FM parameters, and pitch range:

| Slots | Role | Synth |
|---|---|---|
| 0–1 | Bass | FM, mod_depth 200, ratio 1:1, attack 50 ms, release 1 s, pitch -12 semitones |
| 2–4 | Chord | FM, mod_depth 1500, ratio 2:1, attack 20 ms, release 600 ms |
| 5–7 | Melody | KS or FM alternating, mod_depth user-tunable, attack 5 ms, release 600 ms |

Voice stealing is constrained to a role's reserved slot range so the bass never displaces melody and vice versa.

### Synthesis

- **Karplus-Strong** plucked-string voices. Noise-initialised circular buffer, damped average feedback (factor ~0.99).
- **2-op FM** voices. Carrier and modulator NCOs sharing one 1024-entry sine LUT. Modulator ratio is 1:1 for bass, 2:1 for chord and melody.
- **ADSR envelope** per voice. Attack / release durations are per-role; decay (200 ms) and sustain (50%) are shared.
- **State-variable filter** per voice. 2-pole Chamberlin lowpass at ~5.6 kHz, Q ≈ 2.56. State stored as `int32_t` to prevent resonance wrap; output saturates to `int16_t`.
- **Mix**: sum 8 voices into int32, divide by 8 at the output stage.

### Generative layer

- **Scales**: D Dorian, D Lydian, D Phrygian. Auto-rotates every 32 bars; `s` key cycles manually. Markov runs on degree indices so a single matrix applies to any 7-note scale.
- **Rule 110 cellular automaton** (`ca_row`, 32-bit). Low 7 bits select which scale degrees are active for the current bar.
- **Rule 30 CA** (`ca_harm`) advances every 4 steps and ANDs into the active mask to introduce harmonic variation. Pairing 110 with 30 avoids the long repeats of 110 alone and the pure randomness of 30 alone.
- **Markov chain** (7×7 weights) picks the next melody degree from the active mask. Initial weights are hand-tuned: stepwise motion weighted, leading-tone and dominant cadences favour tonic. Weights mutate at runtime.
- **Euclidean rhythm**: two parallel `E(k, 16)` masks (`eucl_k_a`, `eucl_k_b`) combined for the melody trigger pattern. Patterns are true Bjorklund (tresillo, cinquillo, etc.).
- **Per-role scheduling**:
  - Bass: once per bar on root or dominant, one octave below the scale.
  - Chord: at steps 0 and 8, fires the root/3rd/5th triad filtered by the active mask.
  - Melody: existing Euclidean rhythm restricted to voices 5–7.
- **Mutation**: every 4 bars (~8 s) re-rolls one Markov weight, flips one CA bit, and bumps one Euclidean k.

### Memory

All runtime state lives in a single static `pool[65536]` arena with an 8-byte-aligned bump allocator. No `malloc`, no `free`. Tables (sine, envelope, MIDI note increments, Euclidean patterns) are generated at build time and embedded as `static const` arrays in `.rodata`. Arena usage at startup is ~10.4 KB of the 64 KB budget.

### Determinism

Render mode produces bit-exact identical output across runs of the same binary. Two PRNGs (one in `voice.c` for KS noise, one in `gen.c` for Markov/mutation), `ca_row`, `ca_harm`, and `cur_scale` are seeded with fixed constants on every `gen_init`. No reads of clock, environment, or filesystem during render.

## Files

| File | Purpose |
|---|---|
| `main.c` | Argv, ALSA setup, render loop, WAV writer, terminal UI |
| `voice.c` / `voice.h` | Voice struct, KS, FM, ADSR, SVF, role-based voice pool |
| `gen.c` / `gen.h` | Sample clock, scales, CAs, Markov, Euclidean, mutation |
| `arena.c` / `arena.h` | Static arena and bump allocator |
| `gen_sin_table.c` | Build-time generator for 1024-entry sine LUT |
| `gen_env_table.c` | Build-time generator for 256-entry envelope curve |
| `gen_note_table.c` | Build-time generator for 128-entry MIDI note tables |
| `gen_euclid_table.c` | Build-time generator for Bjorklund Euclidean masks |
| `Makefile` | Build orchestration (`make`, `make pack`, `make test`, `make golden`) |
| `tests/test_bitexact.sh` | Renders twice, sha256-compares, also checks against golden |
| `golden/regression_16s.sha256` | Reference hash for 16-second render |
| `PLAN.md` | Original design document |

## Tests

```
make test
```

Renders 16 seconds of audio twice and checks sha256 against `golden/regression_16s.sha256`. After an intentional synth change:

```
make golden
```

regenerates the golden hash.

## Sizes

| Artifact | Bytes |
|---|---|
| `synth` (stripped, dynamic-linked libasound + libc) | 19,264 |
| `synth.packed` (UPX `--ultra-brute`) | 12,860 |

Packed size targets ≤12,288 bytes (PLAN.md Phase 4) but is currently 572 bytes over after roles + multi-scale features were added. The growth was accepted in favour of musical richness.

## Environment notes

- **Native Linux**: live audio routes through libasound to ALSA hardware.
- **WSL2 + WSLg**: live audio routes through libasound → pulse plugin → WSLg → Windows audio. Render mode works in all environments.

## License

MIT. See `LICENSE`.
