# stretto

A generative music synthesizer. Plays live on Linux (PulseAudio/PipeWire) or Windows (waveOut), or renders to a 48 kHz stereo WAV file. C99, no malloc — all runtime state lives in one static 128 KB arena. Output is deterministic: the same `--seed` and flags always produce byte-identical audio.

| Binary | Size | Budget (CI-gated) |
|---|---|---|
| Linux `synth` (stripped; links libpulse + libasound) | ~47 KB | 50 KB |
| Linux `synth.packed` (UPX) | ~25 KB | 30 KB |
| Windows `stretto.exe` (stripped + UPX) | ~38 KB | 48 KB |

## Install

Linux x86_64, no root needed — downloads the latest release binary + man page and verifies both against the release's `sha256sums.txt`:

```
curl --proto '=https' --tlsv1.2 -fsSL https://raw.githubusercontent.com/quanticsoul4772/stretto/main/install.sh | sh
```

Prefer to read it first (same script, same source):

```
curl -fsSLO https://raw.githubusercontent.com/quanticsoul4772/stretto/main/install.sh
less install.sh && sh install.sh
```

Installs to `~/.local/bin` (or `/usr/local` when run as root); `STRETTO_VERSION=vX.Y.Z` picks a specific release. Or use [ubi](https://github.com/houseabsolute/ubi), which selects the unpacked binary automatically:

```
ubi --project quanticsoul4772/stretto --in ~/.local/bin
```

Prebuilt binaries with `sha256sums.txt` are attached to [tagged releases](https://github.com/quanticsoul4772/stretto/releases) — Windows users should take the unpacked `.exe` (the `-upx` variants trip antivirus false positives).

### Hear it now

```
stretto                                   # live synth (q quits, ? = key map)
stretto --render 10 demo.wav --seed 42    # render 10 s; no audio server needed
man stretto
```

If `~/.local/bin` wasn't on your PATH before, restart your shell — Debian/Ubuntu's `~/.profile` adds it only when it exists at login, and `man stretto` resolves through PATH too.

### Homebrew (Linuxbrew)

```
brew tap quanticsoul4772/stretto https://github.com/quanticsoul4772/stretto
brew install stretto
```

The repo doubles as its own tap (`Formula/stretto.rb`). **Linux(brew)-only**: no macOS audio backend exists. An AUR package definition lives at `packaging/aur/PKGBUILD` (unpublished).

## Build

### Linux

```
make
```

Produces `./synth`. Needs `gcc`, `make`, `libpulse-dev`, `libasound2-dev`.

### Windows (cross-compile from Linux / WSL)

```
make win        # produces stretto.exe (~247 KB, stripped)
make winpack    # additionally produces stretto.packed.exe (~38 KB, UPX-packed)
```

Needs `gcc-mingw-w64-x86-64` and `upx`. The packed `.exe` is a single-file native Windows binary — no runtime dependencies beyond the bundled Windows kernel + multimedia DLLs.

### Optional: UPX-pack the Linux build

```
make pack
```

Produces `synth.packed` (~25 KB, within the 30 KB cap).

### Install from source

```
make && sudo make install     # installs /usr/local/bin/stretto + man page
sudo make uninstall
```

Installs the binary under its canonical name `stretto` plus `man 1 stretto`. `PREFIX` and `DESTDIR` are honored (`make install DESTDIR=/tmp/pkg PREFIX=/usr` for packagers). Build **before** installing, as your user — `make install` deliberately does not build (a `sudo`-driven rebuild would embed `dev` as the version, since git refuses to run in another user's checkout).

## Run

### Live mode

```
./synth                  # Linux
.\stretto.exe            # Windows (in PowerShell or CMD)
```

Plays generative audio out the default audio device, draws an ASCII oscilloscope sized to the terminal. Different generative output every launch (PRNG seeded from the system clock).

### Render mode

```
./synth --render <seconds> <out.wav|->
.\stretto.exe --render <seconds> <out.wav|->
```

Writes a stereo 16-bit 48 kHz WAV. Seconds in `1..3600`. No audio device opened.

`-` streams the WAV to stdout for pipelines (sox/ffmpeg convention; byte-identical to the file output):

```
./synth --render 60 - --seed 7 | ffmpeg -i - out.flac
./synth --render 10 - | aplay
```

A file literally named `-` needs `./-`. A downstream that closes early ends the render via SIGPIPE, standard pipeline behavior. Windows note: PowerShell older than 7.4 re-encodes binary pipes and redirection — use cmd.exe redirection or an output path there.

### Help and version

```
./synth --help       # usage + flag reference (stdout, exit 0)
./synth --version    # "stretto <version>" + license block (stdout, exit 0)
```

Both follow GNU conventions: stdout, exit 0, and they take precedence over every other argument. The version string is embedded at build time from `git describe` (`dev` when building outside a git checkout).

### Reproducible runs

```
./synth --seed <N>
./synth --render 60 song.wav --seed 12345
```

Fixes the PRNG / cellular automaton / Markov seeds to `N`. Same `--seed` always produces the same audio (this is how the regression test works).

### Preset capture

Every user-tunable parameter can be set at launch, and every session prints a pasteable recall line on quit:

```
./synth --scale lydian --bar-ms 1500 --reverb 120        # start with these values
./synth --render 60 out.wav --seed 7 --filter-mode bp    # flags work for renders too
...
q                                                        # (or Ctrl-C / SIGTERM)
resume with: --seed 3735928559 --scale lydian --bar-ms 1500 --reverb 120
```

| Flag | Range | Sets |
|---|---|---|
| `--scale <name\|0-5>` | dorian, lydian, phrygian, locrian, harmminor, mixolydian | scale |
| `--bar-ms <760-7600>` | ms per bar (default 2000) | tempo |
| `--gate <32-255>` | | melody gate probability |
| `--mod-depth <100-8000>` | | FM modulation depth |
| `--cutoff <30-180>` | | filter cutoff base |
| `--resonance <0-180>` | | filter resonance base |
| `--lfo-depth <0-255>` | | filter LFO depth |
| `--filter-mode <m\|0-3>` | lp, hp, bp, notch | filter mode |
| `--reverb <0-256>` | | reverb wet mix |
| `--delay <0-256>` / `--feedback <0-200>` | | delay wet / feedback |
| `--comp-threshold <8000-30000>` | | compressor threshold |

Ranges mirror the live-key clamps; out-of-range values are hard errors, never silent clamps. Output is a pure function of (`--seed`, flags) — a flagged render is byte-reproducible, and flags at their defaults are byte-inert.

The resume line contains the seed (also captured for clock-seeded runs) plus **only the parameters you explicitly set** — via flag or live key. The synth's internal mutation drift is never printed: `--seed` alone reproduces that drift from bar 0. Recall reproduces a run with those values **from bar 0**, not the keystroke timing of your session — a mid-session scale change alters the harmony from that point, so a recalled run matches your session only up to where you changed scale. Two quirks: the untouched cutoff default (200) sits above the `--cutoff` dial range (30–180) — omit the flag to keep it; MIDI CC tweaks are not captured (a `--midi` session isn't reproducible anyway, since your playing perturbs the voice pool).

## MIDI

Live MIDI keyboard input — plug a controller in, the synth plays your notes mixed over the generative output. ALSA sequencer on Linux, WinMM on Windows; identical interface.

| Flag | Meaning |
|---|---|
| (none) / `--no-midi` | MIDI off (the default). |
| `--midi` | Subscribe to every available input port (Linux); open the first device (Windows — WinMM binds one handle to one device). |
| `--midi <N>` | Open the device with index `N` from `--midi-list-devices`. |
| `--midi-default` | Alias for `--midi 0` — the first listed device. |
| `--midi-list-devices` | Print `<index> <name>` per input device (capped at 32) and exit. |
| `--midi-channel <1..16>` | Accept only this channel. Requires `--midi` or `--midi-default`. |

Startup failure is loud: if a requested device can't be opened, the synth prints `MIDI: ... unavailable (see --midi-list-devices)` and exits 1. In `--render` mode MIDI is never opened, regardless of flags — an open device could inject notes into a seeded render — so a stderr notice is printed and the render stays byte-identical to a no-MIDI run.

### Finding your device

```
./synth --midi-list-devices
3584 Midi Through Port-0
5120 MPK mini 3 MPK mini 3 MIDI 1
```

The first column is the `<index>` you pass to `--midi <N>`. On Linux it encodes the ALSA sequencer address as `(client<<8)|port` (3584 = client 14, port 0 — the address `aconnect -l` shows as `14:0`); on Windows it is the plain 0-based device ordinal. `--midi 0` / `--midi-default` opens the first listed device on both platforms.

### Note mapping

- Note On / Off maps through the active scale, the same way the generative melody does, so MIDI notes feel in-scale instead of chromatic. Velocity 0 on a Note On means Note Off.
- A held key rings at its sustain level until you release it (gate semantics); the generative voices keep their own fire-and-forget envelopes.
- Octave offset clamps to [-2, +4] so a MIDI note never strays out of the synth's audible range.
- 11-voice pool with voice-stealing: idle first, in-release second, oldest third.

### CC mapping

| CC# | Name (MIDI 1.0) | Target | Scale |
|---|---|---|---|
| 1 | Mod Wheel | Filter cutoff | +1 |
| 7 | Channel Volume | Compressor threshold | +60 |
| 64 | Sustain Pedal | Holds Note Offs (≥64 down, <64 up; per channel) | value |
| 71 | Resonance / Timbre | Filter resonance | +1 |
| 74 | Brightness | Filter cutoff | +1 |
| 91 | Reverb Send | Reverb wet | +1 |
| 93 | Chorus / Delay | Delay wet | +1 |
| 123 | All Notes Off | Releases the channel's notes (pedal-held survive until pedal-up) | value-independent |

Delta = `(V - 64) * scale`, summing additively across multiple CCs targeting the same parameter. Two exceptions use the raw value and move no parameters: CC#64 (pedal down/up — holds notes past their Note Off piano-style, all released together on pedal-up) and CC#123 (All Notes Off — a Note Off for every sounding note on the channel; with the pedal down they convert to held, per MIDI 1.0, so a full panic is pedal-up then CC#123). All other CCs (the General Purpose slots) are silently ignored.

### Disconnect

When the OS disconnects the MIDI source mid-session, the synth does **not** auto-reconnect. It continues from internal generative state and CC-modulated parameters retain their last value. The audio path is unaffected.

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
| `Ctrl-C` | Clean shutdown, terminal restored — same for `SIGTERM`, `SIGQUIT` (Ctrl-\), `SIGHUP` |

`Ctrl-Z` suspends cleanly: the terminal is restored before the stop, and `fg` re-enters raw mode. (Resuming with `bg` re-stops it — the standard TUI behavior; use `fg`.)

Redirected invocations (`./synth < /dev/null`, `./synth > log`) degrade to headless `--no-ui` mode instead of dying; a present, non-empty `NO_COLOR` environment variable disables the ANSI colors while keeping the monochrome oscilloscope readable.

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
- **Master bus** — reverb (4-comb Schroeder + 2 all-pass per channel) → stereo delay (250 ms, runtime wet/feedback) → soft cubic saturation → **feed-forward compressor with brickwall limiter** (4:1 above threshold, ~5 ms attack / ~200 ms release, stereo-linked envelope, ceiling at 32 000). Compressor tames transients and guarantees no int16 clip.

## Architecture summary

11 voice slots share a single sample clock at 48 kHz. The audio is mixed to stereo, run through reverb → delay → soft saturation → compressor + brickwall limiter, and either fed to `pa_stream_write` (Linux) / `waveOutWrite` (Windows) or written to a WAV file. Each voice slot is one of six synthesis types (Karplus-Strong, 2-op FM, wavetable, additive, super-saw subtractive, or a drum generator); the per-section voice mask decides which families actually sound.

A 48-substep bar (LCM of 3, 4, 16) supports 3-against-4 polyrhythm between bass and chord while keeping the melody on a 16-step Euclidean grid.

Mutation runs at a dynamic rate driven by a slow triangle LFO that sweeps the mutation interval between 1 bar (busy section) and 16 bars (calm) over a ~4-minute cycle, so the piece naturally alternates between dense and sparse passages.

See `ARCHITECTURE.md` for the detailed walkthrough.

## Tests

```
make test            # CLI contract + bit-exact regression (16 s seed-0 sha256)
                     # + Constitution<->Makefile bridge/amend + size-gate
                     # fixture regression suites
make test-unit       # 182 unit tests across all pure-synth modules + keys + MIDI
make test-multiseed  # renders 4 seeds, checks determinism + audio bounds + golden
make test-smoke      # spawns ./synth for 2 s, expects clean exit / SIGTERM
make coverage        # rebuilds with -fprofile-arcs -ftest-coverage and prints
                     # per-file line coverage via gcov (output to build_cov/)
make test-asan       # ASan + UBSan (fatal) over the unit suite + a render
                     # (separate build_san/ tree; never touches the release binary)
make verify          # Constitution<->Makefile bridge check + its regression suites
make test-crossplatform  # ./synth vs ./stretto.exe byte-compare (dev box with
                         # WSL interop or Windows; auto-skips elsewhere)
make debug           # builds synth_debug: -O0 -g -DDEBUG, no LTO, gdb-friendly
```

After an intentional synth change:

```
make golden            # regenerate the 16 s seed-0 reference hash
make golden-multiseed  # regenerate the four multi-seed reference hashes
```

CI (`.github/workflows/ci.yml`) runs all of the above on every push and PR, cross-compiles the Windows binary, enforces per-file coverage gates (90–95% on the twelve measured modules; `ui.c`, `keys.c`, `audio_pulse.c`, `audio_midi_linux.c`, `main.c` are excluded — they need a TTY / audio device / ALSA sequencer CI doesn't have), and gates all three binary size budgets. The runner image is pinned (`ubuntu-24.04`): build-time table generation uses host libm, so a glibc bump could shift a table entry and change every render hash. Releases (`.github/workflows/release.yml`, on `v*` tags) rerun the full gates, verify `install.sh` against the assembled assets, and publish checksummed binaries; `workflow_dispatch` rehearses the pipeline without publishing.

The three size budgets live in `.specify/memory/constitution.md` Principle I and the `Makefile`, kept in lockstep by `tools/spec-budget-check.sh`; budget amendments go through `tools/spec-budget-amend.sh`. See `ARCHITECTURE.md` for the full bridge documentation.

## Specification

The spec-kit artifacts (spec, plan, research, tasks, quickstart) live under `specs/` — `001-stretto-baseline/` for the synth core, `003-midi-input/` for the MIDI surface, `004-preset-capture/` for the preset flags. Architectural principles are encoded in `.specify/memory/constitution.md` (NON-NEGOTIABLE: I Tiny Native Binary, III Deterministic, VI Test Discipline).

## Files

| File | Purpose |
|---|---|
| `main.c` | argv parsing + dispatch to render-mode or live-audio |
| `config.h` | Project-wide audio constants (`SAMPLE_RATE`, `BUFFER_FRAMES`) |
| `mixer.c` / `.h` | `render_chunk()` — voice mix → reverb → delay → soft saturation |
| `voice.c` / `.h` | Voice struct (KS / FM / wavetable / additive / super-saw / drum), ADSR, SVF, super-saw glide, role-based pool, peak normalization |
| `effects.c` / `.h` | Master-bus delay, Schroeder reverb, soft saturation, shared `sat16` |
| `gen.c` / `.h` | Sample clock, scales, CAs, counter-melody Markov, Euclidean rhythm, drum patterns, mutation, scheduler dispatcher |
| `lsystem.c` / `.h` | L-system melodic phrase generator (main melody) |
| `chord_progression.c` / `.h` | Markov chain over chord functions (root advances every 2 bars) |
| `section.c` / `.h` | Song-section state machine (intro / body / tension / resolve) over a 96-bar cycle |
| `density.c` / `.h` | Adaptive density: counter-cyclical gate + reverb biases |
| `motif.c` / `.h` | Long-term motif memory: 8-phrase ring buffer with periodic replay |
| `wav.c` / `.h` | `render_wav()` + WAV header |
| `ui.c` / `.h` | Terminal raw mode, oscilloscope, status row builder, help overlay (cross-platform) |
| `keys.c` / `.h` | Key dispatcher (`'?'`, `'q'`, tempo, scale, filter, etc.) |
| `audio_pulse.c` | Linux live-audio backend (PulseAudio `pa_threaded_mainloop` + `pa_stream`) |
| `audio_winmm.c` | Windows live-audio backend (Win32 `waveOut`, 4-buffer cycle) |
| `audio.h` | One-function API (`audio_play()`); selects backend at link time |
| `audio_midi.c` / `.h` | Cross-platform MIDI core: SPSC event ring, scale-degree note mapper, CC→parameter dispatch (`CC_MAP`) |
| `audio_midi_linux.c` | ALSA sequencer MIDI input backend (wildcard subscribe + device enumeration, reader thread) |
| `audio_midi_winmm.c` | Windows MIDI input backend (`midiIn*` callback API) |
| `arena.c` / `.h` | Static 128 KB pool with bump allocator |
| `gen_*_table.c` | Build-time generators for sine / envelope / MIDI note / Bjorklund / wavetable tables |
| `version.h` (generated) | `#define STRETTO_VERSION` from `git describe` |
| `Makefile` | `make`, `make win`, `make winpack`, `make pack`, the `test*` targets, `make coverage`, `make debug`, `make golden*`, `make install`, `make verify` |
| `tests/test_cli.sh` | CLI contract: help/version, usage errors, stdout render, preset flags, no-server UX, offline install.sh checks, man-page lint + help↔man drift gate |
| `tests/test_bitexact.sh` | Renders twice with `--seed 0`, sha256-compares, validates against golden |
| `tests/test_multi_seed.sh` | Renders 4 seeds; determinism + audio bounds + golden hashes |
| `tests/test_smoke_live.sh` | Live-mode smoke + PTY terminal-restore checks + MIDI wildcard smoke |
| `tests/unit/test_*.c` | 182 unit tests across arena, effects, voice, gen, lsystem, chord_progression, section, density, motif, mixer, wav, keys, midi |
| `golden/` | Reference hashes for the bit-exact regressions |
| `.github/workflows/ci.yml` | CI: build, all tests, Windows cross-compile, coverage gates, size gate |
| `.github/workflows/release.yml` | Tag-triggered release: full gates, installer drift gate, publishes checksummed binaries + `stretto.1` |
| `stretto.1` | Man page (hand-written roff; linted + help↔man drift-gated by `tests/test_cli.sh`) |
| `install.sh` | curl\|sh installer: sha256-verified release download, `~/.local`/root-aware install |
| `Formula/stretto.rb` | Homebrew formula (repo doubles as its own tap; Linux-only) |
| `packaging/aur/` | AUR `PKGBUILD` + `.SRCINFO` (unpublished) |
| `tools/` | Size-budget gate + Constitution↔Makefile bridge scripts (`make verify`) |

## Environment notes

- **Native Linux**: live audio via libpulse (`pa_stream` API on a threaded mainloop) — works against PulseAudio and PipeWire (`pipewire-pulse`). If no audio server is reachable, the error says so and points at `--render`, which needs none.
- **WSL2 + WSLg**: WSLg's audio pipe is unreliable for sustained playback. Run `stretto.exe` directly on Windows instead — it bypasses WSL entirely.
- **Windows**: live audio via Win32 `waveOut` (mmsystem). No external dependencies beyond `kernel32` and `winmm`.

## License

MIT. See `LICENSE`.
