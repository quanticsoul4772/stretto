# stretto

A small generative music synthesizer for Linux. Sub-12 KB packed binary. C99. No malloc.

## Build

```
make
```

Produces a stripped `synth` ELF (~19 KB). To pack to the ≤12 KB target:

```
upx --ultra-brute synth -o synth.packed
```

Requires `gcc`, `make`, `libasound2-dev`, and (for packing) `upx`.

## Run

### Live mode

```
./synth
```

Plays generative audio out the default ALSA device and draws an ASCII oscilloscope sized to the terminal.

Status row at the top of the terminal:

```
M:<mod_depth> V:<8 voice activity dots>
```

- `M` — current FM modulation depth (100–8000, default 1500)
- `V` — eight characters, one per voice. `*` means the voice is firing or sustaining; `.` means it is silent.

### Live keyboard controls

| Key | Action |
|---|---|
| `SPACE` | Force a mutation (re-roll one Markov weight, flip one CA bit, change one Euclidean k) |
| `+` | Tempo up 10% |
| `-` | Tempo down 10% |
| `[` | FM `mod_depth` down by 200 (cleaner) |
| `]` | FM `mod_depth` up by 200 (more harmonics) |
| `q` | Quit (restores terminal state) |
| `Ctrl-C` | Same as `q` via atexit handler |

### Render mode

```
./synth --render <seconds> <output.wav>
```

Writes a mono 16-bit 44.1 kHz PCM WAV. No audio device opened. Used for regression tests and offline analysis.

## Architecture

### Synthesis

- **Karplus-Strong** plucked-string voices. Noise-initialized circular buffer, damped average feedback (factor ~0.99).
- **2-op FM** voices. Carrier and modulator NCOs sharing one 1024-entry sine LUT. Modulator ratio 2:1.
- **ADSR envelope** per voice. Attack 5 ms, decay 200 ms, release 600 ms, sustain at 50%.
- **State-variable filter** per voice. 2-pole Chamberlin lowpass at ~5.6 kHz, Q ≈ 2.56. State stored as `int32_t` to avoid resonance wrap; output saturates to `int16_t`.
- **8 voices** mixed by simple sum, divided by 8 at the output stage.

### Generative layer

- **Scale**: D Dorian, 7 degrees (MIDI 62, 64, 65, 67, 69, 71, 72).
- **Rule 110 cellular automaton** (32-bit row). Low 7 bits used as a mask of which scale degrees are active in the current bar.
- **Rule 30 CA** (`ca_harm`) advances every 4 steps; bit selection masks the rhythmic Euclidean output.
- **Markov chain** (7×7 weights) picks the next scale degree from the active mask. Weights are hand-tuned starting values; they mutate at runtime.
- **Euclidean rhythm**: two parallel `E(k, 16)` masks (`eucl_k_a`, `eucl_k_b`) combined to determine which 16th-note steps trigger.
- **Mutation**: every 4 bars (~8 s at default tempo) re-rolls one Markov weight, flips one CA bit, and bumps one Euclidean k.

### Memory

- All runtime state lives in a single static `pool[65536]` arena with an 8-byte-aligned bump allocator. No `malloc`, no `free`.
- Tables (sine, envelope curve, MIDI note increments, Euclidean patterns) are generated at build time and embedded as `static const` arrays. They live in `.rodata`, not in the arena.

### Determinism

Render mode produces bit-exact identical output across runs of the same binary. Two PRNGs (one in `voice.c` for KS noise, one in `gen.c` for Markov/mutation) are seeded with fixed constants and not reseeded.

## Files

| File | Purpose |
|---|---|
| `main.c` | Argv, ALSA setup, render loop, WAV writer, terminal UI |
| `voice.c` / `voice.h` | Voice struct, KS, FM, ADSR, SVF, voice pool |
| `gen.c` / `gen.h` | Sample clock, CA, Markov, Euclidean, mutation |
| `arena.c` / `arena.h` | Static arena and bump allocator |
| `gen_sin_table.c` | Build-time generator for 1024-entry sine LUT |
| `gen_env_table.c` | Build-time generator for 256-entry envelope curve |
| `gen_note_table.c` | Build-time generator for 128-entry MIDI note tables |
| `gen_euclid_table.c` | Build-time generator for 17-entry Euclidean pattern table |
| `Makefile` | Build orchestration |
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
| `synth` (stripped, dynamic-linked libasound + libc) | ~19,300 |
| `synth.packed` (UPX `--ultra-brute`) | ~12,170 |

Packed size is under the 12,288-byte (12 KB) target by ~120 bytes of margin.

## Environment notes

- **Native Linux**: live audio routes through libasound to ALSA hardware.
- **WSL2 + WSLg**: live audio routes through libasound → pulse plugin → WSLg → Windows audio. Render mode works in all environments.

## License

MIT. See `LICENSE`.
