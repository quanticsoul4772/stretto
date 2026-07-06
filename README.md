# stretto

A generative music synthesizer. Plays live on Linux (PulseAudio) or Windows (waveOut), or renders to a 48 kHz stereo WAV file. C99, no malloc, single static arena.

| Binary | Size |
|---|---|
| Linux `synth` (stripped, links libpulse) | ~39 KB |
| Windows `stretto.exe` (stripped + UPX) | ~37 KB |

## Build

### Linux

```
make
```

Produces `./synth`. Needs `gcc`, `make`, `libpulse-dev`.

### Windows (cross-compile from Linux / WSL)

```
make win        # produces stretto.exe (~215 KB, stripped)
make winpack    # additionally produces stretto.packed.exe (~37 KB, UPX-packed)
```

Needs `gcc-mingw-w64-x86-64` and `upx`. The packed `.exe` is a single-file native Windows binary — no WSL, no runtime dependencies beyond the bundled Windows kernel + multimedia DLLs.

### Optional: UPX-pack the Linux build

```
make pack
```

Produces `synth.packed` (~14 KB).

## Run

### Live mode

```
./synth                  # Linux
.\stretto.exe            # Windows (in PowerShell or CMD)
```

Plays generative audio out the default audio device, draws an ASCII oscilloscope sized to the terminal. Different generative output every launch (PRNG seeded from the system clock).

### Render mode

```
./synth --render <seconds> <output.wav>
.\stretto.exe --render <seconds> <output.wav>
```

Writes a stereo 16-bit 48 kHz WAV. Seconds in `1..3600`. No audio device opened.

### Reproducible runs

```
./synth --seed <N>
./synth --render 60 song.wav --seed 12345
```

Fixes the PRNG / cellular automaton / Markov seeds to `N`. Same `--seed` always produces the same audio (this is how the regression test works).

## Keyboard controls (live mode)

| Key | Action |
|---|---|
| `SPACE` | Force a mutation (re-roll one Markov weight, flip one CA bit, change one Euclidean k) |
| `+` / `-` | Tempo up / down 10% |
| `[` / `]` | FM `mod_depth` down / up by 200 |
| `s` | Cycle scale (Dorian → Lydian → Phrygian → Locrian → Harmonic Minor → Mixolydian) |
| `g` / `G` | Gate probability down / up by 16 |
| `r` / `R` | Reverb wet mix down / up by 16 |
| `d` / `D` | Delay wet mix down / up by 16 |
| `f` / `F` | Delay feedback down / up by 16 |
| `c` / `C` | Filter cutoff down / up by 10 |
| `n` / `N` | Filter resonance down / up by 10 |
| `m` / `M` | Filter LFO depth down / up by 8 |
| `t` | Cycle filter mode (LP → HP → BP → notch) |
| `l` / `L` | Compressor threshold down / up by 1000 (lowercase L / shift-L; not the digit `1` or capital `I`). Lower threshold = more compression. |
| `?` | Toggle help overlay |
| `q` | Quit (restores terminal state) |
| `Ctrl-C` | Same as `q` via atexit handler |

## Status row

Colored single-line status at the top of the terminal:

```
M:1500 S:D V:*.***...*** G:200 R:60 D:100/140 deg:3 act:#.##.#. chord:sus2 Cr:4 Sec:body Td:118 Mo:c F:200 N:100 L:80 T:LP Lm:20000
```

