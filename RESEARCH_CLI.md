# RESEARCH: Growing Stretto as a CLI Tool

**Date**: 2026-07-08 · **Type**: research report (no code changes) · **Scope**: CLI ergonomics, reproducibility, packaging

This report answers: *what would make Stretto a genuinely great CLI tool* — great in
the Unix sense (composable, scriptable, predictable) — without violating the three
non-negotiables: bit-exact `--seed` determinism, the 128 KB arena / no-malloc model,
and the packed-binary size budget (48 KB CI gate; ~37 KB measured).

Everything below is grounded in the code as of branch `010-tasks-checklist-sync`
(commit `95d2918`). Where docs and code disagree, the code is cited.

---

## Status (updated 2026-07-08, post-#137)

The analysis below is the original snapshot; several items have since shipped.
This table is the live index — the body is kept for the reasoning (especially
§6's rejected alternatives, which should not be relitigated without new facts).

| Item | Status |
|---|---|
| M1 signal handling / terminal restore | **Shipped** — PR #135 (in-handler async-signal-safe restore + re-raise; also fixed the `O_NONBLOCK`-on-clean-quit leak and D8) |
| M3 reconcile `contracts/cli.md` with `main.c` | **Shipped** — PR #134 (contract amended to as-built; FR-002 exit-1 enforced; `--render` never opens MIDI) |
| F1 `--help` / `-h` / `--version` | **Shipped** — PR #137 (GNU §4.8; `version.h` from `git describe`; `tests/test_cli.sh`) |
| §7 mismatches D1, D3–D6, D8 | **Fixed** — PRs #134/#135; D7 was confirmed a non-bug in the original analysis |
| M2 non-TTY stdin: auto-`--no-ui` instead of `exit(1)` | **Shipped** — 050 (`isatty(0) \|\| isatty(1)` degrade in `ui_term_raw_mode`, stderr notice, Windows-path parity) |
| F2 `--render N -` streams WAV to stdout | **Shipped** — 046 (byte-identical to file output; SIGPIPE default kept; fwrite/fclose error discipline added) |
| F3+F4 preset capture (initial-state flags + print-state-on-quit) | **Shipped** — 047 (`specs/004-preset-capture`; dirty-bit capture — only user-set params are printed, since `--seed` alone reproduces mutate() drift and echoing drifted values would diverge) |
| F5 packaging minimum (man page, tagged releases + checksums) | **Shipped** — 048 (`stretto.1` + `make install` + release.yml with version/cleanliness/size gates; first tag v1.3.0 pending) |
| P1 `NO_COLOR` | **Shipped** — 052 (SGR stripped at the single write site; functional escapes kept) |
| P3 brew/AUR | **Prepared** — 053 (`Formula/stretto.rb` self-tap + `packaging/aur/PKGBUILD`; publication blocked on the repo being private — source URLs 404, and the tarball sha256 cannot be pinned until public. Brew formula is Linux-only: no macOS audio backend exists) |
| P2 exit-code polish | **Won't-do** (closed 2026-07-09) — 0/1 plus shell-reported signal deaths is the documented contract (man page EXIT STATUS, 003/004 contract tables, test assertions). Switching usage errors to 2 or adopting sysexits would break scripts checking `== 1` and churn every one of those surfaces for no consumer: nothing programmatically distinguishes usage-vs-runtime failures today, and nobody has asked to. This report rated it borderline cargo-culting from the start (§2). |
| N1 key record/replay | Open — deferred until someone wants exact session replay |
| §7 D2 (live help overlay missing `l`/`L` keys) · D9 (positional arg cap silently drops) | **Shipped** — 051 (overlay line added; overflow is now a loud usage error) |

---

## 1. The CLI surface as it actually is

From `main.c` (argv pre-scan at `main.c:43-89`, dispatch at `main.c:134-161`):

| Invocation | Behavior |
|---|---|
| `synth` | Live mode: raw terminal, oscilloscope, key controls. Seeds from `time(NULL)`. |
| `synth --no-ui` | Live mode, headless: no raw mode, no scope, no key handler; runs until killed. |
| `synth --render <1..3600> <out.wav>` | Offline render to WAV; `arena: N/131072 bytes used` printed to stderr; exit 0. |
| `--seed N` | Position-independent. Fixes PRNG/CA/Markov seeds. **Works on both paths** — `gen_seed()` sets `gen_seeded_explicitly = 1` (`gen.c:349`) and `gen_init()` only falls back to `time(NULL)` when that flag is unset (`gen.c:373-375`). The suspected live-path reseed bug does not exist. |
| `--midi [N]`, `--midi-default`, `--no-midi`, `--midi-channel 1..16`, `--midi-list-devices` | Position-independent (pre-scanned). `--midi-list-devices` prints `<index> <name>` lines and exits 0. |

What is **absent**:

- No `--help`, `-h`, or `--version`. Any unrecognized argument (including `--help`)
  falls through to a usage line on **stderr** with **exit 1** (`main.c:156-160`).
  The usage string omits every `--midi*` flag.
- Exit codes are 0 (success) and 1 (everything else: usage error, unwritable output,
  arena OOM at `arena.c:12-14`, PulseAudio failure).
- No signal handlers anywhere (verified: only `atexit()` registrations at
  `audio_pulse.c:38` and `audio_winmm.c:41`; zero `signal`/`sigaction` calls in the
  tree). Consequences in §4 finding M1.
- No stdout streaming: `render_wav()` does `fopen(path, "wb")` (`wav.c:36`); `-` is
  treated as a filename. *(Fixed in 046: `-` streams to stdout.)*
- No `NO_COLOR` / TTY awareness on Linux: `ui_term_raw_mode()` **exits with an
  error** if stdin is not a TTY (`ui.c:90-93`), and ANSI color escapes are emitted
  unconditionally. (Windows degrades gracefully: `GetConsoleMode` failure flips
  `no_ui_flag`, `ui.c:147-152`.)
- Flags and positionals are order-sensitive in one spot: `--render` must be the
  first surviving positional and `--no-ui` cannot combine with `--render` at all
  (`main.c:134,153`) — harmless today since `--render` opens no terminal, but it
  means `synth --no-ui --render 60 x.wav` is a usage error.

Live state is all reachable through existing setters (`keys.c:20-45` calls
`gen_set_tempo`, `gen_cycle_scale`, `gen_adjust_gate`, `voice_adjust_cutoff/resonance/
lfo_filter_depth`, `voice_set_mod_depth`, `voice_cycle_filter_mode`,
`reverb_adjust_wet`, `delay_adjust_wet/feedback`, `compressor_adjust_threshold`) —
important later: initial-state flags need almost no new engine API.

---

## 2. Conventions assessed: what fits, what would be cargo-culting

Sources drawn on: **POSIX.1-2024 XBD §12.2 Utility Syntax Guidelines** (guidelines
3–14: option parsing, operand order, `-` as stdin/stdout by convention), **GNU Coding
Standards §4.8** (`--help` and `--version` must write to **stdout** and **exit 0**),
**clig.dev** (Command Line Interface Guidelines: help behavior, machine output to
stdout vs. messages to stderr, respect `NO_COLOR`, "if `-` is passed as a file
argument, read/write the stream instead"), the **no-color.org** spec (any non-empty
`NO_COLOR` env var disables default color output), and **reproducible-builds.org**
(`SOURCE_DATE_EPOCH`).

| Convention | Verdict for a tiny synth |
|---|---|
| `--help`/`-h`/`--version` → stdout, exit 0 | **Fits.** This is the single most-tested behavior by users, scripts, and packagers (`man`, Homebrew audits, AUR helpers all probe `--version`). Cost: ~0.5 KB of `.rodata` strings that UPX compresses well. |
| Exit-code discipline | **Fits, minimally.** 0 success / 1 runtime error / 2 usage error (the `grep`-style trio) is enough. Adopting the full BSD `sysexits.h` taxonomy would be cargo-culting. |
| `-` = stdout for render output | **Fits unusually well.** The WAV header is written up-front from the known duration (`wav.c:8-33`) — no seek-back — so streaming into a pipe works with zero format changes. `stretto --render 60 - \| ffmpeg -i - out.flac` becomes real. |
| Signal handling → terminal restore | **Not a convention question — a correctness bug today.** See M1. |
| Non-TTY detection | **Fits.** Windows already auto-degrades; Linux should stop `exit(1)`-ing on non-TTY stdin and instead behave as `--no-ui`. This is parity, not new machinery. |
| `NO_COLOR` | **Fits, barely.** The scope is a full-screen TUI, which the spec arguably exempts, but honoring it is ~10 lines and one `getenv`. Monochrome scope (keep the glyph ramp `. - + * # @`, drop the escapes) still reads fine. |
| Config files (XDG, dotfiles) | **Does not fit.** Fights the constitution (no config parser, no hidden state) — see Do-not-do. |
| Shell completions, terminfo, locale | **Cargo-culting** at this binary size and flag count. Skip. |

---

## 3. Prior art: what comparable tools actually do

- **sox** — the canonical composable audio CLI. Filenames `-` mean stdin/stdout with
  `-t <type>` to disambiguate format when there's no extension. Lesson: Stretto only
  ever emits one format (48 kHz s16le stereo WAV), so `-` needs no `-t` machinery —
  adopt the filename convention, skip the type system.
- **ffmpeg** — accepts `-` / `pipe:` for input and output; **all logging goes to
  stderr** so stdout stays clean for data. Stretto already does this correctly
  (the arena report and all errors go to stderr) — the discipline just needs to be
  kept when adding output. ffmpeg also demonstrates `-nostdin` (don't consume the
  terminal when scripted) — Stretto's `--no-ui` is the same idea, already present.
- **fluidsynth** — the closest "synth as CLI" precedent: `-n -i` for headless,
  `-F out.wav` for faster-than-realtime render, `-o key=value` for initial settings,
  `-R/-C` reverb/chorus toggles as startup flags. Confirms: startup parameter flags
  are the established pattern for synth CLIs, not config files.
- **timidity / espeak-ng** — both stream renders to stdout for piping (`timidity
  -Ow -o -`, `espeak --stdout \| aplay`). This is the standard composition idiom
  for terminal audio tools; Stretto is the odd one out in not supporting it.
- **sointu** (demoscene, the size-class peer) — splits into `sointu-track` (author),
  `sointu-compile` (deterministic song→code), `sointu-play` (play a file). Its key
  property is that a *song is a text artifact you can commit*. Stretto's analog of
  "the song file" is `--seed N` **plus the starting parameters** — which is exactly
  the preset-capture story in §5.3. Also NB: sointu ships versioned release binaries
  and that alone made it packageable downstream.
- **bytebeat one-liners** — the culture pipes raw PCM through `aplay`/`sox` (`echo
  'main(t){...}' \| tcc -run - \| aplay`). The floor of composability: audio on
  stdout. Reinforces §5.2.
- **amy (shorepine)** — controlled by a compact ASCII "wire protocol" string. An
  anti-model for Stretto's CLI (it's a library API), but its determinism note is
  apt: same message stream → same audio. Stretto's equivalent guarantee (same seed +
  same flags → same bytes) is stronger and worth advertising in `--help`.

