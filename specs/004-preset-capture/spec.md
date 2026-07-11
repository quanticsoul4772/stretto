# Feature Specification: Preset Capture (initial-state flags + resume line)

**Feature dir**: `specs/004-preset-capture` · **Date**: 2026-07-09 · **Status**: shipped with the implementing PR

> Lean artifact set (spec + CLI contract only), declared per the Governance
> clause: this feature is CLI surface + thin engine setters over existing
> tunables — no new synthesis, threading, or memory-model ground to cover
> with research/data-model/tasks artifacts. RESEARCH_CLI.md §5.3 (committed)
> is the research record; the plan-of-record lived in the reviewed
> implementation plan for branch `047-preset-capture`.

## Why

The engine is deterministic per seed, but live tweaks were lost on quit and
renders always started from defaults. Preset capture makes every session
recallable as a pasteable command line: flags in, resume line out. Pasteable
flags were chosen over dotfiles/state-strings in RESEARCH_CLI.md §5.3/§6 —
shell history is the preset store; no config parser, no hidden state
(Constitution II/VII-adjacent minimalism).

## Functional requirements

### Initial-state flags
- **FR-101** (amended 2026-07-11, 069): System MUST accept the 13
  initial-state flags with these inclusive ranges, rejecting
  out-of-range or malformed values as usage errors (stderr + exit 1;
  never silent clamps):
  `--scale <name|0-5>`, `--bar-ms <760-7600>`, `--gate <32-255>`,
  `--mod-depth <100-8000>`, `--cutoff <30-180>`, `--resonance <0-180>`,
  `--lfo-depth <0-255>`, `--filter-mode <lp|hp|bp|notch|0-3>`,
  `--reverb <0-256>`, `--delay <0-256>`, `--feedback <0-200>`,
  `--comp-threshold <8000-30000>`, `--swing <0-100>`. Ranges mirror the
  live-key adjusters' clamps exactly (`--swing` has no live key - the
  first flag-only parameter; see FR-105).
- **FR-102**: Scale names are `dorian lydian phrygian locrian harmminor
  mixolydian` (0..5); filter-mode names `lp hp bp notch` (0..3). Numeric
  forms are always accepted.
- **FR-103**: Flags apply after engine init and before the first rendered
  sample, in both live and render modes. Setters MUST NOT consume PRNG
  draws: output is a pure function of (seed, flags). A render with all
  flags at their explicit defaults is byte-identical to a flagless render.
- **FR-104**: The cutoff dial range [30,180] deliberately excludes the
  compile-time default (200). Omitting `--cutoff` keeps the default; the
  flag cannot express it.
- **FR-105** (added 2026-07-11, 069): `--swing <0-100>` delays every
  second 16th-note (the odd 16th-substeps of the 48-substep bar) by
  `min(spss * swing/100, spss-1)` samples, spss = samples per substep.
  0 = straight and is the byte-inert default; the scale is delay-only
  (not MPC's 50-centered - the model cannot pull a note earlier than
  the grid); the MPC mapping is `MPC% = 50 + swing/6`, so 100 is one
  sample short of true triplet (66.7%). The spss-1 cap guarantees a
  swung tick never reaches the next grid slot. Swing is TIMING-ONLY
  and composition-invariant: the substep sequence, and with it every
  PRNG draw, MUST be identical at any swing value - swing moves
  onsets, never note content. Bar boundaries, the CA advance, bass,
  chord and kick sit on even 16ths and MUST stay straight. `--swing`
  has no live key and no status-row field.

### Resume line
- **FR-110**: On clean quit (`q`) and on SIGINT/SIGTERM/SIGHUP/SIGQUIT
  (POSIX), live mode MUST print to stderr one line:
  `resume with: --seed <raw-seed>` followed by ` --<flag> <value>` for each
  parameter the user explicitly set. The raw (pre-hash) seed is stashed for
  both explicit `--seed` and the clock-derived fallback.
- **FR-111**: Only *user-set* parameters appear (CLI flag or live key —
  dirty bits marked at those call sites). Internal `mutate()` drift is
  NEVER captured: `--seed` alone reproduces drift from bar 0, and printing
  drifted values would seed the recall with them as initial values and
  diverge at the first mutation.
- **FR-112**: The line is a snapshot at the user's LAST action (initial
  snapshot at startup covers untouched/headless sessions). The signal-path
  write is async-signal-safe (double-buffered line; `write(2, ...)`),
  including in `--no-ui` mode.
- **FR-113**: Render mode prints `resume with: --seed <raw>` to stderr at
  render start (captures unseeded renders on any exit path; the other flags
  are already on the user's command line; stdout may carry WAV bytes).
- **FR-114**: MIDI CC tweaks are NOT captured (a `--midi` session is
  already non-reproducible: external note events perturb voices). Windows
  capture is partial: interactive Ctrl-C prints (keystroke `0x03` → `q`
  path); a hard console-kill in `--no-ui` mode prints nothing.

### Semantics
- **FR-120**: Recall reproduces a run with the printed parameters FROM BAR
  0 — not the exact audio of a session whose knobs moved mid-flight.
  Setters consume no PRNG so the note sequence matches; a mid-run scale
  change alters which chord-Markov table subsequent draws index, so
  progressions can diverge from the change point (documented caveat).

## Success criteria
- **SC-001**: `--render 2 x.wav --seed 0 <all-default flags minus --cutoff>`
  byte-equals the flagless render (asserted in `tests/test_cli.sh`).
- **SC-002**: A flagged render differs from flagless and is byte-identical
  across repeated runs (asserted).
- **SC-003**: Out-of-range values exit 1 with the exact-range message on
  stderr, nothing on stdout (asserted for 6 representative cases).
- **SC-004**: A PTY session that presses `s` then `q` prints
  `resume with: --seed <N> --scale lydian` (asserted in
  `tests/test_smoke_live.sh`).
- **SC-005**: A `--no-ui` session with a CLI flag, killed by SIGTERM,
  prints the seed + that flag via the signal handler (verified in the
  implementing PR).
- **SC-006**: Goldens unchanged; all pre-existing suites pass unmodified.