| Field | Meaning |
|---|---|
| `M` | FM `mod_depth` (100–8000) |
| `S` | Scale: `D` Dorian, `L` Lydian, `P` Phrygian, `l` Locrian, `H` Harmonic Minor, `M` Mixolydian |
| `V` | 11 activity dots — `*` = firing, `.` = silent. Colored: red bass (slots 0–1), green chord (2–4), blue melody (5–7), yellow drums (8–10) |
| `G` | Gate probability (0–255) |
| `R` | Reverb wet mix (0–256) |
| `D` | Delay `wet/feedback` (0–256 / 0–200) |
| `deg` | Current Markov walk position (0–6) — counter-melody. Main melody uses the L-system walker; its pointer is internal |
| `act` | Active scale-degree mask (7 chars, `#` = active, `.` = suppressed by CA) |
| `chord` | Current bar's voicing: `triad`, `7th`, `sus4`, `sus2`, `inv1`, `inv2` |
| `Cr` | Current chord root (0–6, advances every 2 bars via the chord-progression Markov chain) |
| `Sec` | Current song section (`intro` / `body` / `tens` / `res`, cycles every 96 bars) |
| `Td` | Adaptive density tension (0–255; biases gate and reverb counter-cyclically — busy textures pull back, sparse fill in) |
| `F` | SVF filter cutoff base (30–180 user-tunable; per-role offsets + LFO + chord fenv modulate this per voice) |
| `N` | SVF resonance base (0–180; per-role offsets apply per voice) |
| `L` | Filter LFO depth (0–255; scales how much the per-voice pan LFO sweeps the cutoff) |
| `T` | Filter mode: `LP` low-pass, `HP` high-pass, `BP` band-pass, `NO` notch |
| `Mo` | Motif memory state: `c` capture (L-system driving + recording), `r` replay (8-phrase ring buffer playing a captured 4-bar phrase, optionally transposed) |
| `Lm` | Master compressor threshold (8000–30000; brickwall at 32000) |

The oscilloscope below paints with a heat-map palette: dim (silence) → blue → cyan → green → yellow → magenta → red (peak).

## What you'll hear

- **Bass** (super-saw subtractive, 2 voices) — 3 detuned saw oscillators (≈±0.78 %) summed and run through the SVF, for a thick, wide bass. 4-event "bouncing" pattern per bar: substeps 0, 18, 24, 42. Plays the current chord's root and fifth, alternating, swapping order per bar. At every chord change, plays a one-step diatonic approach note before resolving — walking-bass feel. Legato re-triggers **glide** (~50 ms portamento) into the new pitch.
- **Chord** (section-selected synthesis, 3 voices) — 4 voicings per bar at substeps 0, 12, 24, 36 (block) or 8 arpeggiated notes per bar in TENSION. The synthesis method changes per section: **wavetable** (INTRO/RESOLVE, animated morphing pad), **additive** (BODY, drawbar-organ pad), **FM** (TENSION, glassy/cutting). Voicing cycles triad / 7th / sus4 / sus2 / inv1 / inv2 per bar; root walks the chord-progression Markov chain (every 2 bars); voice leading octave-shifts each pitch toward the previous chord's centroid.
- **Melody** (Karplus-Strong + FM alternating, 3 voices) — Euclidean rhythm on a 16-step grid, **L-system walker** producing phrased contours over scale degrees, probability-gated. A motif memory captures the last 8 four-bar phrases; every ~30 bars one is replayed (verbatim or ±2 diatonic transpose) instead of generating new. A counter-melody one octave up runs an independent **2nd-order Markov** chain (3-degree context) biased away from unison with the main melody and toward 3rd/6th consonances.
- **Drums** (3 voices: kick, snare, hihat) — kick is a sine pitch-sweep with attack click; snare is noise-dominant with a 200 Hz body; hihat is pure noise. Each drum cycles its own pattern bank — kick 4 / snare 3 / hihat 5 patterns → LCM of 60 bars before exact repeat. The song-section state machine pins the kick pattern per section.
- **Section voice palette** — which families are audible changes per section. BODY and TENSION play the full ensemble; **RESOLVE is drumless** (ambient close); **INTRO opens sparse** — a randomized 1–3-voice subset chosen once per 96-bar cycle from 8 curated combos, so each intro is a different minimal palette before the full mix arrives.
- **Master bus** — reverb (4-comb Schroeder + 2 all-pass per channel) → stereo delay (250 ms, runtime wet/feedback) → soft cubic saturation → **feed-forward compressor with brickwall limiter** (4:1 above threshold, ~5 ms attack / ~200 ms release, stereo-linked envelope, ceiling at 32 000). Compressor tames transients (kick on TENSION sections) and guarantees no int16 clip.

The whole texture sits in a Schroeder reverb tail and a 250 ms stereo delay, with soft cubic saturation at the master bus.

