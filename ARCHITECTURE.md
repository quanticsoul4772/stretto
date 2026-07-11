# Architecture

## Overview

`stretto` is a single-process audio synthesizer that mixes generated music to PulseAudio (Linux), Win32 `waveOut` (Windows), or a WAV file. All runtime allocations come from a single static 128 KB arena. The audio thread is the main thread; on Linux a `pa_threaded_mainloop` helper thread services the PulseAudio event loop, and on Windows the `waveOut` callback fires an event the main thread waits on. The audio path itself needs no cross-thread synchronization in our code; the one place we do synchronize is the `--midi` surface, where the platform MIDI producer (ALSA worker thread / winmm callback) hands events to the audio thread through the SPSC ring in `audio_midi.c` (C11 `__atomic` acquire/release).

| Binary | Size |
|---|---|
| Linux `synth` (stripped, links libpulse + libasound) | ~47 KB (48 584 B per `make synth` + `strip -s -R .comment`, measured 2026-07-11 on the 059 quality pass; STRIP_TARGET = 51 200 B) |
| Linux `synth.packed` (UPX) | ~25 KB (25 460 B per the PR #117 `binary-sizes` CI artifact — historical; grows with the stripped binary); PACK_TARGET = 30 720 B per the Makefile enforces the Constitution v1.2.x cap |
| Windows `stretto.exe` (stripped) | ~247 KB (252 928 B from `make win`, measured 2026-07-11; stripped — `-s` in `WIN_LDFLAGS` strips at link and the `stretto.exe` rule additionally runs `$(WIN_STRIP) -s -R .comment`) |
| Windows `stretto.packed.exe` (UPX) | ~38 KB (per the PR #117 `binary-sizes` CI artifact — historical); WIN_PACK_BUDGET = 49 152 B |

(Exact packed sizes vary per commit; the authoritative per-commit numbers are the `binary-sizes` artifact each CI run uploads, and the three budgets are gated on every push by `tools/size-budget-gate.sh`.)

## Spec-kit pipeline

Source-of-truth design + governance artifacts for the `001-stretto-baseline` capability surface live at `specs/001-stretto-baseline/`:

- `spec.md` — capability surface, requirements, measurable success criteria, edge cases
- `plan.md` — implementation plan + Constitution Check table (all ten principles PASS) + complexity tracking + project structure
- `research.md` — Phase 0 research synthesis: twelve architectural decisions (synthesis methods, generative state machine, audio sample rate + format, memory model, determinism, effects chain ordering, cross-platform audio, section state machine, live-mode UI, voice-family mask + INTRO randomness, coverage gates, deliberately-omitted test categories) with rationale and alternatives rejected
- `tasks.md` — seventy tasks organized by user story across Setup / Foundational / US1 / US2 / US3 / Polish phases; each task carries a `[P]` parallel marker and `[USn]` story label per the spec-kit task-generation format
- `quickstart.md` — minimal CLI usage reference surfaced here as a convenience even though the user-facing copy of the same info lives in this README + the linked plans

The MIDI input capability ([`specs/003-midi-input/spec.md`](./specs/003-midi-input/spec.md) — US1 Note On/Off, US2 CC modulation, US3 device listing) ships alongside the baseline:

- `spec.md` — FR-001..FR-054 + SC-001..SC-007; 4 clarifications (2026-07-06) lock polyphony = 11 voices w/ voice stealing, callback-only producer + audio-thread-consumer SPSC, no auto-reconnect on disconnect, CC initial state = 0
- `plan.md` — implementation plan + Constitution Check + the 4 file-system additions (`audio_midi.c`, `audio_midi_linux.c`, `audio_midi_winmm.c` + tests)
- `research.md` — 7 decisions: D1 libasound pthread pattern (snd_seq_create_thread is NOT a stock API), D2 winmm CALLBACK_FUNCTION, D3 C11 acquire/release for the SPSC ring, D4 Voice struct extension reusing 2 bytes of `_glide_pad` (zero-byte struct growth), D5 channel filter in audio thread (NOT producer callback), D6 dedicated `voice_pool_*_midi` entry points, D7 ring-buffer size 256 in arena
- `tasks.md` — 49 tasks across Setup / Foundational / US1 / US2 / US3 / Polish phases with `[P]` parallelism + `[USn]` story labels
- `quickstart.md` — listener usage + CLI flags + CC map + Linux/Windows platform notes + `--no-midi` byte-identity invariant
- `data-model.md` — 5 entities: `midi_event_t`, `midi_input_device_t`, `midi_queue_t`, `cc_map_entry_t` + `CC_MAP[128]`, `voice_pool_*_midi` + Voice `trigger_key`/`trigger_channel` discriminator

The ten architectural principles (I–X) are encoded in `.specify/memory/constitution.md` v1.2.1. Three are NON-NEGOTIABLE: I (Tiny Native Binary — ≤48 KB UPX-packed Windows, ≤30 KB UPX-packed Linux, ≤50 KB stripped Linux; the Linux caps were realigned to measured reality on 2026-07-08 by v1.1.0/v1.2.0 — the prior 24 KB / 12 KB figures were aspirational PLAN.md-era targets the shipped synth never met, and the 003 MIDI-input chain itself cost ~5 KB stripped / ~9.5 KB packed on top of the ~39 KB / ~16 KB pre-#109 baseline, per the v1.2.1 attribution correction), III (Deterministic — see amendment note in [Determinism](#determinism) below), VI (Test Discipline — per-file coverage gates). Amendments to the constitution follow the Governance clause and bump the version line. Prior wording-only amendment: Principle III → v1.0.1 (2026-07-06), which closed the wording gap exposed by `/speckit-analyze` finding D1 (Constitution vs spec SC-002 platform-scope wording).

## Module layout

```
main.c                  argv + dispatch only (189 LOC; grew from ~80 at 001-stretto-baseline due to the 003 chain's `--midi` / `--midi N` / `--midi-default` / `--midi-channel` / `--midi-list-devices` / `--no-midi` argv restructure in PR #109)
config.h                Project-wide constants (SAMPLE_RATE,
                        BUFFER_FRAMES). Single source of truth so
                        the runtime synth and the build-time
                        generators stay in sync.
mixer.c   / .h          render_chunk(): voice mix -> reverb -> delay
                        -> soft saturation. Single source of the
                        master-bus chain
wav.c     / .h          render_wav() + WAV header writer
ui.c      / .h          cross-platform terminal raw mode, oscilloscope
                        grid renderer, status row builder, help
                        overlay (Linux termios + Windows console)
keys.c    / .h          keys_dispatch(char): single source of truth
                        for the live-mode key map. Returns KEY_QUIT
                        / KEY_CONSUMED / KEY_IGNORED
audio.h                 one-function interface (audio_play())
audio_pulse.c           Linux live-audio backend (PulseAudio:
                        pa_threaded_mainloop + pa_stream)
audio_winmm.c           Windows live-audio backend (Win32 waveOut,
                        CALLBACK_EVENT + 4-buffer cycle)
audio_midi.c   / .h    cross-platform MIDI input surface (the 003-m-i
                        capability): SPSC ring buffer (audio thread is
                        sole consumer per FR-033), CC dispatch table
                        CC_MAP[128], Note On/Off routing via
                        voice_pool_trigger_midi / release_midi,
                        --no-midi opt-out path (preserves baseline
                        golden hash per FR-050 / FR-053)
audio_midi_linux.c     libasound ALSA sequencer backend (Linux):
                        snd_seq_open + one application port "Stretto
                        Input"; one pthread worker drains
                        snd_seq_event_input via poll(100 ms) cycle
                        and writes through audio_midi_enqueue;
                        PORT_EXIT / CLIENT_EXIT flip g_run atomic for
                        FR-034 graceful shutdown (no auto-reconnect);
                        snd_seq_query_next_client + _get_port_info
                        for enumerate_via_snd_seq_client_info
audio_midi_winmm.c     Win32 midiInProc backend (Windows): system
                        thread (midiInStart) installs CALLBACK_FUNCTION
                        callback; 4-buffer sysex pool (4096 B each)
                        prepared + added at init; MIM_DATA parses short
                        messages into the same midi_event_t shape; MIM_CLOSE
                        drives the FR-034 disconnect path; midiInGetNumDevs +
                        midiInGetDevCaps for enumerate_via_midiInGetDevCaps
voice.c / .h            Voice struct (KS / FM / wavetable / additive /
                        super-saw / drum), ADSR, SVF, super-saw glide,
                        per-voice peak normalization, role-scoped
                        voice pool of 11 slots
effects.c / .h          master-bus delay (250 ms stereo), Schroeder
                        reverb (4 combs + 2 all-passes/channel),
                        soft cubic saturation, sat16 clamp
gen.c   / .h            sample clock, six scales, Rule-110 + Rule-30
                        CAs, counter-melody Markov chain, Bjorklund
                        Euclidean rhythm. gen_step is a ~25-LOC
                        dispatcher that delegates to five per-concern
                        static-inline schedulers (schedule_bar_boundary,
                        schedule_drums, schedule_bass, schedule_chord,
                        schedule_melody) plus compute_active_mask.
lsystem.c / .h          L-system phrase generator for the main melody
                        (6-symbol alphabet, 3 hand-tuned characters,
                        3 generations of rewrite into a 256 B buffer)
chord_progression.c/.h  Markov chain over chord functions; chord root
                        advances every 2 bars
section.c / .h          Song-section state machine (intro / body /
                        tension / resolve), 96-bar cycle, crossfades
                        biases (gate, cutoff, reverb wet, mutation
                        interval) across an 8-bar window centered on
                        each boundary; pins discrete biases (kick
                        pattern, L-system character)
density.c / .h          Adaptive density: tension = popcount(active)
                        * 18 + gate >> 2. Counter-cyclical biases
                        (gate +/-16, reverb wet +/-32) sum on top of
                        section biases at the gen.c call sites
motif.c   / .h          Long-term motif memory: ring buffer of the
                        last 8 four-bar main-melody phrases. Every
                        ~30 bars with ~25% per-bar probability,
                        replay one (verbatim or +/-2 diatonic
                        transpose) in place of L-system output for
                        the next 4 bars. Pure function of caller-
                        supplied bar count + PRNG values
arena.c / .h            static pool[131072], 8-byte-aligned bump allocator
gen_sin_table.c         build-time: 1024-entry int16 sine LUT (peak 24576)
gen_env_table.c         build-time: 256-entry uint8 exponential env curve
gen_note_table.c        build-time: 128 MIDI notes -> {phase inc, KS len}
                        at the SAMPLE_RATE from config.h
gen_euclid_table.c      build-time: 17 16-bit Bjorklund masks E(0..16, 16)
```

Dependency direction is strictly one-way:

```
main.c -> {wav, audio, ui, gen, effects, voice, audio_midi, arena}
audio_pulse.c / audio_winmm.c -> {mixer, ui, keys, arena}
audio_midi.c -> {effects, voice, arena}            # CC dispatch + voice_pool_trigger_midi + opt-out
audio_midi_linux.c -> {audio_midi}                 # producer (pthread) -> audio_midi_enqueue
audio_midi_winmm.c -> {audio_midi}                 # producer (midiInProc) -> audio_midi_enqueue
wav.c -> {mixer, arena}
mixer.c -> {gen, voice, effects, audio_midi}       # audio_midi_drain at top of render_chunk
keys.c -> {ui, gen, voice, effects}
ui.c -> {voice, gen, effects}
gen.c -> {voice, lsystem, chord_progression, section, effects}
voice.c -> {arena, effects (for sat16), build-time tables}
```

No reverse calls; no extern declarations across module boundaries; no weak symbols (the old `main_set_reverb_wet_bias` weak-stub workaround was eliminated when the master-bus moved into `effects.c`).

The `gen_*` programs run during `make` and emit C headers (`sin_table.h`, `env_table.h`, `note_table.h`, `euclid_table.h`, `wavetable.h`) that are `#include`d by `voice.c` and `gen.c`. Tables end up in `.rodata` of the final binary; they do not consume arena space. A sixth generated header, `version.h` (`#define STRETTO_VERSION` from `git describe`, consumed only by `main.c` for `--version`), is written by a compare-and-swap Makefile rule: the recipe runs on every `make`, but the file's mtime only changes when the version does, so incremental builds stay no-ops and a version change rebuilds exactly `main.o`.

## Audio path

```
                                 audio_midi_drain (top of render_chunk, audio thread)
                                             │
                                             │ NOTE_ON / NOTE_OFF / CC →
                                             │ voice_pool_trigger_midi / _release_midi / adjust_*
                                             ▼
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
| 0–1 | Bass | Super-saw subtractive (3 detuned saws) + glide | A 50 ms / R 1000 ms | Center |
| 2–4 | Chord | Section-selected: wavetable / additive / FM | A 20 ms / R 600 ms | L / C / R |
| 5–7 | Melody | KS or FM alternating | A 5 ms / R 600 ms | Outer L / R |
| 8 | Kick | DRUM, sine sweep + click | A 1 ms / linear R 150 ms | Center |
| 9 | Snare | DRUM, noise + 200 Hz body | A 0.5 ms / linear R 100 ms | Slight R |
| 10 | Hihat | DRUM, white noise | A 0.5 ms / linear R 30 ms | Slight L |

Voice stealing (`pick_slot_range`) only searches within a role's reserved range, so a chord trigger never displaces a bass voice. Drum slots are dedicated per drum type — no stealing inside the kit.

MIDI Note On bypasses the role-slot ranges entirely: `voice_pool_trigger_midi` walks the full `N_VOICES` pool directly (per Clarifications 2026-07-06 Q1, polyphonic with voice-stealing — first `ENV_OFF`, then oldest in release, then oldest regardless) so a MIDI-held chord never displaces a generative bass voice and vice versa. The two schedulers share the slot array but not the slot-selection rules. Identification for Note Off matching is via the `trigger_key` / `trigger_channel` discriminator on each `Voice` (zero-byte struct growth; preflight D4 reuses two bytes of `_glide_pad`).

The chord row's synthesis method is chosen per section by `section_chord_voice_type` (wavetable in INTRO/RESOLVE, additive in BODY, FM in TENSION). Which roles are *audible* is gated by the per-section voice-family mask (`section_voice_mask`) — see [Song-section state machine](#song-section-state-machine).

## Voice struct

```
type            VOICE_OFF | VOICE_KS | VOICE_FM | VOICE_DRUM
                | VOICE_WT | VOICE_ADD | VOICE_SUB
note            MIDI 0..127 (or drum sub-type for VOICE_DRUM)
env_phase       ENV_OFF | ENV_A | ENV_D | ENV_R
role            ROLE_BASS | ROLE_CHORD | ROLE_MELODY | ROLE_DRUM
pan             0 = full L, 128 = center, 255 = full R
env_amp         0..32767, ADSR amplitude
env_time        sample count since current phase started
lfo_phase, lfo_inc       per-voice pan LFO (also drives FM pitch detune
                         and the wavetable position sweep)
peak_seen, gain, peak_window   per-voice peak normalization state
svf_lp, svf_bp           int32 SVF state
fenv_*                   chord filter-envelope state
inc_target, glide_remain super-saw bass portamento (glide) state
union:
  ks    { int16 buf[512], idx, len }
  fm    { uint32 phase_c, phase_m, inc_c, inc_m; uint16 mod_depth }
  drum  { uint32 phase, inc; uint8 drum_type }
  wt    { uint32 phase, inc; uint16 position }
  add   { uint32 phase[8], inc_base; const uint8 *amps }
  sub   { uint32 phase[3], inc[3] }
```

`voice_step` advances one sample of one voice: an optional glide ramp (super-saw bass), then the raw oscillator (KS / FM / wavetable / additive / super-saw / drum), envelope multiplication, SVF, per-voice peak-normalization gain, and finally a per-drum-type post-normalization boost (kick 3×, snare 2.5×, hihat 1.5×) so percussion sits on top of the harmonic content.

`voice_pool_mix` calls `voice_step` for all 11 voices, applies per-voice pan (with slow LFO modulation) to produce L and R contributions, sums into int32 per channel, and returns a `Stereo` pair.

## Synthesis details

### Karplus-Strong (melody, sometimes)

On trigger, the voice's circular buffer of length `note_ks_len[note]` is filled with half-amplitude white noise. Each step outputs the head sample, then writes back the damped average of two adjacent samples (damp factor ≈ 0.99). The result is the classic plucked-string timbre.

### 2-op FM (chord / melody)

Two uint32 NCOs share the sine LUT. Modulator output scales by `mod_depth` and offsets the carrier phase. `inc_m = inc_c * ratio` where ratio is 2:1 for chord/melody (bell-like). (Bass formerly used a 1:1 FM; it is now super-saw — see below.)

Per-voice LFO pitch detune (~5 cents peak) is layered on top of FM by reusing the pan LFO — same LFO sample, applied to both `inc_c` and `inc_m` proportionally so the FM ratio stays constant.

### Wavetable (chord, INTRO/RESOLVE)

Reads from `WAVETABLE[8][256]` — 8 single-cycle waveforms built at compile time by `gen_wavetable.c` (sine → harmonic-rich → saw/square/pulse → inharmonic bell). A "position" value selects between adjacent waveforms with linear interpolation; position is swept by the per-voice pan LFO, so the timbre morphs continuously through all 8 waves over ~10 s — an animated pad. No extra modulator state: position is derived from `lfo_phase` in `voice_step`.

### Additive (chord, BODY)

Sums 8 sinusoidal partials at integer multiples of the fundamental (8 phase accumulators stepping at k×`inc_base`), each weighted by one row of `ADD_PROFILES[4][8]` — drawbar-style amplitude profiles (Hammond / square / strings / brass). The weighted sum is `>>8` back into int16 range; `sat16` catches the loudest profile's overshoot. Steady, organ-like character.

### Super-saw subtractive (bass)

Sums 3 band-limited saw oscillators (reusing `WAVETABLE[4]`, the band-limited saw) at the fundamental and at ±≈0.78 % detune (`inc ± inc>>7`), averaged, then fed through the existing per-voice SVF. The slow beating between the three detuned copies gives the thick, wide super-saw bass that a single oscillator can't.

**Glide (portamento).** When a bass note re-triggers while the previous note's amplitude envelope is still above half the sustain level (`env_amp > GLIDE_LEGATO_THRESH`), `voice_trigger` does not hard-restart: it sets `inc_target` to the new note and `glide_remain` to `GLIDE_SAMPLES` (~50 ms), keeping phases/envelope/SVF state. `voice_step`'s `glide_advance` then walks `inc[0]` linearly toward `inc_target` over the remaining samples (rebuilding the ±detune each step), landing exactly on pitch when the counter hits 0. Deeper into the release tail the threshold fails and a normal hard re-trigger happens. No extra PRNG draws — determinism holds.

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

Cubic transfer curve `y = x - x^3 / 2^31`. Linear for small `x`; gracefully compresses peaks. Per-channel, second-to-last in the chain (before the compressor) so any peaks reverb / delay introduce are smoothed before dynamics processing.

### Compressor + brickwall limiter

Feed-forward, stereo-linked, runs last in the chain (after `saturate_process`, before `sat16`). Envelope follower tracks `max(|L|, |R|)` so both channels get identical gain reduction (preserves stereo imaging). One-pole IIR with asymmetric coefficients: **~5 ms attack**, **~200 ms release** at 48 kHz. **4:1 ratio** above threshold, **+1 dB makeup gain**, **brickwall ceiling at 32 000** (below int16 max so `sat16` still has margin).

Threshold runtime-tunable in `[8000, 30000]` via `l` / `L` keys; default 20 000. Status row shows current threshold as `Lm:<n>`.

Effect: tames transient stabs (kick on TENSION sections in particular), brings up quiet content slightly via makeup gain, and guarantees no output sample reaches int16 boundary — clip count goes from "≤100 per render" to zero on every seed.

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

Scale-degree indices (0..6) abstract the generators (Markov for counter-melody, L-system for main melody, chord-progression Markov for chord root) from concrete MIDI pitches. Only the degree-to-MIDI mapping changes when `cur_scale` rotates. Scale never auto-rotates; only the `s` key in live mode cycles it.

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

Each entry is `(degree, octave_offset)` measured **above the current chord root** (see Chord progressions below). Pattern index = `bar_count % 6` (rotates voicing every bar). The resolved degree is `(pat[i].degree + current_chord_root) % 7`, then mapped to MIDI via `SCALES[cur_scale][...]`. Each pitch is finally octave-shifted to stay within ±8 semitones of the running chord centroid — that is the voice-leading step.

### Chord progressions

`chord_progression.c` holds the current chord function as a single `uint8_t current_root` in [0, 6]. The root advances once every two bars via a Markov chain over chord functions; chord triggers within those two bars share the same root.

Two 7×7 weight tables (`uint8_t` each, totaling 98 B of `.rodata`):

- `CHORD_MARKOV_MAJOR` — used for Lydian (`cur_scale=1`) and Mixolydian (`cur_scale=5`). Cadences (V→I, IV→I, vii°→I, ii→V) weighted highest. Diagonal weights are nonzero so the synth can sit on a chord across multiple advances.
- `CHORD_MARKOV_MINOR` — used for Dorian, Phrygian, Locrian, Harmonic Minor. Modal motion: VII↔i, iv↔i, weaker dominant pull than major-mode tables.

`chord_progression_step(rng, scale)` is called from `gen.c` once at the start of every even bar. It sums the source row, draws `rng % sum`, walks. Module is one-way coupled: gen.c passes `prng()` output and `cur_scale` in; the module never reads gen.c file-scope state.

Bass also reads `chord_progression_get_root()` so its root/fifth alternation (substeps 0, 18, 24, 42) tracks the current chord function rather than always playing scale-degree 0/4.

### L-system melodic phrase generator

`lsystem.c` produces the main melody's degree sequence. Replaces the older Markov walker for that voice; counter-melody still uses Markov so the two lines contrast (phrased vs walked).

Alphabet (6 symbols, 1 byte each):

```
SYM_UP   move pointer +1   SYM_UP2  move +2 (leap)
SYM_DN   move pointer -1   SYM_DN2  move -2 (leap)
SYM_REP  no move           SYM_REST emit rest (caller skips trigger)
```

The grammar is one of three hand-tuned **characters** (stepwise / leaping / sparse). Each character has a 6-rule production table. `lsystem_reset()` expands the axiom for 3 generations using the current character into a 256-byte output buffer. `lsystem_next(active_mask)` reads the next symbol, advances a scale-degree pointer wrapped to [0, 6], snaps to the nearest in-mask degree, and returns it. `LSYSTEM_REST` is returned for the rest symbol; the caller in `gen.c` skips the melody trigger for that Euclidean hit (gives the melody breathing room).

`lsystem_mutate(rng)` is called by `mutate()` with ~33% probability per event:
- ~50%: re-roll one rule's RHS in the current character.
- ~25%: cycle to the next character.
- ~25%: swap one symbol in the axiom.

After mutation, `lsystem_reset()` re-expands so the next phrase reflects the new grammar.

Memory cost: ~410 B static state (3 chars × 6 rules × 7 B + axiom + 256 B output buffer + pointer + state).

### Drum patterns

```
kick_patterns[4]   - cycles per bar (basic 1+3, syncopated, 4-on-floor, off-kilter)
snare_patterns[3]  - classic 2+4, ghost-notes added, half-time
hihat_patterns[5]  - 8ths, 16ths, quarters, offbeats only, triplet feel
```

Each is a uint64 bitmask where bit N = trigger at substep N. Coprime bank sizes (4, 3, 5) → combined kit cycles every LCM(4, 3, 5) = 60 bars (~2 minutes) before exact repeat.

### Bass pattern

4 events per bar at substeps `0, 18, 24, 42` — beats 1 and 3 anchor the tempo, offbeats at "and of 2" and "and of 4" anticipate. Pitch alternates root/fifth with bar parity swapping the order. Root and fifth are computed relative to the **current chord function** from `chord_progression`, not always scale degree 0/4.

### Cellular automata

Two CAs run in parallel:
- `ca_row` (Rule 110, class IV) — 32-bit row advanced once per bar. Low 7 bits become the active-degree mask.
- `ca_harm` (Rule 30, class III) — 32-bit row advanced every 12 substeps. ANDed with `ca_row & 0x7F` to filter degrees.

Pairing Rule 110 (long-period repeats alone) with Rule 30 (pure randomness alone) gives recurring structure with variation. If either CA collapses to zero it is reseeded.

### Markov chain (counter-melody)

`markov2[7][7][7]` — **2nd-order** Markov: indexed by `[prev_prev][prev][next]`. 343-byte runtime table seeded by replicating the 1st-order `MARKOV_SEED[7][7]` across the prev_prev axis at `gen_init` time so day 1 produces the same musical character as the original 1st-order chain. `mutate()` drifts individual cells, so distinct `(prev_prev, prev)` contexts evolve different transition tendencies over the piece.

Used **only for the counter-melody** since the L-system phrase generator replaced Markov on the main melody. The walker (`markov2_next_voiced`) is biased against the main melody's most recent degree (`cur_degree`) using a per-interval consonance factor:

| interval (degrees) | bias × | musical meaning |
|---|---|---|
| 0 | 0 | unison — avoided (counter is +12, same pitch class) |
| 1 (2nd / 7th) | 64 | mild dissonance, low weight |
| 2 (3rd / 6th) | 192 | preferred consonance |
| 3 (4th / 5th) | 128 | neutral |

Effect: counter-melody sounds responsive to the main line instead of an independent parallel stream. The fallback when the per-bias row sums to zero is to return `prev` unchanged (same safety net as the un-biased `markov2_next`).

### Long-term motifs

`motif.c` holds an 8-slot ring buffer of 64-degree arrays (`MOTIF_NO_NOTE = 0xFF` for empty positions corresponding to gate-suppressed Euclidean hits). Each phrase is 4 bars × 16 Euclidean slots.

State machine ticked per bar by `motif_bar_step(bar, rng)`:

- **Capture mode** (default): L-system drives the main melody; each fired degree is recorded via `motif_record(step_in_bar, degree)`. When the current phrase fills (4 bars), advance to the next ring slot and clear it.
- **Replay-trigger gate**: after `MOTIF_REPLAY_MIN_GAP = 30` bars since last replay AND `(rng & 0xFF) < 64` (≈ 25% per bar), enter replay mode. Pick a random ring slot (avoiding the currently-capturing one). Pick a transpose from `{0, 0, +2, −2}` (50% verbatim, 25% +2, 25% −2).
- **Replay mode**: `motif_replay_at(step_in_bar)` returns the recorded degree (transposed mod 7) or `MOTIF_NO_NOTE`. Caller in `schedule_melody` snaps to the current active mask before triggering. After `MOTIF_PHRASE_BARS = 4` ticks the state returns to capture, overwriting the next ring slot.

Counter-melody continues its 2nd-order Markov walk during replay (inter-voice listening still biases away from unison). Status row shows `Mo:c` / `Mo:r`.

Memory cost: 512 B ring buffer + ~13 B state.

### Euclidean rhythm

True Bjorklund (recursive bucket merge). The 17-entry `euclid_table[]` holds `E(k, 16)` for `k = 0..16`. Two parameters `eucl_k_a` and `eucl_k_b` select two masks for the main melody; `eucl_k_counter` drives the counter-melody.

### Dynamic mutation rate

A triangle LFO sweeps the mutation interval between `MUTATE_MIN = 1` bar (busy section) and `MUTATE_MAX = 16` bars (calm section) over a 128-bar period (~4.3 min). At each bar boundary, `bars_until_mutate` decrements; on zero, `mutate()` fires and the counter reloads from the current LFO-derived interval. This gives natural alternation between dense and sparse passages. The current section's `mutation_interval` bias is added on top, so TENSION sections fire mutations faster than INTRO.

### Song-section state machine

`section.c` runs a 96-bar cycle of four sections — INTRO (24 bars) → BODY (24) → TENSION (24) → RESOLVE (24) — that biases gate density, filter cutoff, reverb wet, mutation interval, and pins drum-kick pattern, L-system character, chord voice type, chord playback mode, and the voice-family mask. The continuous biases and the discrete pins are a pure function of `bar_count`; the INTRO voice-mask combo is the one PRNG-driven choice (drawn once per cycle), which keeps `--seed N` reproducibility intact.

| Bias | INTRO | BODY | TENSION | RESOLVE | Type |
|---|---|---|---|---|---|
| `gate` | −64 | 0 | +32 | −16 | crossfaded |
| `cutoff` | −40 | 0 | +30 | −10 | crossfaded |
| `reverb wet` | +40 | 0 | −20 | +20 | crossfaded |
| `mutation interval` | +8 | 0 | −4 | +4 | crossfaded |
| `kick pattern idx` | 0 | 0 | 2 | 0 | discrete |
| `L-system character` | sparse | stepwise | leaping | stepwise | discrete |
| `chord voice type` | wavetable | additive | FM | wavetable | discrete |
| `chord playback` | block | block | arpeggio | block | discrete |
| `voice mask` | 1–3 (random) | full | full | drumless | discrete |

Continuous biases interpolate linearly across an 8-bar window centered on each boundary: the last 4 bars of a section blend toward the next, the first 4 bars finish the blend. At the boundary exactly the value is halfway between adjacent sections. Discrete biases switch instantly. 10-minute renders have audible long-form shape — intro opens sparse (one of 8 curated 1–3-voice combos, chosen per cycle), body fills out, tension feels dense and bright (arpeggiated chords), resolve settles back and drops the drums.

**Voice-family mask.** `section_voice_mask` returns a 7-bit field (kick/snare/hat/bass/chord/melody/counter). BODY and TENSION are `VF_ALL`; RESOLVE clears the three drum bits; INTRO returns one of `INTRO_COMBOS[8]`, selected by `section_set_intro_combo` from a `prng()` draw at each cycle boundary (and once in `gen_init` for the opening). The schedulers in `gen.c` consult the mask before triggering — but **only the trigger calls are gated**, never the PRNG / L-system / Markov / motif state updates, so masking a voice silences it without altering the generative trajectory of the rest of the piece.

Status row shows the current section as `Sec:<name>` (intro / body / tens / res) so the listener can see boundaries align with the audible changes.

### Adaptive density

`density.c` derives a per-bar tension scalar from the current bar-stable active mask and the user gate probability, then biases gate and reverb wet **counter-cyclically** — busy textures pull back a touch, sparse textures fill in a touch. Energy self-balances over bar timescales while staying responsive to the section state machine.

```
tension = popcount(ca_row & 0x7F) * 18 + gate_prob >> 2;   /* 0..189 */
gate_bias   = (128 - tension) / 8   /* approx +/-16 */
reverb_bias = (128 - tension) / 4   /* approx +/-32 */
```

Composes with `section.c` additively. Both reverb biases sum and are pushed to `effects.c` via `reverb_set_wet_bias`; the gate bias adds to the section + user values at the melody trigger's clamp step. Density is a pure function of the current bar's CA + gate inputs — no PRNG, no persistent state beyond the cached tension. Status row shows the tension as `Td:<n>` (yellow).

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

**Cross-platform bit-exactness scope (Constitution Principle III v1.0.1).** With `--seed N`, the same render on any of the supported build targets (Linux glibc + Windows winmm, both little-endian x86) produces byte-identical output by code construction: the runtime engine is integer-only across voice / gen / mixer / effects (no `double` / `float` in any hot path), the PRNG is `xorshift32` over `uint32`, the build-time `gen_*_table.c` programs round doubles to committed-header bytes via the deterministic IEEE-754 `(int)(x + 0.5)` contract, and `wav.c` emits native-endian RIFF (`fwrite(&uint16/uint32, ...)`) which is little-endian on both supported targets. The `'--render'` path bypasses `audio_pulse.c` / `audio_winmm.c` entirely — those files affect live playback only, not the bit-exact regression. The bit-exact regression is gated on the Linux CI runner only; a Windows-side regression runner is not currently in CI (the invariant holds by code construction, not by automated cross-platform test). See `.specify/memory/constitution.md` Principle III v1.0.1 and the spec-kit pipeline section above for the precise invariant claim.

## Live audio backends

### MIDI input (`--midi` cross-platform surface)

[`audio_midi.c`](./audio_midi.c) is the cross-platform SPSC ring + dispatch layer; the platform backends below produce into it, and the audio thread (sole consumer per FR-033) drains it at the top of every `mixer.c:render_chunk` call. With `--no-midi` (the default), `audio_midi_init(-1)` sets a file-scope `enabled = 0` flag so the entire MIDI stack is bypassed; `render_chunk` is byte-identical to baseline (the `golden/regression_16s.sha256` regression still matches; FR-050 / FR-053). The capability spec is [`specs/003-midi-input/spec.md`](./specs/003-midi-input/spec.md); the bench-validation runbook is [`scripts/midi-smoke.md`](./scripts/midi-smoke.md).

**Producer (ALSA, Linux)** — `audio_midi_linux.c` opens a `SND_SEQ_OPEN_INPUT` handle with the client name `Stretto` and one generic application port `Stretto Input` (end-users subscribe their controller with `aconnect <controller> 129:0`). A single `pthread_create` worker blocks on `snd_seq_event_input` via a 100 ms `poll` / `snd_seq_poll_descriptors` cycle and writes through `audio_midi_enqueue`. Per event: `NOTEON` / `NOTEOFF` / `CONTROLLER` parse into a `midi_event_t` (ALSA channel 0..15 → MIDI 1..16, key / value from structured `ev->data.{note, control}` fields); `SND_SEQ_EVENT_PORT_EXIT` / `SND_SEQ_EVENT_CLIENT_EXIT` flip a cooperative-shutdown atomic (`g_run`) so the worker returns within one poll window (`pthread_join` then completes the close — FR-034 graceful disconnect, **no auto-reconnect**). Sysex / clock / pitch-bend / program-change / active-sensing fall through silently (queue slots are precious; 256 entries per FR-040). Enumeration (`audio_midi_linux_list_devices`) walks `snd_seq_query_next_client` + `snd_seq_query_next_port` with the same dual filter as the wildcard open path (`SND_SEQ_PORT_CAP_READ` + `SND_SEQ_PORT_TYPE_MIDI_GENERIC` — the once-documented `MIDI_KEYBOARD` constant does not exist in upstream alsa-lib), so the list shows exactly what `--midi` would subscribe to; each entry's index is the `(client<<8)|port` encoding that `audio_midi_linux_init` decodes.

**Producer (winmm, Windows)** — Win32 manages the service thread. `audio_midi_winmm.c` opens the device via `midiInOpen(... CALLBACK_FUNCTION)`, prepares + adds a 4-buffer pool of 4096 B sysex headers (`MIDIHDR` re-prepared + re-added on each `MIM_LONGDATA` so the pool keeps delivering), then arms the input flow with `midiInStart`. Each `MIM_DATA` callback parses the short message (`status & 0xF0` selects Note On / Off / CC; channel `+1` from 0..15 to MIDI 1..16; data1 = key / CC#; data2 = velocity / value) into the same `midi_event_t` shape and writes via the same `audio_midi_enqueue`. `MIM_CLOSE` triggers the symmetric FR-034 path. Sysex payload contents are discarded in v1 (SysEx is out of scope). `MIM_ERROR` / `MIM_LONGERROR` are surfaced as drops counted by `audio_midi_drop_count`. Enumeration (`audio_midi_winmm_list_devices`) walks `midiInGetNumDevs` + `midiInGetDevCaps`, populating `midi_input_device_t { .index, .name = truncated_szPname }`.

**Consumer (audio thread, both platforms)** — `audio_midi_drain` runs at the top of `render_chunk`. One acquire-load of `q.head` per chunk; the loop then acquire-loads `q.tail`, dispatches one event, and release-stores `tail + 1` — per event, not one bulk store at the end (single-consumer invariant per FR-033 — the audio thread is never blocked on platform I/O; researchers P1 documented that `__atomic_*` is the C11 acquire/release model not `volatile`). Per event: if `q.channel_filter != 0 && ev.channel != q.channel_filter` → silent drop (FR-004 + preflight M1 fix — the filter lives in the audio thread, NOT in the producer callback, so a multi-thread callback race cannot leak disallowed events past the filter). Then dispatch by `type`:

- **Note On** (incl. velocity 0 ⇒ Note Off per FR-011) → `voice_pool_trigger_midi(scaled_note, velocity, channel)` where `scaled_note = SCALES[cur_scale][K%7] + clamp(K/7 - 5, -2, +4) * 12` (octave clamp + preflight H2 fix). Synth voice = `VOICE_FM` (mirroring the live-mode melody handler's per-step FM/KS alternation; fixed at FM for MIDI since external triggers are not on the 16-step Euclidean grid). Velocity carries into the output through the voice's peak-normalization `gain` (env_amp is overwritten every sample by `env_step`, so scaling it directly would be undone): `gain = velocity * 256 / 127`, clamped `[64, 1024]` (minimum-audible floor / `PEAK_GAIN_MAX` 4× ceiling per `voice.c:voice_pool_trigger_midi`).
- **Note Off** → `voice_pool_release_midi(key, channel)` matching by the `trigger_key` + `trigger_channel` discriminator on each `Voice`. Sets `env_phase = ENV_R; env_time = 0;`; no-op if already `ENV_R` (FR-013) or no match (FR-012 unmatched-key no-op).
- **CC** → lookup `CC_MAP[ev.key]`. If `target == CC_TARGET_NONE` → silent drop (CC#0, #10, #16, #17, #19, #64, #123 all unassigned per Principle VII). Else `delta = ((int)ev.value - 64) * entry.scale` and call the corresponding `adjust_*`. Multiple CCs targeting the same parameter sum additively per FR-022 (`adjust_*` composes over the prior call).

### CC mapping table (FR-020)

The 128-entry `CC_MAP` lives in `.rodata` of `audio_midi.c` (512 B total). Per Clarifications 2026-07-06 Q4, all CC values start at 0 at synth launch — the first CC message from the controller sets the actual value; a knob moved before launch is not reflected until the user wiggles it (avoids the controller-inquiry round-trip not all hardware supports).

| CC#        | Target                              | Scale | Max swing (V = 0..127)             |
|------------|-------------------------------------|-------|------------------------------------|
| 1          | per-voice filter cutoff             | +1    | ±63 against [30, 180] base         |
| 7          | master compressor threshold         | +60   | ±3780 against [8000, 30000] base   |
| 71         | per-voice filter resonance          | +1    | ±63 against [0, 180] base          |
| 74         | per-voice filter cutoff             | +1    | sums with CC#1 per FR-022          |
| 91         | master reverb wet                   | +1    | ±63 against [0, 256] base          |
| 93         | master delay wet                    | +1    | sums with `d` / `D` key live-edit  |

`CC_TARGET_NONE` slots (silently dropped, no `fprintf`, no callback overhead; all 7 zero-initialized via the C99 designated-initializer `[N]={}` form so the table footprint is exactly 512 B in `.rodata` regardless of populated slot count):

- **CC#0** / **CC#10** (Bank Select MSB / Pan) — common wheel/encoder assignments on hardware controllers; explicitly out of scope for v1.
- **CC#16 / CC#17 / CC#18 / CC#19** (General Purpose Controllers 1–4) — controller-specific use; deliberately unassigned so explicit CC routing stays in the 6-way table above.
- **CC#64** (Sustain Pedal) — **unassigned in v1 per Principle VII; sustained key-release will release immediately (no pedal-aware hold state implemented — gap, not a design choice)**. Hardware pedals are ignored; the voice pool releases on the matching Note Off per FR-012 regardless of pedal position. A future spec can wire sustain semantics without breaking CC table compatibility.
- **CC#123** (All Notes Off) — v1 relies on the MIDI 1.0 standard's NOTE_ON V=0 mechanism (FR-011) for release propagation; CC#123 as a parallel "panic" command is not routed.

Voice synthesis methods consumed by MIDI and the generative scheduler are documented in [Synthesis details](#synthesis-details): `VOICE_KS` (Karplus-Strong plucked-string for melody slot 5–7 alternation), `VOICE_FM` (2-op FM with shared sine LUT — the direct voice for MIDI Note On), `VOICE_SUB` (super-saw bass + glide portamento for legato re-triggers, role BASS slots 0–1), plus `VOICE_WT` / `VOICE_ADD` (wavetable / additive chord voices, section-selected) and `VOICE_DRUM` (kick / snare / hihat kit). The MIDI dispatch uses the same `Voice` struct as the generative engine — only the slot-selection paths differ (see [Voice pool](#voice-pool-11-slots-4-roles)).

### Linux: `pa_stream` + threaded mainloop

Uses the full PulseAudio API (`pa_threaded_mainloop` + `pa_context` + `pa_stream`) with `PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE` — the same architecture `paplay` uses. The main loop blocks on `pa_stream_writable_size` and wakes when the write callback fires. 300 ms target buffer (passed via `pa_buffer_attr.tlength`).

WSL2 + WSLg note: WSLg's RDP-based audio pipe is unreliable for sustained playback (multiple open GitHub issues against `microsoft/wslg`). Render to WAV and play on Windows, or run `stretto.exe` directly on Windows.

### Windows: Win32 `waveOut`

Four cycling buffers of 1024 frames each (~85 ms total latency). Opens via `waveOutOpen` with `CALLBACK_EVENT` so the main thread can wait on a single `HANDLE` for buffer completion — no callback function needed. After each `WHDR_DONE`, the freed buffer is filled by `render_chunk` and resubmitted via `waveOutWrite`. Links against `winmm.lib` only.

## Terminal UI

Platform-abstracted by four helpers in `ui.c`:
- `ui_term_get_size(&w, &h)` — Unix `ioctl(TIOCGWINSZ)` / Windows `GetConsoleScreenBufferInfo`
- `ui_term_read_key(&ch)` — Unix non-blocking `read(0,...)` / Windows `_kbhit()` + `_getch()`
- `ui_term_raw_mode()` — Unix `tcsetattr` / Windows `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING` for ANSI escapes
- `ui_term_restore_mode()` — restore on exit

The oscilloscope draws each frame into a 24 KB static buffer (one `write()` syscall per frame to keep terminal I/O from blocking the audio loop) with ANSI 16-color escapes inline. Color escapes are RLE-emitted — only when intensity changes between cells — to keep per-frame byte count modest.

`--no-ui` skips `term_raw_mode` entirely; when set, the oscilloscope and key handler are also skipped and the audio loop just renders and plays continuously until SIGTERM / Ctrl-C / SIGINT. The flag is no longer *required* for redirected invocations: `ui_term_raw_mode` itself degrades to headless when stdin or stdout is not a TTY (POSIX `isatty` check, printing a one-line stderr notice — exact parity with the Windows path, where `GetConsoleMode` failure has always flipped the same flag), so `./synth < /dev/null` or `./synth > log` runs headless instead of dying on the ENOTTY `tcgetattr`.

**Signal-safe terminal restore (Linux).** atexit handlers do not run on signal death, so `audio_play()` installs SIGINT / SIGTERM / SIGHUP / SIGQUIT handlers (`ui_install_signal_handlers`, `ui.c`) that restore the terminal from signal context — `tcsetattr`, `fcntl`, `write`, `raise` are all POSIX async-signal-safe — and then re-raise, so the process still reports an honest killed-by-signal wait status (130/143). `SA_RESETHAND` restores the default disposition before the handler runs, which both enables the re-raise and guarantees a second Ctrl-C hard-kills if anything wedges. Raw-mode setup also saves the stdin fcntl flags, and `ui_term_restore_mode` restores them: `O_NONBLOCK` lives on the open file description shared with the parent shell, so without that restore every live session (even a clean `q` quit) handed the shell back a nonblocking stdin. `atexit(restore_terminal)` is registered *before* raw mode engages so the `exit(1)` paths inside raw-mode setup are covered too. Handlers are installed from `audio_play()` only — `--render` keeps default signal behavior. Windows needs none of this: Ctrl-C arrives as keystroke `0x03` (raw mode clears `ENABLE_PROCESSED_INPUT`) and quits through the `q` path. Regression coverage: two PTY sub-checks in `tests/test_smoke_live.sh` (SIGTERM restore + clean-`q` `O_NONBLOCK`). Known deferral: SIGTSTP (Ctrl-Z) suspends with the terminal still raw until `fg`.

## Testing

| Target | Scope |
|---|---|
| `make test` | CLI contract (`tests/test_cli.sh`: help/version/usage errors, stdout render, preset flags, no-server UX, offline install.sh, man page) + bit-exact regression (render 16 s at `--seed 0` twice, byte-compare, sha256 against `golden/regression_16s.sha256`) + the Constitution↔Makefile bridge and amend regression suites. |
| `make test-unit` | 173 unit tests across the `tests/unit/test_*.c` files (arena, effects, voice, gen, lsystem, midi, chord_progression, section, density, motif, mixer, wav, keys) using the hand-rolled framework in `tests/unit/test.h`. |
| `make test-multiseed` | Renders 4 s at seeds 0 / 1 / 42 / 12345, asserts each is deterministic across runs, asserts all four produce distinct sha256s, asserts each render's peak / clip count / spectral centroid / zero-crossing rate land within sane bounds (RMS is reported but not gated, since the randomized INTRO palette varies loudness), then matches each hash against `golden/regression_multiseed.sha256.txt`. |
| `make test-smoke` | Spawns `./synth --no-ui` under a 2 s timeout. Pass on exit 0 / 124 / 143; fail on segfault. Auto-skips if no PulseAudio. |
| `make coverage` | Rebuilds instrumented (`-fprofile-arcs -ftest-coverage`), runs the regression + unit suites, prints per-file line coverage via `gcov`. |

The framework header `tests/unit/test.h` (149 LOC) provides `TEST(name) {...}` registration via constructor attributes plus assertion macros (`ASSERT_TRUE` / `ASSERT_EQ` / `ASSERT_NE` / `ASSERT_NEAR` / `ASSERT_BETWEEN`). Each `tests/unit/test_*.c` links against `arena.o + voice.o + gen.o` (no `main.o`) and runs as a standalone binary.

Approximate line coverage:

| File | Coverage | CI gate |
|---|---|---|
| `arena.c` | 100% (OOM exit covered via fork) | ≥95% |
| `effects.c` | 100% (test_effects + test_keys) | ≥95% |
| `voice.c` | 98% | ≥95% |
| `gen.c` | 99% | ≥90% |
| `lsystem.c` | 94% | ≥90% |
| `chord_progression.c` | 93% | ≥90% |
| `section.c` | 100% | ≥95% |
| `density.c` | 100% | ≥95% |
| `motif.c` | 100% | ≥95% |
| `mixer.c` | 100% | ≥95% |
| `wav.c` | 95% | ≥90% |
| `main.c` | — | excluded (process-level argv branches; see Makefile `COV_SRCS_INTERACTIVE`) |
| `audio_midi.c` | 97.48% (gcov WSL Ubuntu per PR #112; CC dispatch + channel filter + ring buffer + opt-out; 23 unit tests in `tests/unit/test_midi.c` cover US1/US2/US3 + T034/T036 enumeration + wildcard-sentinel contracts) | ≥90% |
| `ui.c`, `keys.c`, `audio_pulse.c`, `audio_midi_linux.c` | — | excluded (interactive; require TTY + audio device or snd-seq-dummy loopback to enumerate — listed in `Makefile` `COV_SRCS_INTERACTIVE`) |
| `audio_midi_winmm.c` | — | platform-gated (Windows cross-compile only via `x86_64-w64-mingw32-gcc`; the Linux CI runner does not produce `audio_midi_winmm.o`, so it is implicitly excluded from `COV_SRCS_MEASURED` without needing an interactive-source listing) |

Total: 153 unit tests across 13 modules (the 23 new MIDI tests in `tests/unit/test_midi.c` per FR-051 / SC-005: US1 scale-degree + octave clamp + velocity + voice-stealing + no-midi byte-identity, US2 CC dispatcher + multi-CC additive composition + reserved-CC no-op slots, US3 channel filter + enum nulate contract + wildcard sentinel).

The coverage build (`make coverage`) writes every artifact (instrumented `.o`, `.gcno`, `.gcda`, `synth_cov`, `.cov` test binaries) into `build_cov/` so it does not clobber the normal build. `make coverage` and `make test-unit` can be alternated freely without `make clean`. CI's "Coverage gates" step parses the per-file numbers and fails if any drop below the gate.

CI also enforces a Windows packed binary size budget of 48 KB (current ~38 KB post-#117 per the `binary-sizes` CI artifact — Constitution Principle I target is the ≤48 KB UPX-packed Windows invariant). A PR that doubles the binary fails CI immediately.

CI (`.github/workflows/ci.yml`) runs every target on push and pull-request to `main`, plus the Windows cross-compile (`make winpack`). Per-file coverage gates apply to the twelve measured modules at 90–95% (see the coverage table above). The Windows binary and coverage log are uploaded as build artifacts. The runner image is pinned (`ubuntu-24.04`, not `-latest`) and kept identical to the release workflow's: the build-time table generators use host libm (`sinf`/`exp`), so a glibc bump can flip a table entry and change every render hash — releases must run on the exact image CI gates the goldens on.

**Releases** (`.github/workflows/release.yml`): pushing an annotated `v*` tag builds on the same pinned image, runs the full gate set (`make test`, `test-unit`, `test-multiseed`, plus `tools/size-budget-gate.sh` — the same 3-key budget gate ci.yml runs, extracted to a script so published binaries are gated too), asserts `./synth --version` equals `stretto <tag>` and that the tree stayed clean through all builds, then assembles `stretto-<tag>-{linux-x86_64,linux-x86_64-upx,windows-x86_64.exe,windows-x86_64-upx.exe}` + `stretto.1` + `sha256sums.txt` into a dist directory, runs the **installer drift gate** (executes the repo's `install.sh` against that dist via `STRETTO_BASE_URL=file://…` and asserts the installed binary's `--version` — so an asset rename fails the release, not the user), and publishes via an idempotent `gh release` sequence (draft → upload → publish, re-run-safe). `workflow_dispatch` rehearses the whole pipeline — including the drift gate — without publishing.

### Size budget amendment workflow (Constitution ↔ Makefile bridge)

The post-#117 cascade (PRs #121–#130) introduced a Constitution↔Makefile bridge: the 3 size budgets in Constitution Principle I (Windows UPX ≤48 KB, Linux UPX ≤30 KB, Linux stripped ≤50 KB) MUST stay in lockstep with the 3 Makefile variables (`WIN_PACK_BUDGET = 49152`, `PACK_TARGET = 30720`, `STRIP_TARGET = 51200`). Forgetting to bump one of them in a v1.X.0 amendment is what caused the original drift cascade. These two files are the ONLY copies: the ci.yml `Binary size budget gate` carries no inline budget constants — `make size` echoes the three Makefile variables into `binary-sizes.txt` as `budget_*` key=value rows, and the gate reads measurements and budgets from that one artifact. A v1.X.0 amendment therefore only ever touches the Constitution + Makefile pair. The bridge is enforced by these artifacts:

| File | Role |
|---|---|
| `tools/spec-budget-check.sh` | Read-only bridge. Asserts `Makefile byte value = Constitution KB value × 1024` for all 3 budgets. Extracts via targeted `grep -oE` regex on Principle I paragraph (the `≤X KB` literal before each keyword, not the parenthetical measurement mentions). Exits 0 on match, 1 on drift, 2 on malformed inputs. |
| `tools/spec-budget-amend.sh` | Write helper. Bumps 1-3 budgets in BOTH `.specify/memory/constitution.md` AND `Makefile` in lockstep via targeted `sed -E` regex (preserves surrounding text + cross-references). Prints a `git diff` for review. **Refuses to commit** (no `git add` or `git commit`); developer commits manually with a v1.X.0 rationale. Rejects shrink attempts (budgets can only grow per post-#117 policy). KB→bytes conversion done automatically (`KB × 1024`). |
| `tests/test_spec_budget_check.sh` | 5-scenario regression: (1) happy-path, (2) tamper Constitution, (3) tamper Makefile, (4) malformed constant, (5) recovery via `git checkout`. |
| `tests/test_spec_budget_amend.sh` | 6-scenario regression: (1) happy-path amend 1 budget, (2) atomic amend all 3, (3) input validation (0 flags / non-integer / <1 KB / shrink), (4) dry-run, (5) refuse-to-commit, (6) recovery via `git checkout`. Has a scoped dirty-tree guard (only checks Constitution + Makefile, not whole tree) to avoid the `chmod +x` mode-change trap. |
| `tools/verify-bridge.sh` (called via `make verify`) | One-command local dev wrapper: runs the 3 verification artifacts above in sequence (bridge check → bridge regression → amend regression), exits on first failure with a clear per-step status + recovery hint. Equivalent to running the 3 dedicated ci.yml steps (Bridge regression test + Amend helper regression test + the inline Binary size budget gate's pre-flight) on a dev box. **Run this before opening a PR** to catch spec↔build drift + amend helper regressions locally instead of waiting for CI. |

The amend helper's flags:

- `--win KB` — set Windows UPX-packed budget to KB
- `--lin-upx KB` — set Linux UPX-packed budget to KB
- `--lin-str KB` — set Linux stripped budget to KB
- `--dry-run` — preview the changes without modifying any files
- `--help` / `-h` — show usage

At least one `--{win,lin-upx,lin-str}` flag is required. Multiple flags can be combined for an atomic 3-budget amend. The script's exit codes: 0 = amend complete (changes left in working tree, NOT committed), 1 = invalid input or amend failed (working tree restored to pre-amend state), 2 = FATAL setup failure (script bug / missing files).

The amend script leaves the Makefile rationale paragraph (comment block above the 3 budget variables) AND the Constitution footer (Last Amended date + Version line) unchanged — those are editorial content, manually curated in the same amend-PR. See PR #127 / 032-spec-budget-amend for the helper's design rationale + PR #130 / 037-amend-test-dirty-guard-scope for the scoped dirty-tree guard fix.

### CI step layout (post-#128)

`.github/workflows/ci.yml` defines 18 explicit steps. **Note on step numbering:** GitHub Actions auto-prepends `Set up job` to every job, so UI step numbers are **1-indexed from the auto-added step**. The explicit `actions/checkout@v4` step (no `name:`) renders as `Run actions/checkout@v4` / step #2. The YAML order is 0-indexed from `actions/checkout@v4` and internal to the file. Use UI numbers in PR bodies / commit messages (the header comment at the top of `ci.yml` documents this convention so future PRs don't re-discover the off-by-one).

The 18 explicit steps render as UI rows 2–19 (row 1 is the auto-prepended `Set up job`):

| # | Step | Purpose |
|---:|---|---|
| 1 | Set up job | *(auto-prepended by GitHub Actions)* |
| 2 | Run actions/checkout@v4 | *(no `name:`; auto-renders)* |
| 3 | Install build deps | apt: gcc, make, libpulse-dev, libasound2-dev, upx-ucl, gcc-mingw-w64-x86-64, python3, python3-numpy |
| 4 | Build Linux synth | `make` |
| 5 | Bit-exact regression test | `make test` (bitexact + bridge + amend + unit) |
| 6 | Bridge regression test (Constitution↔Makefile) | `bash tests/test_spec_budget_check.sh` — 5 scenarios / 9 sub-checks. Pre-flight for the Binary size budget gate (step 15). |
| 7 | Amend helper regression test (Constitution↔Makefile) | `bash tests/test_spec_budget_amend.sh` — 6 scenarios / 21 sub-checks |
| 8 | Unit tests | `make test-unit` (153 tests across 13 modules) |
| 9 | Multi-seed integration test | `make test-multiseed` |
| 10 | Live-mode smoke test (skips if no PA) | `make test-smoke` |
| 11 | Cross-compile Windows .exe | `make win` |
| 12 | UPX-pack Windows .exe | `make winpack` |
| 13 | Binary sizes report | `make size | tee binary-sizes.txt` (measurements + `budget_*` rows) |
| 14 | Upload binary sizes artifact | uploads `binary-sizes.txt` as the `binary-sizes` artifact |
| 15 | **Binary size budget gate** | 3-key gate against `binary-sizes.txt`; budgets come from the artifact's `budget_*` rows (echoed by `make size` from the Makefile — no inline constants in ci.yml). Replaces the pre-#125 single-key gate. |
| 16 | Coverage report | `make coverage | tee coverage.log` |
| 17 | Coverage gates | Per-file coverage thresholds (90-95%) |
| 18 | Upload Windows binary artifact | `stretto-windows` artifact |
| 19 | Upload coverage log | `coverage-log` artifact |

(The pre-041 versions of this table omitted the `Upload binary sizes artifact` row and numbered the gate #14; the corrected UI number is #15.) The pre-#125 cascade also had a redundant `Assert Spec↔Build size budgets` step that duplicated the bridge. PR #125 removed it; the Bridge regression test (step 6) + Binary size budget gate (step 15) are the only 2 spec↔build enforcement points now, with clear pre-flight / measurement roles.

## Build details

Linux flags (`-Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections`) and `strip -s -R .comment` are standard. `make pack` runs UPX `--ultra-brute` on top for a final ~42% reduction (historical example, per the PR #117 `binary-sizes` artifact: synth 43 944 B → synth.packed 25 460 B = 57.94 % retained = 42.06 % reduction; the prior ~33 % pre-#109 hedge reflected the smaller pre-003-chain baseline where 24 576 × 0.67 ≈ 16 384).

Windows cross-compile uses `x86_64-w64-mingw32-gcc` with the same size flags. `make winpack` adds UPX. The packed Windows binary is ~38 KB — well under the 64 KB target from the original PLAN.md and the demoscene "tiny generative synth" tradition.
