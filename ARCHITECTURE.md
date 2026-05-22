# Architecture

## Overview

`stretto` is a single-process audio synthesizer that mixes generated music to PulseAudio (Linux), Win32 `waveOut` (Windows), or a WAV file. All runtime allocations come from a single static 128 KB arena. The audio thread is the main thread; on Linux a `pa_threaded_mainloop` helper thread services the PulseAudio event loop, and on Windows the `waveOut` callback fires an event the main thread waits on — neither model requires cross-thread synchronization inside our code.

| Binary | Size |
|---|---|
| Linux `synth` (stripped) | ~29 KB |
| Linux `synth.packed` (UPX) | ~14 KB |
| Windows `stretto.exe` (stripped) | ~210 KB |
| Windows `stretto.packed.exe` (UPX) | ~30 KB |

## Module layout

```
main.c            argv, audio backend (PulseAudio / waveOut), WAV writer,
                  terminal UI (cross-platform raw mode + key polling +
                  oscilloscope + status row), master-bus delay + reverb +
                  soft saturation
voice.c / .h      Voice struct (KS / FM / drum), ADSR, SVF, per-voice peak
                  normalization, role-scoped voice pool of 11 slots
gen.c   / .h      sample clock, six scales, Rule-110 + Rule-30 CAs, Markov
                  chain, Bjorklund Euclidean rhythm, drum pattern banks,
                  bass / chord / melody / counter-melody / drum scheduling,
                  dynamic mutation rate
arena.c / .h      static pool[131072], 8-byte-aligned bump allocator
gen_sin_table.c   build-time: 1024-entry int16 sine LUT (peak 24576)
gen_env_table.c   build-time: 256-entry uint8 exponential envelope curve
gen_note_table.c  build-time: 128 MIDI notes -> {phase increment, KS length}
                  at the 48 kHz sample rate
gen_euclid_table.c build-time: 17 16-bit Bjorklund masks E(0..16, 16)
```

The `gen_*` programs run during `make` and emit C headers (`sin_table.h`, `env_table.h`, `note_table.h`, `euclid_table.h`) that are `#include`d by `voice.c` and `gen.c`. Tables end up in `.rodata` of the final binary; they do not consume arena space.

## Audio path

```
gen_step ──► voice_pool_trigger_role/_drum ──► Voice[0..10]
                                                    │
                                                    ▼
                                            voice_pool_mix (stereo)
                                                    │
                                                    ▼
                                              render_chunk
                                                    │
                                            reverb_process
                                                    │
                                             delay_process
                                                    │
                                          saturate_process
                                                    │
                              ┌─────────────────────┼─────────────────────┐
                              ▼                     ▼                     ▼
                       pa_stream_write       waveOutWrite          fwrite (WAV)
                       (Linux)               (Windows)             (--render)
```

`render_chunk(buf, frames)` fills `2 * frames` int16 samples in interleaved L,R order. It is the only function that advances the sample clock, so live and render outputs are sample-identical given the same starting state and the same seed.

## Voice pool (11 slots, 4 roles)

| Slots | Role | Synth type | Envelope | Pan |
|---|---|---|---|---|
| 0–1 | Bass | FM, 1:1 ratio, mod_depth 200 | A 50 ms / R 1000 ms | Center |
| 2–4 | Chord | FM, 2:1 ratio, mod_depth 1500 | A 20 ms / R 600 ms | L / C / R |
| 5–7 | Melody | KS or FM alternating | A 5 ms / R 600 ms | Outer L / R |
| 8 | Kick | DRUM, sine sweep + click | A 1 ms / linear R 150 ms | Center |
| 9 | Snare | DRUM, noise + 200 Hz body | A 0.5 ms / linear R 100 ms | Slight R |
| 10 | Hihat | DRUM, white noise | A 0.5 ms / linear R 30 ms | Slight L |

Voice stealing (`pick_slot_range`) only searches within a role's reserved range, so a chord trigger never displaces a bass voice. Drum slots are dedicated per drum type — no stealing inside the kit.