Concrete patterns worth borrowing: `-` filename (sox/ffmpeg), stderr-only logging
(ffmpeg — already true), startup-parameter flags (fluidsynth), committed-text
reproducibility + versioned releases (sointu), stdout-PCM piping culture (timidity/
bytebeat).

---

## 4. Prioritized improvements

Value is judged for a terminal power user; effort in rough person-hours for someone
who knows the codebase. **None of these touch the audio path, consume PRNG draws, or
allocate from the arena** unless noted; determinism impact is stated per row.

| # | Improvement | Tag | Value | Effort | Determinism / size / arena impact |
|---|---|---|---|---|---|
| M1 | SIGINT/SIGTERM handling that guarantees terminal restore (Linux live mode) | [correctness] | **High** — today Ctrl-C or `kill` during live mode leaves the terminal in raw mode, echo off, cursor hidden | Low (~1 h) | None / +~150 B / none |
| M2 | Linux non-TTY behavior: auto-`--no-ui` instead of `exit(1)` when stdin isn't a TTY; suppress escapes when stdout isn't a TTY | [correctness] | High for scripting (`synth < /dev/null` currently dies) | Low | None / negligible / none |
| M3 | Reconcile `main.c` with `specs/003-midi-input/contracts/cli.md` (list format, exit-1 on failed `--midi N` open, mutual exclusions, stale "US3 stub" message) | [correctness] | Medium — the contract says tests "MUST assert against these exact strings" and the code diverges (§6, D3–D5) | Low–Med | None |
| F1 | `--help`/`-h` and `--version` → stdout, exit 0; usage string that lists **all** flags | [feature] | **High** — baseline trust signal; packaging prerequisite | Low (~1 h) | None / +~0.5 KB `.rodata` / none |
| F2 | `--render N -` streams WAV to stdout | [feature] | **High** — unlocks `\| ffmpeg`, `\| aplay`, `\| sox` pipelines with ~10 lines of change | Low | Byte-identical output; needs `_setmode(_fileno(stdout), _O_BINARY)` on Windows |
| F3 | Initial-state flags: `--scale`, `--tempo`, `--gate`, `--cutoff`, `--resonance`, `--filter-mode`, `--lfo-depth`, `--mod-depth`, `--reverb`, `--delay`, `--feedback`, `--comp-threshold` | [feature] | **High** — renders no longer always start from defaults; the recall half of preset capture | Medium (~4 h) | Safe: setters consume no PRNG; applied after `gen_init()` before the first sample, output is a pure function of (seed, flags). ~1 KB of parse code |
| F4 | Print-state-on-quit: on `q`/signal, emit one pasteable `--seed N --scale ... --cutoff ...` line to stderr | [feature] | Med-High — the capture half; costs a dozen `fprintf`s | Low | None / ~300 B / none |
| P1 | `NO_COLOR` (and monochrome fallback) for status row + scope | [polish] | Low-Med | Low | None |
| P2 | Exit code 2 for usage errors; document codes in `--help` and README | [polish] | Low | Low | None |
| F5 | Man page (`stretto.1`, hand-written roff or scdoc source committed) + `--version` string wired from git tag via `-DSTRETTO_VERSION` in Makefile + release binaries with sha256sums attached to GitHub tags | [feature] | Medium — the realistic packaging minimum (§5.5) | Medium | None (keep `__DATE__` out; honor `SOURCE_DATE_EPOCH` if a build date is ever wanted) |
| P3 | Homebrew tap formula + AUR `PKGBUILD` | [polish] | Low-Med (audience-dependent) | Medium | None — but note UPX-packed binaries trigger AV false positives on Windows; ship the **unpacked** exe as the primary release artifact and the packed one as the size-budget artifact |
| N1 | Key-event record/replay (`--record-keys f` / `--replay-keys f`: lines of `<sample_clock> <key>`) for exact reproduction of a *tweaked* session | [nice-to-have] | Low today | Med-High | Deterministic by construction, but new surface + file I/O; defer until someone asks |

