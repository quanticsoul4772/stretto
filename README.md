# stretto

A generative music synthesizer. Plays live on Linux (PulseAudio) or Windows (waveOut), or renders to a 48 kHz stereo WAV file. C99, no malloc, single static arena.

| Binary | Size |
|---|---|
| Linux `synth` (stripped, links libpulse + libasound) | ~47 KB (48 584 B, 2026-07-11; budget 50 KB) |
| Windows `stretto.exe` (stripped + UPX) | ~38 KB (post-#117; budget 48 KB) |

## Build

### Linux

```
make
```

Produces `./synth`. Needs `gcc`, `make`, `libpulse-dev`.

### Windows (cross-compile from Linux / WSL)

```
make win        # produces stretto.exe (~247 KB, stripped)
make winpack    # additionally produces stretto.packed.exe (~38 KB, UPX-packed)
```

Needs `gcc-mingw-w64-x86-64` and `upx`. The packed `.exe` is a single-file native Windows binary — no WSL, no runtime dependencies beyond the bundled Windows kernel + multimedia DLLs.

### Optional: UPX-pack the Linux build

```
make pack
```

Produces `synth.packed` (~25 KB post-#117 — grows with the stripped binary; within the 30 KB Constitution cap, gated in CI).

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

### Hear it now

```
stretto                                   # live synth (q quits, ? = key map)
stretto --render 10 demo.wav --seed 42    # render 10 s; no audio server needed
man stretto
```

If `~/.local/bin` wasn't on your PATH before, restart your shell — Debian/Ubuntu's `~/.profile` adds it only when it exists at login, and `man stretto` resolves through PATH too.

### From source

```
make && sudo make install     # installs /usr/local/bin/stretto + man page
sudo make uninstall
```

Installs the binary under its canonical name `stretto` plus `man 1 stretto`. `PREFIX` and `DESTDIR` are honored (`make install DESTDIR=/tmp/pkg PREFIX=/usr` for packagers). Build **before** installing, as your user — `make install` deliberately does not build (a `sudo`-driven rebuild would embed `dev` as the version, since git refuses to run in another user's checkout).

Prebuilt binaries with `sha256sums.txt` are attached to [tagged releases](https://github.com/quanticsoul4772/stretto/releases) — Windows users should take the unpacked `.exe` (the `-upx` variants trip antivirus false positives).

### Homebrew (Linuxbrew) and AUR

```
brew tap quanticsoul4772/stretto https://github.com/quanticsoul4772/stretto
brew install stretto
```

The repo doubles as its own Homebrew tap (`Formula/stretto.rb`, sha256-pinned to the v1.3.0 tarball). **Linux(brew)-only**: no macOS audio backend exists (live audio is PulseAudio/waveOut, and `__APPLE__` currently routes MIDI to the ALSA backend, which cannot build on macOS). The AUR package definition lives at `packaging/aur/PKGBUILD` (+`.SRCINFO`), ready for the maintainer to push to `ssh://aur@aur.archlinux.org/stretto.git`. Both recipes build with `make STRETTO_VERSION=<ver>` so tarball builds (no `.git`) report the release version instead of `dev`, and install via the upstream `make install`.

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

`-` streams the WAV to stdout for pipelines (sox/ffmpeg convention; byte-identical to the file output — the RIFF header is written up front from the known duration, no seek-back):

```
./synth --render 60 - --seed 7 | ffmpeg -i - out.flac
./synth --render 10 - | aplay
```

A file literally named `-` needs `./-`. A downstream that closes early ends the render via SIGPIPE, standard pipeline behavior. Windows note: PowerShell older than 7.4 re-encodes binary pipes and redirection (byte-stream passthrough landed in 7.4) — use cmd.exe redirection or an output path there.

### Help and version

```
./synth --help       # usage + flag reference (stdout, exit 0)
./synth --version    # "stretto <version>" + license block (stdout, exit 0)
```

The version string is embedded at build time from `git describe`
(`1.2.0-final-N-g<hash>`, `-dirty` suffix for modified trees; `dev`
when building outside a git checkout). Both flags follow GNU Coding
Standards §4.8: stdout, exit 0, and they take precedence over every
other argument.

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

The resume line contains the seed (also captured for clock-seeded runs) plus **only the parameters you explicitly set** — via flag or live key. The synth's own internal mutation drift is never printed: `--seed` alone reproduces that drift from bar 0, so echoing drifted values back would make the recalled run *diverge* at the first mutation. Semantics worth knowing: recall reproduces a run with those values **from bar 0**, not the keystroke timing of your session; a mid-session scale change alters the chord-progression path from that point, so a recalled run matches your session's harmony only up to where you changed scale. Two quirks: the untouched cutoff default (200) sits above the `--cutoff` dial range (30–180) — omit the flag to keep it; MIDI CC tweaks are not captured (a `--midi` session isn't reproducible anyway, since your playing perturbs the voice pool).

## MIDI

Live MIDI keyboard input — plug a controller in, the synth plays notes as you press them (mixed over the generative output rather than replacing it). Two backends, identical interface:

| Backend | Platform | API |
|---|---|---|
| libasound sequencer | Linux | `snd_seq_open INPUT` + `snd_seq_connect_from` + polling worker pthread |
| WinMM | Windows | `midiInOpen` + `midiInProc` callback; sysex via `MIDIHDR` pool |

### Flags

| Flag | Default | Meaning |
|---|---|---|
| (no flag) | MIDI off | BSS-default `g_enabled = 0` — audio path is bitexact with `golden/regression_16s.sha256`. |
| `--no-midi` | — | Same as omitting the flag; passes `audio_midi_init(-1)` so the queue short-circuits and drain() is a no-op. Use this in CI when MIDI hw isn't available. |
| `--midi` | wildcard | Auto-subscribe to every readable ALSA `MIDI_GENERIC` / `MIDI_KEYBOARD` port (`snd_seq_query_next_client` + `snd_seq_query_next_port` walk; per-port connect failures tolerated). On WinMM, falls back to `WAVE_MAPPER` so the OS picks the default input. |
| `--midi <N>` | explicit | Bind to the N-th device from the most recent `--midi-list-devices` output. ALSA addresses decode `(client << 8) \| port` to `snd_seq_connect_from(client, port)`. WinMM passes the raw device id to `midiInOpen`. |
| `--midi-default` | explicit 0 | Alias for `--midi 0` (FR-002). |
| `--midi-list-devices` | — | Prints one line per enumerated input device as `<index> <name>`, capped at 32. Exit 0. Use to discover the N for an explicit `--midi N`. |
| `--midi-channel <1..16>` | all | Drop events whose channel != N at drain time, BEFORE CC dispatch. Omit the flag to accept all 16 channels (passing 0 is a usage error). Requires `--midi` or `--midi-default`. |

Startup failure is loud (FR-002): if `--midi` / `--midi <N>` / `--midi-default` cannot open a device, the synth prints `MIDI: ... unavailable (see --midi-list-devices)` and exits 1 — the continue-without-MIDI behavior applies only to a *mid-session* disconnect (FR-034). In `--render` mode MIDI is never opened, regardless of flags: the render loop is the queue consumer, so an open device could inject notes into a seeded render; a stderr notice is printed and the render stays byte-identical to a no-MIDI run (Constitution III).

### Mapping (FR-010)

- Note On / Off maps through the active scale (`SCALES[cur_scale]`) the same way the L-system marker does, so MIDI notes feel “in-scale” instead of chromatic. Velocity 0 on a Note On means Note Off (FR-011).
- Octave offset clamps to [-2, +4] so a MIDI note never strays out of the synth's audible range.
- 11-voice pool with Q1 voice-stealing policy: idle first, in-release second (max env_time), oldest third.

### CC mapping (US2 / FR-020)

| CC# | Name (MIDI 1.0) | Target | Scale |
|---|---|---|---|
| 1 | Mod Wheel | Filter cutoff | +1 |
| 7 | Channel Volume | Compressor threshold | +60 |
| 71 | Resonance / Timbre | Filter resonance | +1 |
| 74 | Brightness | Filter cutoff | +1 |
| 91 | Reverb Send | Reverb wet | +1 |
| 93 | Chorus / Delay | Delay wet | +1 |

Delta = `(V - 64) * scale`, summing additively across multiple CCs targeting the same param (FR-022). All other CCs (including CC#64 sustain pedal, CC#123 All Notes Off, the 4 General Purpose slots) are silently dropped per Principle VII — the synth continues playing from its own generative state on a disconnect.

### Disconnect (FR-034)

When the OS disconnects the MIDI source (ALSA `PORT_EXIT` / `CLIENT_EXIT`, winmm `MIM_CLOSE`), the synth does **not** auto-reconnect. It continues from internal generative state and the CC-modulated parameters retain their last value. The audio path is unaffected.

### Discoverability

```
./synth --midi-list-devices
3584 Midi Through Port-0
5120 MPK mini 3 MPK mini 3 MIDI 1
```

The first column is the `<index>` you pass to `--midi <N>`. On Linux it encodes the ALSA sequencer address as `(client<<8)|port` (3584 = client 14, port 0 — the same address `aconnect -l` shows as `14:0`); on Windows it is the plain 0-based device ordinal. `--midi 0` / `--midi-default` opens the first listed device on both platforms. Capped at 32 devices per the enumerator contract.

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
| `Ctrl-C` | Clean shutdown. Windows: arrives as keystroke `0x03`, routed through the `q` path. Linux: a signal handler restores the terminal (async-signal-safe) and re-raises, so the process dies by the signal with the terminal sane — same for `SIGTERM`, `SIGQUIT` (Ctrl-\), `SIGHUP`. |

Known issue: `Ctrl-Z` (SIGTSTP) suspends with the terminal still in raw mode until `fg` resumes — suspend/resume handling is deferred.

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
make test            # CLI contract + bit-exact regression (16 s seed-0 sha256)
                     # + Constitution<->Makefile bridge/amend regression suites
make test-unit       # 173 unit tests across all pure-synth modules + keys + MIDI
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

## Size budget amendment workflow

The post-#117 spec↔build cascade (PRs #121–#130) introduced a Constitution↔Makefile bridge: the 3 size budgets (Windows UPX ≤48 KB, Linux UPX ≤30 KB, Linux stripped ≤50 KB) live in BOTH `.specify/memory/constitution.md` Principle I AND the `Makefile` (as `WIN_PACK_BUDGET`, `PACK_TARGET`, `STRIP_TARGET`). Forgetting to bump one of them in a v1.X.0 amendment is what caused the original drift cascade. These two files are the only copies: the ci.yml `Binary size budget gate` reads its budgets from the `budget_*` rows that `make size` echoes into `binary-sizes.txt`, so an amendment never needs to touch ci.yml.

Two tools prevent future drift:

| Tool | Purpose | When to use |
|---|---|---|
| `tools/spec-budget-check.sh` | Read-only bridge: asserts Makefile byte values = Constitution KB values. Runs as ci.yml step `Bridge regression test` + `make test` target. | Automatic; no user action. Catches drift at PR-merge time. |
| `tools/spec-budget-amend.sh` | Write helper: bumps 1-3 budgets in BOTH files in lockstep, prints a `git diff` for review, refuses to commit. | Run manually before a v1.X.0 amendment PR. Makes the amendment a 1-invocation edit. |
| `tests/test_spec_budget_check.sh` | 5-scenario regression for the bridge (tamper Constitution, tamper Makefile, malformed constant, recovery via `git checkout`). | Automatic; runs on every PR. |
| `tests/test_spec_budget_amend.sh` | 6-scenario regression for the amend helper (happy-path, atomic multi-amend, input validation, dry-run, refuse-to-commit, recovery). | Automatic; runs on every PR. |
| `make verify` (calls `tools/verify-bridge.sh`) | One-command local dev check: runs the 3 verification artifacts in sequence (bridge check + 2 regression suites), exits on first failure with a clear per-step status + recovery hint. | **Run this before opening a PR** to catch spec↔build drift + amend helper regressions locally instead of waiting for CI. Equivalent to running the 3 dedicated ci.yml steps (Bridge regression test + Amend helper regression test + the inline Binary size budget gate's pre-flight) on a dev box. |

### Amending the size budgets (v1.3.0+ workflow)

```bash
# 1. Preview the change (no file modifications)
tools/spec-budget-amend.sh --dry-run --win 49

# 2. Apply the change (Constitution + Makefile updated in lockstep)
tools/spec-budget-amend.sh --win 49

# 3. Review the diff the script prints
git diff .specify/memory/constitution.md Makefile

# 4. (Optional) Update the Makefile rationale paragraph + Constitution footer
#    (the script leaves both unchanged since they're editorial content)

# 5. Commit manually
git add .specify/memory/constitution.md Makefile
git commit -m 'v1.X.0: bump <budget> from N to M KB per Principle I amendment'
```

**Flags:**

- `--win KB` — set Windows UPX-packed budget to KB (positive integer, can only grow)
- `--lin-upx KB` — set Linux UPX-packed budget to KB
- `--lin-str KB` — set Linux stripped budget to KB
- `--dry-run` — preview the changes without modifying any files
- `--help` / `-h` — show usage

At least one of `--win` / `--lin-upx` / `--lin-str` is required. Multiple flags can be combined for an atomic 3-budget amend (e.g. `tools/spec-budget-amend.sh --win 49 --lin-upx 32 --lin-str 60`).

**Budget-grow policy:** per the post-#117 amendment policy, budgets can only GROW, not shrink. To shrink (e.g. after a major refactor reduces binary size), do it manually with explicit Constitution + Makefile edits + an accompanying PR-body rationale. The helper rejects shrink attempts with `FATAL: ... budget can only grow per post-#117 amendment policy`.

**Refuse-to-commit:** the helper does NOT run `git add` or `git commit`. The developer reviews the printed `git diff` and commits manually. This is by design — amendment PRs need a v1.X.0 rationale in the commit message + PR body that the helper can't infer.

**Dirty-tree guard:** the amend test (`tests/test_spec_budget_amend.sh`) refuses to run if `.specify/memory/constitution.md` or `Makefile` have un-committed changes. Scope is limited to those 2 files (not the whole working tree) because the `make test` target runs `chmod +x tests/test_spec_budget_*.sh` first, and that mode change would trigger a whole-tree `git diff --quiet HEAD` check spuriously. To run the test, commit or stash changes to those 2 files first.

## CI step layout

`.github/workflows/ci.yml` defines 18 explicit steps. **Note on step numbering:** GitHub Actions auto-prepends `Set up job` to every job, so UI step numbers are **1-indexed from the auto-added step**. The explicit `actions/checkout@v4` step (no `name:`) renders as `Run actions/checkout@v4` / step #2. The YAML order is 0-indexed from `actions/checkout@v4` and internal to the file. Use UI numbers in PR bodies / commit messages.

| # (UI) | Step | Purpose |
|---:|---|---|
| 1 | Set up job | *(auto-prepended by GitHub Actions)* |
| 2 | Run actions/checkout@v4 | *(no `name:`; auto-renders)* |
| 3 | Install build deps | apt: gcc, make, libpulse-dev, libasound2-dev, upx-ucl, gcc-mingw-w64-x86-64, python3, python3-numpy |
| 4 | Build Linux synth | `make` |
| 5 | Bit-exact regression test | `make test` (CLI contract + bitexact + bridge + amend) |
| 6 | Bridge regression test (Constitution↔Makefile) | `bash tests/test_spec_budget_check.sh` — 5 scenarios / 9 sub-checks. Pre-flight for the Binary size budget gate (step 15). |
| 7 | Amend helper regression test (Constitution↔Makefile) | `bash tests/test_spec_budget_amend.sh` — 6 scenarios / 21 sub-checks |
| 8 | Unit tests | `make test-unit` (173 tests across 13 modules) |
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
| `audio_midi.c` | 98% | ≥90% |
| `main.c` | — | excluded (process-level argv branches; see Makefile `COV_SRCS_INTERACTIVE`) |
| `ui.c`, `keys.c`, `audio_pulse.c`, `audio_midi_linux.c` | — | excluded (require TTY / audio device / ALSA sequencer) |

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
| `audio_midi.c` / `.h` | Cross-platform MIDI core: SPSC event ring, scale-degree note mapper, CC→parameter dispatch (`CC_MAP`) |
| `audio_midi_linux.c` | ALSA sequencer MIDI input backend (wildcard subscribe + device enumeration, reader thread) |
| `audio_midi_winmm.c` | Windows MIDI input backend (`midiIn*` callback API) |
| `arena.c` / `.h` | Static 128 KB pool with bump allocator |
| `gen_*_table.c` | Build-time generators for sine / envelope / MIDI note / Bjorklund / wavetable tables |
| `version.h` (generated) | `#define STRETTO_VERSION` from `git describe`, written by a compare-and-swap Makefile rule so it only changes (and only `main.o` rebuilds) when the version does |
| `Makefile` | `make`, `make win`, `make winpack`, `make pack`, `make test`, `make test-unit`, `make test-multiseed`, `make test-smoke`, `make coverage`, `make debug`, `make golden`, `make golden-multiseed` |
| `tests/test_bitexact.sh` | Renders twice with `--seed 0`, sha256-compares, validates against golden |
| `tests/test_multi_seed.sh` | Renders 4 seeds; determinism + audio bounds + golden hashes |
| `tests/test_smoke_live.sh` | Spawns `./synth --no-ui` for 2 s; expects clean exit / SIGTERM |
| `tests/unit/test_*.c` | 173 unit tests across arena, effects, voice, gen, lsystem, chord_progression, section, density, motif, mixer, wav, keys, midi |
| `golden/regression_16s.sha256` | Reference hash for the 16-second seed-0 render |
| `golden/regression_multiseed.sha256.txt` | Reference hashes for the four multi-seed renders |
| `.github/workflows/ci.yml` | CI: build, all tests, Windows cross-compile, coverage gates, size gate |
| `.github/workflows/release.yml` | Tag-triggered release: full gates + version/cleanliness assertions, publishes checksummed binaries + `stretto.1` (rehearsable via `workflow_dispatch`) |
| `stretto.1` | Man page (hand-written roff; linted + help↔man drift-gated by `tests/test_cli.sh`) |
| `install.sh` | curl\|sh installer: sha256-verified release download, `~/.local`/root-aware install (offline-tested in `test_cli.sh`; drift-gated against every assembled dist by release.yml) |
| `Formula/stretto.rb` | Homebrew formula (repo doubles as its own tap; Linux-only; live — `brew install quanticsoul4772/stretto/stretto` works against the public repo) |
| `packaging/aur/` | AUR `PKGBUILD` + `.SRCINFO`, prepared but unpublished (no AUR account; kept for a future maintainer) |
| `tools/size-budget-gate.sh` | The 3-key binary size budget gate (shared by ci.yml and release.yml) |
| `PLAN.md` | Original design document (historical) |

## Environment notes

- **Native Linux**: live audio via libpulse direct (`pa_stream` API on a threaded mainloop).
- **WSL2 + WSLg**: WSLg's audio pipe is unreliable for sustained playback (multiple GitHub issues open against `microsoft/wslg`). Run `stretto.exe` directly on Windows instead — it bypasses WSL entirely.
- **Windows**: live audio via Win32 `waveOut` (mmsystem). No external dependencies beyond `kernel32` and `winmm`.

## License

MIT. See `LICENSE`.