## Voice struct

```
type            VOICE_OFF | VOICE_KS | VOICE_FM | VOICE_DRUM
note            MIDI 0..127 (or drum sub-type for VOICE_DRUM)
env_phase       ENV_OFF | ENV_A | ENV_D | ENV_R
role            ROLE_BASS | ROLE_CHORD | ROLE_MELODY | ROLE_DRUM
pan             0 = full L, 128 = center, 255 = full R
env_amp         0..32767, ADSR amplitude
env_time        sample count since current phase started
lfo_phase, lfo_inc       per-voice pan LFO (also drives FM pitch detune)
peak_seen, gain, peak_window   per-voice peak normalization state
svf_lp, svf_bp           int32 SVF state
union:
  ks    { int16 buf[512], idx, len }
  fm    { uint32 phase_c, phase_m, inc_c, inc_m; uint16 mod_depth }
  drum  { uint32 phase, inc; uint8 drum_type }
```

`voice_step` advances one sample of one voice through five stages: raw oscillator (KS / FM / drum), envelope multiplication, SVF, per-voice peak-normalization gain, and finally a per-drum-type post-normalization boost (kick 3×, snare 2.5×, hihat 1.5×) so percussion sits on top of the harmonic content.

`voice_pool_mix` calls `voice_step` for all 11 voices, applies per-voice pan (with slow LFO modulation) to produce L and R contributions, sums into int32 per channel, and returns a `Stereo` pair.

## Synthesis details

### Karplus-Strong (melody, sometimes)

On trigger, the voice's circular buffer of length `note_ks_len[note]` is filled with half-amplitude white noise. Each step outputs the head sample, then writes back the damped average of two adjacent samples (damp factor ≈ 0.99). The result is the classic plucked-string timbre.

### 2-op FM (bass / chord / melody)

Two uint32 NCOs share the sine LUT. Modulator output scales by `mod_depth` and offsets the carrier phase. `inc_m = inc_c * ratio` where ratio is 1:1 for bass (soft, sine-like) and 2:1 for chord/melody (bell-like).

Per-voice LFO pitch detune (~5 cents peak) is layered on top of FM by reusing the pan LFO — same LFO sample, applied to both `inc_c` and `inc_m` proportionally so the FM ratio stays constant.

### Drums

```
KICK   sine wave starting at ~150 Hz; phase increment decays each sample
       (inc -= inc >> 12) so pitch sweeps down to ~45 Hz over ~100 ms.
       First 240 samples (~5 ms) blend a noise burst 50/50 with the sine
       for an audible attack click on speakers with weak bass response.
SNARE  white noise + ~200 Hz sine body, mixed 90/10 noise-dominant.
HIHAT  pure white noise.
```

All three use a one-shot envelope (ENV_A then straight to ENV_R, skipping decay/sustain) with per-drum-type linear release: kick 150 ms, snare 100 ms, hihat 30 ms.

### Envelope (ADSR)

Per-role attack and release durations come from `role_attack[]` and `role_release[]`. Shared decay (200 ms) and sustain (50%) for pitched voices. Drums override the release with a linear `PEAK -> 0` ramp and skip decay/sustain entirely.

### State-variable filter

Chamberlin 2-pole topology. Cutoff (`svf_f_base`) and resonance (`svf_q_base`) are now runtime-tunable file-scope variables (replaced the old `SVF_F`/`SVF_Q` macros). State (`svf_lp`, `svf_bp`) is `int32_t` because at Q ≈ 2.5 the internal state can ring above int16 range; the int16 saturation happens at the function's return.

The effective per-voice filter is composed each sample:

```
f_eff = svf_f_base
      + role_svf_f_off[role]        // bass darker, melody open
      + (lfo * lfo_filter_depth) >> 15   // per-voice pan LFO sweeps cutoff
      + (fenv_amp * 30) >> 14            // chord voices only: filter envelope
                                         //   opens cutoff on attack, closes
                                         //   during release
                                         //   (range 0..+60 units at peak)
clamp f_eff to [20, 230]

q_eff = svf_q_base + role_svf_q_off[role]   // bass less resonant, drums damped
clamp q_eff to [0, 220]
```