---

## 5. Sketches for the top items (proposals, not implementations)

### 5.1 M1 — Signal handling with guaranteed terminal restore

**Problem.** `ui_term_raw_mode()` clears `ICANON|ECHO` but leaves `ISIG` set
(`ui.c:96`), so Ctrl-C raises SIGINT; with no handler installed the process dies at
default disposition and **atexit handlers do not run** (C11 §7.22.4.4 / POSIX: only
`exit()` runs them). The README's claim that Ctrl-C restores the terminal via atexit
is only true on Windows, where Ctrl-C arrives as keystroke `0x03` because
`ENABLE_PROCESSED_INPUT` is cleared (`ui.c:134-137,154`) and quits through the `q`
path.

**Approach.** Smallest-correct version, following the standard pattern (see also
clig.dev "Responsiveness"/cleanup guidance):

- `ui.c` (or `main.c`): `static volatile sig_atomic_t g_quit;` and a 3-line handler
  that sets it. Install for `SIGINT`, `SIGTERM`, `SIGHUP` via `sigaction` (no
  `SA_RESTART`, so blocked syscalls return `EINTR`).
- `audio_pulse.c:111` loop: check `g_quit` each iteration (the loop already wakes
  every ~21 ms buffer) and route into the existing `KEY_QUIT` cleanup block at
  `audio_pulse.c:132-143`. `pa_threaded_mainloop_wait` may need a
  `pa_threaded_mainloop_signal` from... nothing — it already wakes on every write
  callback, so worst-case quit latency is one buffer. Acceptable.