## Architecture summary

11 voice slots share a single sample clock at 48 kHz. The audio is mixed to stereo, run through reverb → delay → soft saturation → compressor + brickwall limiter, and either fed to `pa_stream_write` (Linux) / `waveOutWrite` (Windows) or written to a WAV file. Each voice slot is one of six synthesis types (Karplus-Strong, 2-op FM, wavetable, additive, super-saw subtractive, or a drum generator); the per-section voice mask decides which families actually sound.

A 48-substep bar (LCM of 3, 4, 16) supports 3-against-4 polyrhythm between bass and chord while keeping the melody on a 16-step Euclidean grid.

Mutation runs at a dynamic rate driven by a slow triangle LFO that sweeps the mutation interval between 1 bar (busy section) and 16 bars (calm) over a ~4-minute cycle, so the piece naturally alternates between dense and sparse passages.

See `ARCHITECTURE.md` for the detailed walkthrough.

## Specification

The synth's user-facing spec, architecture plan, decision history, and task inventory live under `specs/001-stretto-baseline/`:

- `spec.md` — what the synth does today (three prioritized user stories, functional requirements FR-001..FR-063, measurable success criteria SC-001..SC-007, edge cases)
- `plan.md` — implementation plan + Constitution Check (all ten principles PASS) + complexity tracking + project structure
- `research.md` — Phase 0 research synthesis: twelve architectural decisions with rationale and alternatives considered
- `tasks.md` — seventy tasks organized by user story across six phases (Setup / Foundational / US1 / US2 / US3 / Polish)
- `quickstart.md` — minimal CLI usage reference (build, render, live mode, key map, platform notes)

Architectural principles are encoded in `.specify/memory/constitution.md` (NON-NEGOTIABLE: I Tiny Native Binary, III Deterministic, VI Test Discipline). Each spec-kit artifact declares compliance or documents an exception per the Governance clause. The most recent constitution amendment is Principle III → v1.0.1 (2026-07-06), documented in `CHANGELOG.md → Recent: spec-kit bootstrap + Constitution III amendment`, which closes the platform-scope wording gap exposed by `/speckit-analyze` finding D1.

## Tests

```
make test            # bit-exact regression (renders 16 s at seed 0, sha256 check)
make test-unit       # 130 unit tests across all pure-synth modules + keys
make test-multiseed  # renders 4 seeds, checks determinism + audio bounds + golden
make test-smoke      # spawns ./synth for 2 s, expects clean exit / SIGTERM
make coverage        # rebuilds with -fprofile-arcs -ftest-coverage and prints
                     # per-file line coverage via gcov (output to build_cov/)
make debug           # builds synth_debug: -O0 -g -DDEBUG, no LTO, gdb-friendly
                     # (co-exists with synth; separate .dbg.o filenames)
```

After an intentional synth change:

```
make golden            # regenerate the 16 s seed-0 reference hash
make golden-multiseed  # regenerate the four multi-seed reference hashes
```

CI (GitHub Actions) runs all of the above on every push and pull request to `main`, plus the Windows cross-compile (`make winpack`). The Windows binary is uploaded as a build artifact.

Approximate line coverage (unit + integration combined; CI enforces these as per-file gates):

| File | Coverage | Gate |
|---|---|---|
| `arena.c` | 100% | ≥95% |
| `effects.c` | 100% | ≥95% |
| `voice.c` | 98% | ≥95% |
| `gen.c` | 99% | ≥90% |
| `lsystem.c` | 94% | ≥90% |
| `chord_progression.c` | 93% | ≥90% |
| `section.c` | 100% | ≥95% |
| `density.c` | 100% | ≥95% |
| `motif.c` | 100% | ≥95% |
| `mixer.c` | 100% | ≥95% |
| `wav.c` | 95% | ≥90% |
| `main.c` | 97% | ≥90% |
| `ui.c`, `keys.c`, `audio_pulse.c` | — | excluded (require TTY + audio device) |

Coverage build writes all artifacts under `build_cov/` so `make test-unit` and `make coverage` can be alternated without `make clean`. Windows binary size budget (48 KB packed) is also gated in CI.