User base ranges are deliberately tighter than the effective-value clamps so LFO and filter-envelope modulation always have headroom at the top of the dial:

| Parameter | User clamp | Effective clamp | Default |
|---|---|---|---|
| `svf_f_base` | [30, 180] | [20, 230] | 200 |
| `svf_q_base` | [0, 180] | [0, 220] | 100 |
| `lfo_filter_depth` | [0, 255] | — | 80 |

Per-role offsets:

| Role | f offset | q offset |
|---|---|---|
| Bass | -100 | -30 |
| Chord | -40 | 0 |
| Melody | 0 | 0 |
| Drum | -120 | -50 |

The Chamberlin topology computes `hp`, `bp`, and `lp` outputs inline. `notch = hp + lp`. `voice_step` selects one of the four via the global `filter_mode` (0 LP / 1 HP / 2 BP / 3 notch) and that becomes the post-SVF signal. The mode is cycled live with the `t` key.

`mutate()` also calls `voice_mutate_filter()` about 50% of the time it fires, drifting cutoff ±16 and resonance ±8 so the global filter character evolves alongside Markov / CA / Euclidean drift.

#### Chord filter envelope

Voice struct has dedicated `fenv_amp` / `fenv_time` / `fenv_phase` fields. Only consumed when `role == ROLE_CHORD`. Runs the same ADSR shape as the amplitude envelope (5 ms attack / 200 ms decay / 600 ms release) but feeds into the cutoff modulation rather than the audio gain. Each chord trigger opens the filter and closes it as the chord decays — classic synth filter sweep, scoped to chord voices so it stays subtle.

### Per-voice peak normalization

Each trigger starts a 50 ms (2400-sample) measurement window. While the window is open, the running `peak_seen` updates whenever a new peak is found, and `gain` is recomputed as `PEAK_TARGET / peak_seen` (clamped to a 4× ceiling). The peak grows monotonically (envelope ramps, SVF settles), so gain only decreases — smooth ramp, no clicks. After the window expires, gain is frozen for the rest of the voice's life.

Effect: loud voices (chord at full mod_depth) get attenuated; quiet voices (bass at mod_depth 200) get boosted up to 4×. The mix sits in a predictable amplitude range regardless of which voices are active.

## Master bus

```
voice_pool_mix  ──► reverb_process ──► delay_process ──► saturate_process
                    (Schroeder)        (stereo, 250ms)   (cubic soft sat)
```

### Schroeder reverb

4 parallel comb filters feed 2 series all-pass filters, per channel. Comb feedback ~0.70, RT60 ~1.5 s. L and R use slightly different prime delays so the reverb tail keeps stereo separation.