- Alternative belt-and-braces: since `tcsetattr` and `write` are async-signal-safe
  (POSIX XSH 2.4.3), the handler *may* call `ui_term_restore_mode()` directly before
  `_exit(130)`. The flag-check approach is cleaner (also drains PulseAudio) and is
  the recommendation; direct restore in the handler is a fallback only if quit
  latency ever matters.
- Exit status: `128+signum` (130 for SIGINT) per shell convention, or 0 — either is
  defensible; `test_smoke_live.sh:38` already accepts 143, keep that working.

**Files**: `ui.c`/`ui.h` (+flag, +installer), `audio_pulse.c` (loop check),
`audio_winmm.c` (optional `SetConsoleCtrlHandler` for console-close symmetry),
README (fix the atexit claim either way). No determinism, size, or arena impact.

### 5.2 F2 — `--render 60 -` streams RIFF to stdout

**Behavior.** `synth --render 60 - --seed 7 | ffmpeg -i pipe:0 out.flac`. Output
bytes are identical to the file path — the header is already written up-front with
sizes computed from the fixed duration (`wav.c:8-33`), so no `fseek` is needed and
pipes just work.

**Approach.** In `render_wav()` (`wav.c:35-38`):

```c
FILE *f = strcmp(path, "-") == 0 ? stdout : fopen(path, "wb");
/* Windows: if streaming, _setmode(_fileno(stdout), _O_BINARY) first,
   else CRLF translation corrupts the RIFF stream. */
/* on the stdout path: fflush(stdout) instead of fclose(f) */
```

