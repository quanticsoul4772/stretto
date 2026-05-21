# Architecture

## Overview

`stretto` is a single-process Linux audio program that mixes generated music to an ALSA device or a WAV file. It compiles to a stripped ELF around 19 KB and packs with UPX to ~12.9 KB. All runtime allocations come from a single 64 KB static arena. The audio thread is the main thread; no concurrency primitives are used.

## Module layout

```
main.c            argv, ALSA open/write loop with xrun recovery, WAV writer,
                  terminal UI (raw stdin, status row, oscilloscope, key handler)
voice.c / .h      Voice struct, KS, FM, ADSR, SVF; role-scoped voice pool
gen.c   / .h      sample clock, three scales, Rule-110 + Rule-30 CAs, Markov chain,
                  Bjorklund Euclidean rhythm, mutation, role-based scheduling
arena.c / .h      static pool[65536], 8-byte-aligned bump allocator, oom = exit(1)
gen_sin_table.c   build-time generator: 1024-entry int16 sine LUT (peak 24576)
gen_env_table.c   build-time generator: 256-entry uint8 exponential envelope curve
gen_note_table.c  build-time generator: 128 MIDI notes -> {phase increment, KS buffer length}
gen_euclid_table.c build-time generator: 17 16-bit Bjorklund Euclidean masks E(0..16, 16)
```

The `gen_*` programs run during `make`. They emit C headers (`sin_table.h`, `env_table.h`, `note_table.h`, `euclid_table.h`) that are `#include`d by `voice.c` and `gen.c`. The tables end up in `.rodata` of the final binary and do not consume arena space.

## Audio path

```
gen_step ──► voice_pool_trigger_role ──► Voice[0..7] ──► voice_pool_mix ──► render_chunk ──► snd_pcm_writei
                                                                                          └► fwrite (WAV)
```

Each call to `render_chunk(buf, frames)` produces `frames` mono int16 samples. It is the only path that advances the sample clock and the only function called by both live and render modes. This guarantees live and render outputs are sample-identical given the same starting state.

### Voice roles

The 8-voice pool is partitioned into three role groups:

| Slots | Role | Envelope | FM mod_depth | FM ratio | Pitch offset |
|---|---|---|---|---|---|
| 0–1 | Bass   | A 50 ms / R 1000 ms | 200 (fixed) | 1:1 | -12 semitones |
| 2–4 | Chord  | A 20 ms / R 600 ms  | 1500 (fixed) | 2:1 | 0 |
| 5–7 | Melody | A 5 ms / R 600 ms   | 1500 default, user-tunable | 2:1 | 0 |

Decay (200 ms) and sustain (50%) are shared across roles. Voice stealing (`pick_slot_range`) only searches within a role's reserved range so a chord trigger never displaces a bass voice and vice versa.

### Voice struct