Comb delays (samples, primes near Schroeder's originals rescaled for 48 kHz):
```
L:  1693, 1759, 1621, 1549
R:  1721, 1747, 1613, 1571
```
All-pass delays:
```
L: 241, 607     R: 251, 613
```

### Delay

Two independent mono buffers (one per stereo channel) of 12000 samples each (250 ms at 48 kHz). Standard feed-forward + feedback:
```
out = dry + tap * wet
delay_buf[idx] = dry + tap * feedback
```
Default `wet = 100/256`, `feedback = 140/256` (cap 200/256 to avoid runaway).

### Soft saturation

Cubic transfer curve `y = x - x^3 / 2^31`. Linear for small `x`; gracefully compresses peaks. Per-channel, last in the chain so any peaks reverb / delay introduce are smoothed before the device.

## Generative path

A single 48 kHz sample clock drives everything. The bar grid is **48 substeps** per bar (LCM of 3, 4, 16), giving 1.999 s at default tempo and supporting true 3-against-4 polyrhythm between bass and chord. `gen_step` is called once per output sample by `render_chunk`; it compares `sample_clock` to `next_step` and on each substep boundary does:

1. **At substep 0 of a bar**: advance `ca_row` (Rule 110), increment `bar_count`. Advance the mutation LFO. Decrement `bars_until_mutate`; when it reaches 0, call `mutate()` and reload from `dynamic_mutate_interval()`.
2. **Every 12 substeps** (i.e. 4 times per bar): advance `ca_harm` (Rule 30).
3. Compute `active_mask = (ca_row & 0x7F) & (ca_harm_mask | 0x11)` — degrees allowed this bar.
4. Fire role triggers:
   - **Drums** check three pattern bitmasks for the current substep.
   - **Bass** checks `bass_substeps = {0, 18, 24, 42}` and fires root or fifth, octave down.
   - **Chord** at substeps `{0, 12, 24, 36}` fires a 3-note voicing from a 6-pattern rotation (triad / 7th / sus4 / sus2 / inv1 / inv2), with each pitch octave-shifted toward the previous chord's centroid (voice leading).
   - **Melody** at every 3rd substep (the 16-step Euclidean grid maps onto 48 substeps with stride 3): two parallel Euclidean rhythms `E(k_a) | E(k_b)`, Markov walk over scale degrees, probability-gated.
   - **Counter-melody** on its own Euclidean `E(k_counter)` with an independent Markov walk, transposed +12 semitones.

### Scales

```
SCALES[6][7] = {
    /* Dorian          */ { 62, 64, 65, 67, 69, 71, 72 },
    /* Lydian          */ { 62, 64, 66, 68, 69, 71, 73 },
    /* Phrygian        */ { 62, 63, 65, 67, 69, 70, 72 },
    /* Locrian         */ { 62, 63, 65, 67, 68, 70, 72 },
    /* Harmonic Minor  */ { 62, 64, 65, 67, 69, 70, 73 },
    /* Mixolydian      */ { 62, 64, 66, 67, 69, 71, 72 },
};
```

Markov runs on degree indices (0..6), so a single 7×7 matrix applies to any 7-note scale. Only the degree-to-MIDI mapping changes when `cur_scale` rotates. Scale never auto-rotates; only the `s` key in live mode cycles it.

### Chord voicings

```
CHORD_PATTERNS[6][3] = {
    triad    : (0,0)  (2,0)  (4,0)     1 - 3 - 5
    seventh  : (0,0)  (2,0)  (6,0)     1 - 3 - 7    (drops 5 to fit 7)
    sus4     : (0,0)  (3,0)  (4,0)     1 - 4 - 5
    sus2     : (0,0)  (1,0)  (4,0)     1 - 2 - 5
    inv1     : (2,0)  (4,0)  (0,1)     3 - 5 - 1'   (3rd in bass)
    inv2     : (4,0)  (0,1)  (2,1)     5 - 1' - 3'  (5th in bass)
};
```

Each entry is `(degree, octave_offset)`. Pattern index = `bar_count % 6`. After choosing the pattern, each pitch is octave-shifted to stay within ±8 semitones of the running chord centroid — that is the voice-leading step.

### Drum patterns

```
kick_patterns[4]   - cycles per bar (basic 1+3, syncopated, 4-on-floor, off-kilter)
snare_patterns[3]  - classic 2+4, ghost-notes added, half-time
hihat_patterns[5]  - 8ths, 16ths, quarters, offbeats only, triplet feel
```

Each is a uint64 bitmask where bit N = trigger at substep N. Coprime bank sizes (4, 3, 5) → combined kit cycles every LCM(4, 3, 5) = 60 bars (~2 minutes) before exact repeat.

### Bass pattern

4 events per bar at substeps `0, 18, 24, 42` — beats 1 and 3 anchor the tempo, offbeats at "and of 2" and "and of 4" anticipate. Pitch alternates root/fifth with bar parity swapping the order.

### Cellular automata

Two CAs run in parallel:
- `ca_row` (Rule 110, class IV) — 32-bit row advanced once per bar. Low 7 bits become the active-degree mask.
- `ca_harm` (Rule 30, class III) — 32-bit row advanced every 12 substeps. ANDed with `ca_row & 0x7F` to filter degrees.

Pairing Rule 110 (long-period repeats alone) with Rule 30 (pure randomness alone) gives recurring structure with variation. If either CA collapses to zero it is reseeded.

### Markov chain

`markov[7][7]` of unnormalized `uint8_t` weights. `markov_next(cur, mask)` sums weights for columns in the active mask, draws `prng() % sum`, walks. Initial weights hand-tuned: stepwise motion weighted moderate, strong cadences (dominant/leading → tonic) weighted high, diagonal zero (no stuck self-transitions). `mutate()` modifies cells over time.

### Euclidean rhythm

True Bjorklund (recursive bucket merge). The 17-entry `euclid_table[]` holds `E(k, 16)` for `k = 0..16`. Two parameters `eucl_k_a` and `eucl_k_b` select two masks for the main melody; `eucl_k_counter` drives the counter-melody.

### Dynamic mutation rate

A triangle LFO sweeps the mutation interval between `MUTATE_MIN = 1` bar (busy section) and `MUTATE_MAX = 16` bars (calm section) over a 128-bar period (~4.3 min). At each bar boundary, `bars_until_mutate` decrements; on zero, `mutate()` fires and the counter reloads from the current LFO-derived interval. This gives natural alternation between dense and sparse passages.

### Mutation

Per call (`mutate()`):
1. Re-roll one cell of the Markov matrix.
2. Flip one bit of `ca_row` (with reseed if it collapses).
3. Bump one Euclidean k (alternating `eucl_k_a` / `eucl_k_b`).
4. With 25% probability: drift `gate_prob` by ±16, clamped to [64, 240].
5. With 50% probability: re-roll `eucl_k_counter`.

## Memory model

```
static uint8_t pool[131072] __attribute__((aligned(64)));   // 128 KB
static size_t bump;
```

`arena_alloc(n)` rounds to 8-byte alignment, bumps the cursor, exits on overflow. No `free` path. Typical startup usage:

```
Voices (11 * sizeof(Voice))   ~11.6 KB
Delay buffers (2 * 24 KB)        48 KB
Reverb buffers (12 * varying)    27 KB
Live render buffer                8 KB
                                ───────
                                ~95 KB / 128 KB available
```

## Determinism

With `--seed N`, all random state is derived from `N`. To avoid xorshift32's zero fixed point and to keep small seeds from colliding, `gen_seed` XORs the input with a constant before hashing:

```
s = hash32(N ^ 0xDEADBEEFu)
gen_prng_state = s
ca_row         = hash32(s)
ca_harm        = hash32(hash32(s))
voice.c PRNG (KS noise) = 0xCAFEBABE (fixed)
```

Without `--seed`, `gen_init` derives the seed from `time(NULL)` at startup, so each launch produces a different generative output but every audio sample of any specific run is fully determined by that initial time stamp.

`make test` renders 16 seconds with `--seed 0` twice, sha256-compares to verify byte-exact determinism, and checks the hash against `golden/regression_16s.sha256` to verify the algorithm hasn't drifted unexpectedly. See the **Testing** section below for the rest of the test surface.

## Live audio backends

### Linux: `pa_stream` + threaded mainloop

Uses the full PulseAudio API (`pa_threaded_mainloop` + `pa_context` + `pa_stream`) with `PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE` — the same architecture `paplay` uses. The main loop blocks on `pa_stream_writable_size` and wakes when the write callback fires. 300 ms target buffer (passed via `pa_buffer_attr.tlength`).

WSL2 + WSLg note: WSLg's RDP-based audio pipe is unreliable for sustained playback (multiple open GitHub issues against `microsoft/wslg`). Render to WAV and play on Windows, or run `stretto.exe` directly on Windows.

### Windows: Win32 `waveOut`

Four cycling buffers of 1024 frames each (~85 ms total latency). Opens via `waveOutOpen` with `CALLBACK_EVENT` so the main thread can wait on a single `HANDLE` for buffer completion — no callback function needed. After each `WHDR_DONE`, the freed buffer is filled by `render_chunk` and resubmitted via `waveOutWrite`. Links against `winmm.lib` only.

## Terminal UI

Platform-abstracted by four helpers in `main.c`:
- `term_get_size(&w, &h)` — Unix `ioctl(TIOCGWINSZ)` / Windows `GetConsoleScreenBufferInfo`
- `term_read_key(&ch)` — Unix non-blocking `read(0,...)` / Windows `_kbhit()` + `_getch()`
- `term_raw_mode()` — Unix `tcsetattr` / Windows `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING` for ANSI escapes
- `term_restore_mode()` — restore on exit

The oscilloscope draws each frame into a 24 KB static buffer (one `write()` syscall per frame to keep terminal I/O from blocking the audio loop) with ANSI 16-color escapes inline. Color escapes are RLE-emitted — only when intensity changes between cells — to keep per-frame byte count modest.

`--no-ui` skips `term_raw_mode` entirely so the program can run from scripts and CI (no TTY on stdin) without `tcgetattr` exiting it. When `--no-ui` is set, the oscilloscope and key handler are also skipped; the audio loop just renders and plays continuously until SIGTERM / Ctrl-C / SIGINT.

## Testing

| Target | Scope |
|---|---|
| `make test` | Bit-exact regression: render 16 s at `--seed 0` twice, byte-compare, then sha256 against `golden/regression_16s.sha256`. |
| `make test-unit` | ~50 unit tests across `tests/unit/test_arena.c`, `test_voice.c`, `test_gen.c` using the hand-rolled framework in `tests/unit/test.h`. |
| `make test-multiseed` | Renders 4 s at seeds 0 / 1 / 42 / 12345, asserts each is deterministic across runs, asserts all four produce distinct sha256s, asserts each render's peak / RMS / clip count lands within sane bounds (catches runaway-state bugs), then matches each hash against `golden/regression_multiseed.sha256.txt`. |
| `make test-smoke` | Spawns `./synth --no-ui` under a 2 s timeout. Pass on exit 0 / 124 / 143; fail on segfault. Auto-skips if no PulseAudio. |
| `make coverage` | Rebuilds instrumented (`-fprofile-arcs -ftest-coverage`), runs the regression + unit suites, prints per-file line coverage via `gcov`. |

The framework header `tests/unit/test.h` (~130 LOC) provides `TEST(name) {...}` registration via constructor attributes plus assertion macros (`ASSERT_TRUE` / `ASSERT_EQ` / `ASSERT_NE` / `ASSERT_NEAR` / `ASSERT_BETWEEN`). Each `tests/unit/test_*.c` links against `arena.o + voice.o + gen.o` (no `main.o`) and runs as a standalone binary.

Approximate line coverage:

| File | Coverage |
|---|---|
| `arena.c` | ~80% (OOM exit path excluded; not exercised by tests) |
| `voice.c` | ~98% |
| `gen.c` | ~97% |
| `main.c` | ~70% (live-audio + interactive UI excluded by design) |

CI (`.github/workflows/ci.yml`) runs every target on push and pull-request to `main`, plus the Windows cross-compile (`make winpack`). Coverage gate at 80% per file on `arena.c`, `voice.c`, `gen.c`. The Windows binary and coverage log are uploaded as build artifacts.

## Build details

Linux flags (`-Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections`) and `strip -s -R .comment` are standard. `make pack` runs UPX `--ultra-brute` on top for a final ~33% reduction.

Windows cross-compile uses `x86_64-w64-mingw32-gcc` with the same size flags. `make winpack` adds UPX. The packed Windows binary is ~30 KB — well under the 64 KB target from the original PLAN.md and the demoscene "tiny generative synth" tradition.