Guard rails: keep the arena report and any errors on stderr (already true); while
streaming, nothing else may write to stdout (already true — render mode prints
nothing to stdout). Improve the open-failure message to include `strerror(errno)`
while touching this function (`wav.c:38` currently prints just `open <path>`).

**Files**: `wav.c` (~10 lines), `main.c` usage text, README. Tests: the bit-exact
regression can gain a one-line variant (`--render 16 - > f.wav` hash-equals the file
render). Determinism: byte-identical by construction. Size: negligible.

### 5.3 F3 + F4 — Preset capture: initial-state flags + print-state-on-quit

Three candidate patterns were weighed against the constitution (tiny binary, no
config parser, no hidden state):

| Pattern | Verdict |
|---|---|
| **(a) Pasteable flags**: print current params on quit; accept the same params as startup flags | **Recommended.** Zero file I/O, zero parser beyond argv (which exists), state is visible in the invocation, shell history *is* the preset store, and it composes with `--render` for free. |
| (b) Keyed dotfile (`w` writes `~/.strettorc`, auto-loaded at start) | Rejected: needs a file-format parser, introduces hidden state that silently changes what `synth --seed 5` produces — a determinism-adjacent trap and a direct constitution conflict. |
| (c) Packed state string (`--state b32:...`) | Rejected: encoder+decoder is *more* code than flags, and the artifact is opaque — not greppable, not diffable, not hand-editable. Flags are the transparent version of the same idea. |

**Flag sketch** (each maps to one existing setter; integer arguments; parse with the
`strtoul` pattern already in `main.c:44-51`):

```
--scale dorian|lydian|phrygian|locrian|harmminor|mixolydian   (or 0..5)
--tempo <pct>        e.g. 120 = 20% faster than default   → gen_set_tempo delta from 100
--gate 64..240       → gate_prob                (new tiny setter or adjust-from-default)
                       (as-built: [32,255] — the sketch's 64..240 was the mutate-drift clamp, not the user range)
--cutoff 30..180     → voice cutoff base
--resonance 0..180   → voice resonance base
--lfo-depth 0..255   → filter LFO depth
--filter-mode lp|hp|bp|notch
--mod-depth 100..8000
--reverb 0..256      → reverb wet
--delay 0..256 --feedback 0..200
--comp-threshold 8000..30000
```

Application point matters: after `gen_init()` + `effects_init()` (which reset
defaults, `gen.c:352-401`), before the first `render_chunk`. Setters consume no PRNG
draws, so output stays a pure function of (seed, flags) — determinism is *extended*,
not threatened: the golden tests (no flags) are untouched, and a new multiseed-style
case can pin one flagged render. Most controls need no new engine API; a couple need
trivial absolute setters next to the existing relative `adjust_*` (e.g.
`gen_set_scale(uint8_t)` beside `gen_cycle_scale()`, `gen.h:14`).

**Print-on-quit** (F4): in the `KEY_QUIT` path (and the M1 signal path), emit one
line to **stderr** (stdout may be a pipe carrying audio):

```
resume with: stretto --seed 3735928559 --scale lydian --cutoff 140 --reverb 96 ...
```

