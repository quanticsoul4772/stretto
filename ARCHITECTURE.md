# Architecture

## Overview

`stretto` is a single-process Linux audio program that mixes generated music to an ALSA device or a WAV file. It compiles to a stripped ELF in the 15–20 KB range and packs with UPX to under 12 KB. All runtime allocations come from a single 64 KB static arena. The audio thread is the main thread; no concurrency primitives are used.

## Module layout

```
main.c            argv, ALSA open/write loop, WAV writer, terminal UI (raw stdin, oscilloscope, key handler)
voice.c / .h      Voice struct, KS, FM, ADSR, SVF; voice pool; mod_depth getter/setter; active mask
gen.c   / .h      sample clock, Rule-110 + Rule-30 CAs, Markov chain, Euclidean rhythm, mutation
arena.c / .h      static pool[65536], 8-byte-aligned bump allocator, oom = exit(1)
gen_sin_table.c   build-time generator: 1024-entry int16 sine LUT (peak 24576)
gen_env_table.c   build-time generator: 256-entry uint8 exponential envelope curve
gen_note_table.c  build-time generator: 128 MIDI notes -> {phase increment, KS buffer length}
gen_euclid_table.c build-time generator: 17 16-bit Euclidean masks E(0..16, 16)
```

The `gen_*` programs run during `make`. They emit C headers (`sin_table.h`, `env_table.h`, `note_table.h`, `euclid_table.h`) that are `#include`d by `voice.c` and `gen.c`. The tables end up in `.rodata` of the final binary and do not consume arena space.

## Audio path

```
gen_step ──► voice_pool_trigger ──► Voice[0..7] ──► voice_pool_mix ──► render_chunk ──► snd_pcm_writei
                                                                                      └► fwrite (WAV)
```

Each call to `render_chunk(buf, frames)` produces `frames` mono int16 samples. It is the only path that advances the sample clock and the only function called by both live and render modes. This guarantees that live and render outputs are sample-identical given the same starting state.

### Voice

`struct Voice` is a union of two synthesis variants sharing a common envelope and filter:

```
type            VOICE_OFF | VOICE_KS | VOICE_FM
note            MIDI 0..127
env_phase       ENV_OFF | ENV_A | ENV_D | ENV_R
env_amp         0..32767, ADSR amplitude in 16.0 fixed-point
env_time        sample count since current phase started
svf_lp, svf_bp  int32 SVF state (widened from int16 to prevent resonance wrap)
union:
  ks { int16 buf[512], idx, len }
  fm { uint32 phase_c, phase_m, inc_c, inc_m; uint16 mod_depth }
```

`voice_step` advances one sample of one voice through three stages: raw oscillator (KS or FM), envelope multiplication, then SVF. It returns the SVF lowpass output saturated to int16.

`voice_pool_mix` calls `voice_step` for all 8 voices, sums to int32, and divides by 8 (right-shift 3). Result fits in int16 without further clipping at current per-voice amplitudes.

### Karplus-Strong

On trigger, the voice's circular buffer of length `note_ks_len[note]` is filled with half-amplitude white noise from a dedicated xorshift32 PRNG. Each step outputs the head sample, then writes back the damped average of two adjacent samples:

```
out = buf[idx]
avg = (a + b) * 32440 >> 16   (damp factor ≈ 0.99)
buf[idx] = avg
idx = (idx + 1) % len
```

The dedicated PRNG is reseeded only at process start, so KS noise initialisation is part of the deterministic render output.

### 2-op FM

Two uint32 NCOs share the sine LUT. The modulator's output (signed int16) scales by `mod_depth` and offsets the carrier's phase:

```
mod = sin_table[phase_m >> 22]
phase_m += inc_m
phase_c_eff = phase_c + ((int32)mod * mod_depth) << 6
out = sin_table[phase_c_eff >> 22]
phase_c += inc_c
```

`inc_m = 2 * inc_c` (a fixed 2:1 modulator-to-carrier ratio). `mod_depth` is runtime-tunable in live mode via `[` and `]` keys; default is 1500. Higher values widen the FM spectrum; values past ~3000 alias significantly for higher notes.

### Envelope

ADSR with hand-tuned constants:

```
attack    220 samples  (~5 ms)
decay   8820 samples  (~200 ms)
sustain   16384 / 32767 = 50%
release 26460 samples  (~600 ms)
```

Curve shape comes from `env_table[]`, a 256-entry `1 - exp(-x * 5)` ramp normalised to 0–255. Attack uses the curve directly; decay and release use `255 - env_table[idx]` to invert it.

### State-variable filter

A standard Chamberlin 2-pole topology:

```
hp = in - lp - (bp * damp)
bp += hp * f1
lp += bp * f1
```

`f1 = f / 256 = 200 / 256 ≈ 0.78` gives cutoff ~5.6 kHz at 44.1 kHz sample rate. `damp = q / 256 = 100 / 256 ≈ 0.39` gives Q ≈ 2.56.

The state (`svf_lp`, `svf_bp`) is stored in `int32_t`, not `int16_t`. At Q ≈ 2.56 the SVF can ring the internal state to roughly 2.5× input amplitude. With per-voice input peaking near full-scale int16, an `int16` state would wrap, producing audible clicks (measured: 395 click events per 16-second render). Widening to int32 eliminates the wrap; the output `lp` is saturated to int16 at return.

## Generative path

A single sample clock drives everything. `gen_step` is called once per output sample by `render_chunk`. It compares `sample_clock` to `next_step` and, on each step boundary:

1. If start of bar, advance `ca_row` (Rule 110) and increment `bar_count`. Every `MUTATE_BARS` (4) bars, call `mutate()`.
2. Compute `active_mask = ca_row & 0x7F` (which scale degrees are allowed this bar).
3. Compute `hits = euclid_table[eucl_k_a] | euclid_table[eucl_k_b]`. If bit `(15 - step_in_bar)` is set, fire a note.
4. To fire: `markov_next(cur_degree, active_mask)` returns the next scale degree; map it to MIDI via `SCALE[]`; alternate KS/FM by step parity; call `voice_pool_trigger`.

### Markov chain

`markov[from][to]` is a 7×7 `uint8_t` matrix of unnormalised weights. `markov_next` sums the weights of columns in the active mask, draws `prng() % sum`, and walks. If the row totals to zero in the active mask, the chain holds the current degree (this is the only path that produces a silent note).

The weights are hand-tuned for D Dorian on first init; `mutate()` modifies them at runtime so they drift.

### Cellular automata

Two CAs run in parallel:

- `ca_row` (Rule 110) — a 32-bit row advanced once per bar. Low 7 bits become the active-degree mask.
- `ca_harm` (Rule 30) — a second 32-bit row advanced every 4 steps. Its bits modulate the harmonic mask.

Rule 110 is class IV (complex, computationally universal). Rule 30 is class III (chaotic). Pairing them avoids both reducing to short cycles or pure noise.

If either CA collapses to all-zero, it is reseeded to `0x12345678`.

### Euclidean rhythm

Two parameters `eucl_k_a` and `eucl_k_b` select two 16-step rhythm masks from `euclid_table[]`. The OR of the two masks defines hit positions inside a 16-step bar. `mutate()` alternates which one it bumps so both drift over time.

The generator at build time uses floor-distributed pulses (`pat[i * n / k] = 1`), not true Bjorklund. The patterns differ slightly from canonical Bjorklund for some `k`, but stay musically valid.

### Mutation

Triggered at the start of each bar where `bar_count % 4 == 0`. Mutation does three things:

1. Re-rolls one cell of the Markov matrix (random row, random column, new value 0–15).
2. Flips one bit of `ca_row` (16 possible positions).
3. Bumps either `eucl_k_a` or `eucl_k_b` to a new value in its allowed range.

Defenses: CA reseed on collapse, mod_depth clamping (100–8000), tempo clamping (samples_per_step 2000–20000), Euclidean k clamped to ≥ 1.

## Memory model

```
static uint8_t pool[65536] __attribute__((aligned(64)));
static size_t bump;
```

Allocations come from `arena_alloc(n)`, which rounds `n` up to 8-byte alignment, bumps the cursor, and exits on overflow. There is no free path. Lifetimes match the process lifetime.

Typical layout at startup (after `voice_pool_init` and `render_chunk` first call):

```
N_VOICES (8) * sizeof(Voice) (~1052)  ≈ 8400 bytes
BUFFER_FRAMES (1024) * sizeof(int16)  =  2048 bytes
                                       --------
                                       ~10448 bytes used of 65536
```

The arena reports its usage at the end of `--render` runs (`arena: 10368/65536 bytes used`). The remaining headroom is intentional — adding voices, larger KS buffers, or a reverb would draw from here.

## Determinism

All random state at process start is fixed:

```
voice.c PRNG seed = 0xCAFEBABE
gen.c   PRNG seed = 0xDEADBEEF
ca_row init       = 0x12345678
ca_harm init      = 0xABCDEF01
markov[] init     = hand-tuned constants
```

Nothing reads the clock, environment, or filesystem during render. Two renders with the same binary produce byte-identical WAV output. This is the basis of the `make test` regression check.

## ALSA path

Live mode opens `"default"` via `snd_pcm_open`, configures via `snd_pcm_set_params` (S16_LE, mono, 44100 Hz, ~100 ms latency), and writes 1024-frame blocks via `snd_pcm_writei`. There is no xrun recovery: any negative return from `snd_pcm_writei` prints `snd_strerror` to stderr and exits 1.

On WSL2, this routes through libasound's `pulse` plugin into WSLg's PulseAudio bridge. On native Linux, it routes through libasound to whatever ALSA device `default` resolves to.

## Terminal UI

Live mode also configures raw stdin (no canonical mode, no echo) and `O_NONBLOCK` on fd 0. An `atexit` handler restores the saved termios so quitting via `q`, `Ctrl-C`, or any `exit(1)` path leaves the terminal usable.

Drawing happens after each `snd_pcm_writei`. The cursor is moved home with `\x1b[H` (no full screen clear, to avoid flicker), the status row is written, then the oscilloscope. Each row is preceded by `\x1b[2K` (erase line) so width changes don't leave artifacts.

Amplitude thresholds in `draw_oscilloscope` are calibrated to the actual signal range (~6000 peak) rather than full int16 scale.

## Size discipline

Compiler flags (`-Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections`) and stripping (`strip -s -R .comment`) are conventional. `-no-pie` is required by the absence of `-fpic`.

UPX `--ultra-brute` provides the final ~35% reduction. It decompresses in-memory at runtime; no `/tmp` file is written.

The packed binary is under the 12,288-byte target by ~120 bytes after the SVF int32 widening and the realtime control additions. Further reductions are possible (musl static link, custom `_start`, hand-written linker script) but not currently applied.