```
type            VOICE_OFF | VOICE_KS | VOICE_FM
note            MIDI 0..127
env_phase       ENV_OFF | ENV_A | ENV_D | ENV_R
role            ROLE_BASS | ROLE_CHORD | ROLE_MELODY
env_amp         0..32767, ADSR amplitude
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

`inc_m = inc_c * ratio` where the ratio is 1:1 for bass (gives a soft sine-like sub) and 2:1 for chord/melody (bell-ish). `mod_depth` is runtime-tunable in live mode via `[` and `]` keys for melody voices; bass and chord use fixed depths.

### Envelope

ADSR with per-role attack and release durations (`role_attack[]`, `role_release[]` tables in voice.c). Shared decay 8820 samples (~200 ms) and sustain 16384/32767 (~50%). The 256-entry `env_table[]` provides a `1 - exp(-x*5)` curve normalised to 0–255; attack reads it directly, decay and release read the inverse (`255 - env_table[idx]`).

### State-variable filter

A standard Chamberlin 2-pole topology:

```
hp = in - lp - (bp * damp)
bp += hp * f1
lp += bp * f1
```

`SVF_F = 200`, `SVF_Q = 100`, both shifted by 8 (divided by 256). Gives cutoff ~5.6 kHz and Q ≈ 2.56 at 44.1 kHz. State (`svf_lp`, `svf_bp`) is `int32_t`, not `int16_t`. At Q ≈ 2.56 the SVF can ring the internal state to ~2.5× input amplitude; int16 would wrap and produce broadband clicks. Widening to int32 eliminates the wrap; output `lp` is saturated to int16 at return.

## Generative path

A single sample clock drives everything. `gen_step` is called once per output sample by `render_chunk`. It compares `sample_clock` to `next_step` and on each step boundary:

1. If start of bar: advance `ca_row` (Rule 110), increment `bar_count`. Every `MUTATE_BARS` (4) bars call `mutate()`. Every `SCALE_ROTATE_BARS` (32) bars rotate `cur_scale`.
2. Every 4 steps: advance `ca_harm` (Rule 30).
3. Compute `active_mask = (ca_row & 0x7F) & (ca_harm_mask | 0x11)` — degrees allowed this bar.
4. Compute Euclidean `hits = euclid_table[eucl_k_a] | euclid_table[eucl_k_b]`. Determine if step is a hit.
5. Fire role triggers:
   - **Bass** at step 0: root or dominant (alternating by bar parity), one octave down.
   - **Chord** at steps 0 and 8: up to three triad voices on degrees 0/2/4 filtered by active_mask.
   - **Melody** on Euclidean hits: `markov_next(cur_degree, active_mask)` picks a degree; map via `SCALES[cur_scale][]`; type alternates KS/FM by step parity.

### Scales

```
SCALES[3][7] = {
    /* Dorian   */ { 62, 64, 65, 67, 69, 71, 72 },
    /* Lydian   */ { 62, 64, 66, 68, 69, 71, 73 },
    /* Phrygian */ { 62, 63, 65, 67, 69, 70, 72 },
};
```

The Markov runs on degree indices (0..6), so a single 7×7 matrix applies to any 7-note scale. Only the degree-to-MIDI mapping changes when `cur_scale` rotates. `cur_scale` resets to 0 in `gen_init` for render-mode determinism. Auto-rotation happens at the 32-bar boundary inside `gen_step`; the `s` key in live mode cycles manually.

### Markov chain

`markov[from][to]` is a 7×7 `uint8_t` matrix of unnormalised weights. `markov_next` sums weights for columns in the active mask, draws `prng() % sum`, and walks. Initial weights are hand-tuned with these principles:

- Stepwise motion (cols at ±1) weighted moderate (3–4).
- Strong cadences: rows 4 (dominant) and 6 (leading) bias toward row 0 (tonic) with weight 5.
- Diagonal is 0 — prevents stuck self-transitions.
- Tonic (row 0) opens broadly to all degrees except itself, slight bias to dominant.

`mutate()` modifies the matrix at runtime so it drifts from the initial seed.

### Cellular automata

Two CAs run in parallel:

- `ca_row` (Rule 110, class IV) — a 32-bit row advanced once per bar. Low 7 bits become the active-degree mask.
- `ca_harm` (Rule 30, class III) — a second 32-bit row advanced every 4 steps. ANDed with `ca_row & 0x7F` to filter degrees.

Pairing Rule 110 (which alone tends to settle into long-period repeats) with Rule 30 (which alone is too random) gives recurring structure with variation. If either CA collapses to all-zero it is reseeded.

### Euclidean rhythm

Two parameters `eucl_k_a` and `eucl_k_b` select two 16-step rhythm masks from `euclid_table[]`. The OR of the two masks defines hit positions inside a 16-step bar. `mutate()` alternates which one it bumps so both drift over time.

The build-time generator implements true Bjorklund (recursive bucket merge, the Euclid-of-GCD algorithm). The resulting masks are the canonical tresillo, cinquillo, and related patterns. Popcount of `euclid_table[k]` equals `k` for all `k = 0..16`.

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

Allocations come from `arena_alloc(n)`, which rounds `n` up to 8-byte alignment, bumps the cursor, and exits on overflow. No free path. Lifetimes match the process lifetime. Typical usage at startup is ~10.4 KB of 64 KB.

## Determinism

All random state at process start is fixed:

```
voice.c PRNG seed = 0xCAFEBABE
gen.c   PRNG seed = 0xDEADBEEF
ca_row init       = 0x12345678
ca_harm init      = 0x87654321
cur_scale init    = 0 (Dorian)
markov[] init     = hand-tuned constants
```

Nothing reads the clock, environment, or filesystem during render. Two renders with the same binary produce byte-identical WAV output. This is the basis of the `make test` regression check.

## ALSA path

Live mode opens `"default"` via `snd_pcm_open`, configures via `snd_pcm_set_params` (S16_LE, mono, 44100 Hz, ~100 ms latency), and writes 1024-frame blocks via `snd_pcm_writei`. On any negative return, `snd_pcm_recover(pcm, err, 1)` is called: it handles EPIPE (underrun), ESTRPIPE (PM suspend), and EINTR internally by re-preparing the stream. Only truly unrecoverable errors exit; the recovered iteration skips its oscilloscope update and continues.

On WSL2, this routes through libasound's `pulse` plugin into WSLg's PulseAudio bridge. On native Linux, it routes through libasound to whatever ALSA device `default` resolves to.

## Terminal UI

Live mode configures raw stdin (no canonical mode, no echo) and `O_NONBLOCK` on fd 0. An `atexit` handler restores the saved termios so quitting via `q`, `Ctrl-C`, or any `exit(1)` path leaves the terminal usable.

Drawing happens after each `snd_pcm_writei`. The cursor is moved home with `\x1b[H` (no full screen clear, to avoid flicker), the status row is written, then the oscilloscope. Each row is preceded by `\x1b[2K` (erase line) so width changes do not leave artifacts.

Status row format: `M:<mod_depth> S:<D|L|P> V:<8 activity dots>`.

Amplitude thresholds in `draw_oscilloscope` are calibrated to the actual signal range (~10000 peak after roles + multi-scale).

## Size discipline

Compiler flags (`-Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections`) and stripping (`strip -s -R .comment`) are conventional. `-no-pie` is required by the absence of `-fpic`.

`make pack` runs UPX `--ultra-brute` for the final ~33% reduction. UPX decompresses in-memory at runtime; no `/tmp` file is written.

Current sizes:
- unpacked synth: 19,264 bytes
- UPX packed:     12,860 bytes (572 bytes over PLAN.md's 12,288 threshold)

The pre-roles baseline was ~11.8 KB packed. Adding bass/chord/melody roles, multi-scale rotation, Bjorklund Euclidean, status row, and the runtime mod_depth control added ~1.1 KB packed. The growth was accepted in favour of musical richness; further reductions (musl static link, custom `_start`, hand-written linker script) remain available if needed.