Include the actual seed used — which means `gen_seed()` should stash the pre-hash
input value (one `static uint32_t`), so clock-seeded runs become reproducible after
the fact. That is arguably the single highest-joy feature in this report: *every*
run becomes recallable.

**Honest caveat to document**: replaying seed+flags reproduces a run with those
parameters *from bar 0*, not the exact audio of a session where you tweaked knobs
mid-flight. Parameter setters don't consume PRNG draws, so the note sequence
matches; but a mid-run **scale** change alters which chord-Markov table subsequent
draws index (`gen.c:422-423` passes `cur_scale`), so the progression can diverge
from the moment of the change. Exact session replay is N1 (key-event log), which is
deliberately deferred.

**Files**: `main.c` (parse + apply + usage), `gen.c/.h`, `voice.c/.h`,
`effects.c/.h` (a few absolute setters), `keys.c` or the two `audio_*.c` quit paths
(print), README, and a new unit-test file for flag parsing. Size estimate: ~1–1.5 KB
uncompressed, well inside the 48 KB gate (current headroom ≈ 11 KB packed).

### 5.4 F1 — `--help` / `-h` / `--version`

Per GNU Coding Standards §4.8: both write to **stdout** and exit 0. `--version`
prints `stretto <ver>` on line 1 (version injected via `-DSTRETTO_VERSION=\"$(git
describe --tags --always)\"` in the Makefile; fallback `"dev"`), optionally
license + "written by" lines. `--help` prints: one-line description, usage synopsis
covering **all** flags (render, seed, no-ui, all five `--midi*`, and the F3 set),
the determinism promise ("same --seed and flags ⇒ byte-identical audio"), and where
to find the key map (`?` in live mode). Handle these **before** the MIDI dispatch
block in `main.c` so `--help` never touches audio/MIDI subsystems. Keep unknown-flag
errors on stderr with (optionally, P2) exit 2.

**Files**: `main.c`, `Makefile` (version define), README. ~0.5 KB `.rodata`.

### 5.5 F5 — Packaging minimum

The realistic floor for "installable and trusted", in order:

1. **`--version`** (F1) — packagers and users probe it; without it a formula is
   guesswork.
2. **Man page** — one hand-written `stretto.1` committed to the repo (or scdoc
   source rendered at release time; do *not* add a doc toolchain to `make all`).
   Content is essentially README §Run + §Keyboard controls reflowed.
3. **Tagged releases with binaries + `sha256sums.txt`** — CI already builds and
   uploads the Windows artifact on every push; the delta is a release workflow
   triggered on tags that attaches `synth` (Linux, stripped), `stretto.exe`
   (unpacked — see AV note), `stretto.packed.exe`, and checksums.
4. **Reproducible builds** — mostly free here: no `__DATE__`/`__TIME__` in the
   tree, tables are committed-deterministic by design (ARCHITECTURE.md
   §Determinism). Pin the compiler version in the release workflow and the binary
   hash becomes checkable; mention `SOURCE_DATE_EPOCH` in the workflow for
   completeness (reproducible-builds.org).
5. **Then** Homebrew tap (a 15-line formula building from source with `make`) and
   AUR `PKGBUILD` — both trivially easy *after* 1–3 exist, pointless before.

Caveat worth stating in release notes: UPX-packed executables are routinely
false-positived by Windows AV. The packed binary is the demoscene artifact; the
unpacked ~215 KB exe should be the recommended download.

---

## 6. Do not do

- **Config files / dotfiles / XDG paths.** Hidden state that changes what an
  invocation produces breaks the "seed+flags = the piece" contract, and a config
  parser is exactly the machinery the constitution excludes. The preset story is
  flags (§5.3); the preset *store* is shell history and shell scripts.
- **A TUI framework (ncurses etc.).** Already rejected in PLAN.md (§J) for size
  (~150 KB); nothing has changed. The hand-rolled VT100 UI is the right call.
- **Live-mode audio on stdout.** Unlike `--render -`, live streaming couples the
  real-time audio loop to pipe backpressure (a slow consumer stalls playback or
  forces buffer-drop policy). Anyone who wants a pipe has `--render`; anyone who
  wants live piping has `--render 3600 - | aplay`. Adding a second streaming path
  is complexity without a user.
- **Machine-readable status output (JSON status row, `--json`).** No consumer
  exists; the status row's stable field labels are already scrape-friendly. YAGNI.