## Files

| File | Purpose |
|---|---|
| `main.c` | argv parsing + dispatch to render-mode or live-audio |
| `config.h` | Project-wide audio constants (`SAMPLE_RATE`, `BUFFER_FRAMES`) |
| `mixer.c` / `.h` | `render_chunk()` — voice mix → reverb → delay → soft saturation |
| `voice.c` / `.h` | Voice struct (KS / FM / wavetable / additive / super-saw / drum), ADSR, SVF, super-saw glide, role-based pool, peak normalization |
| `effects.c` / `.h` | Master-bus delay, Schroeder reverb, soft saturation, shared `sat16` |
| `gen.c` / `.h` | Sample clock, scales, CAs, counter-melody Markov, Euclidean rhythm, drum patterns, mutation, scheduler dispatcher (`schedule_bar_boundary`, `schedule_drums`, `schedule_bass`, `schedule_chord`, `schedule_melody`) |
| `lsystem.c` / `.h` | L-system melodic phrase generator (main melody) |
| `chord_progression.c` / `.h` | Markov chain over chord functions (root advances every 2 bars) |
| `section.c` / `.h` | Song-section state machine (intro / body / tension / resolve) over a 96-bar cycle: biases parameters and pins kick pattern, L-system character, chord voice type, chord arpeggio mode, and the per-section voice-family mask |
| `density.c` / `.h` | Adaptive density: counter-cyclical gate + reverb biases derived from active-mask popcount + gate |
| `motif.c` / `.h` | Long-term motif memory: 8-phrase ring buffer; every ~30 bars replays one captured 4-bar phrase (verbatim or ±2 transposed) instead of L-system output |
| `wav.c` / `.h` | `render_wav()` + WAV header |
| `ui.c` / `.h` | Terminal raw mode, oscilloscope, status row builder, help overlay (cross-platform) |
| `keys.c` / `.h` | Key dispatcher (`'?'`, `'q'`, tempo, scale, filter, etc.) |
| `audio_pulse.c` | Linux live-audio backend (PulseAudio `pa_threaded_mainloop` + `pa_stream`) |
| `audio_winmm.c` | Windows live-audio backend (Win32 `waveOut`, 4-buffer cycle) |
| `audio.h` | One-function API (`audio_play()`); selects backend at link time |
| `arena.c` / `.h` | Static 128 KB pool with bump allocator |
| `gen_*_table.c` | Build-time generators for sine / envelope / MIDI note / Bjorklund / wavetable tables |
| `Makefile` | `make`, `make win`, `make winpack`, `make pack`, `make test`, `make test-unit`, `make test-multiseed`, `make test-smoke`, `make coverage`, `make debug`, `make golden`, `make golden-multiseed` |
| `tests/test_bitexact.sh` | Renders twice with `--seed 0`, sha256-compares, validates against golden |
| `tests/test_multi_seed.sh` | Renders 4 seeds; determinism + audio bounds + golden hashes |
| `tests/test_smoke_live.sh` | Spawns `./synth --no-ui` for 2 s; expects clean exit / SIGTERM |
| `tests/unit/test_*.c` | 130 unit tests across arena, effects, voice, gen, lsystem, chord_progression, section, density, motif, mixer, wav, keys |
| `golden/regression_16s.sha256` | Reference hash for the 16-second seed-0 render |
| `golden/regression_multiseed.sha256.txt` | Reference hashes for the four multi-seed renders |
| `.github/workflows/ci.yml` | CI: build, all tests, Windows cross-compile, coverage gates, size gate |
| `PLAN.md` | Original design document (historical) |

## Environment notes

- **Native Linux**: live audio via libpulse direct (`pa_stream` API on a threaded mainloop).
- **WSL2 + WSLg**: WSLg's audio pipe is unreliable for sustained playback (multiple GitHub issues open against `microsoft/wslg`). Run `stretto.exe` directly on Windows instead — it bypasses WSL entirely.
- **Windows**: live audio via Win32 `waveOut` (mmsystem). No external dependencies beyond `kernel32` and `winmm`.

## License

MIT. See `LICENSE`.