- **OSC / network / IPC control surface.** Wrong project. MIDI input already
  provides external control within budget.
- **Packed/opaque state strings** (`--state <blob>`). More code than flags, less
  transparent (§5.3c).
- **Full `sysexits.h` exit-code taxonomy, shell completions, locale/terminfo
  handling.** Ceremony disproportionate to a two-mode binary; the 0/1/2 trio plus a
  man page covers every real consumer.
- **Auto-update, telemetry, "check for new version".** Obviously — but stated for
  completeness: a 37 KB deterministic binary's whole appeal is that it does nothing
  you didn't ask for.
- **Snap/Flatpak.** Sandboxed-runtime packaging for a 16 KB static-ish binary
  inverts the value proposition. Tarball + checksums, brew, AUR are the fit.

---

## 7. Code/doc mismatches found while reading

- **D1 [correctness-relevant]** README §Keyboard controls: "`Ctrl-C` — Same as `q`
  via atexit handler." False on Linux: no signal handler exists, `ISIG` is left
  enabled (`ui.c:96`), and atexit handlers do not run on signal death — Ctrl-C or
  SIGTERM during live-with-UI leaves the terminal raw with the cursor hidden. True
  on Windows only because Ctrl-C arrives as keystroke `0x03` (`ui.c:134-137`). Fix
  is M1 (preferred) or a README correction.
- **D2** Help overlay (`ui.c:36-55`) is missing the `l` / `L` compressor-threshold
  keys, which exist in `keys.c:44-45` and are documented in the README key table.
- **D3** `specs/003-midi-input/contracts/cli.md` vs `main.c`, several divergences in
  a contract that declares its strings/exit codes normative ("tests MUST assert
  against these exact strings"):
  - List format: contract §3 specifies `0: <name>` and a `0 devices found` line for
    the empty case; code prints `%d %s` (no colon) and **nothing** when empty
    (`main.c:101-109`).
  - `--midi N` open failure: contract says exit 1 with `midi: device index N not
    found; run with --midi-list-devices...`; code prints a different message and
    **continues without MIDI** (`main.c:115-124`).
  - Mutual exclusions (`--midi` + `--midi-list-devices`, `--midi-channel` without
    `--midi`) are specified as usage errors; code enforces neither (`main.c:101`,
    `main.c:125-128`).
  - Channel range: contract allows `--midi-channel 0` (= all); code rejects 0 with
    a `1..16` message (`main.c:65-69`).
- **D4** `main.c:119-121` error text says the MIDI backend "lands in US3 T022/T023"
  — stale: the real ALSA backend exists (`audio_midi_linux.c:138-156`,
  `snd_seq_open` + `pthread_create`), as ARCHITECTURE.md correctly describes.
- **D5** `Makefile:230-233` coverage comment still claims `audio_midi_linux.c` is a
  "stub returns -1" — same staleness as D4.
- **D6** Both usage strings (`main.c:136-138`, `main.c:157-159`) omit all five
  `--midi*` flags.
- **D7 (confirmed non-bug)** The suspected loss of an explicit `--seed` on the live
  path does **not** occur: `gen_seed()` marks `gen_seeded_explicitly`
  (`gen.c:322-350`) and `gen_init()` skips the `time(NULL)` reseed when set
  (`gen.c:373-375`). README's determinism claims are accurate.
- **D8 (minor)** `ui_term_restore_mode()` writes 8 bytes of the 7-character string
  `"\x1b[?25h\n"` — the trailing NUL is written to the terminal (`ui.c:113`,
  `ui.c:163`).
- **D9 (minor)** The argv pre-scan caps positionals at 8 (`main.c:43`,
  `positional[8]`); arguments beyond that are silently dropped rather than
  rejected.

---

## 8. Suggested sequencing

1. **M1 + M2 + D1/D2 doc fixes** — correctness, small, no design debate.
2. **F1 + F2** — one small PR each; immediately visible payoff; F1 unblocks F5.
3. **F3 + F4 together** — one spec-kit feature ("preset capture"): flags, print-on-
   quit, seed stashing, tests pinning a flagged render.
4. **M3** — decide whether the contract or the code is right, then align (the
   contract's exit-1-on-open-failure conflicts with FR-005's "continue without
   MIDI"; that tension needs a spec decision, not just a patch).
5. **F5**, then P-items opportunistically.
