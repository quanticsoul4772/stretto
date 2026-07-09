# CLI Contract: 004-preset-capture

**Date**: 2026-07-09 · **Spec**: [../spec.md](../spec.md)

Executable contract for the initial-state flags + resume line. `main.c`'s
flag table, `keys.c`'s builder, and the tests assert against these exact
strings, ranges, and exit codes.

## 1. Flags

| Flag | Range | Named values | Engine setter |
|------|-------|--------------|---------------|
| `--scale` | 0–5 | `dorian lydian phrygian locrian harmminor mixolydian` | `gen_set_scale` |
| `--bar-ms` | 760–7600 | — | `gen_set_bar_ms` (ms-per-bar; ≡ samples-per-substep at 48 kHz) |
| `--gate` | 32–255 | — | `gen_set_gate` |
| `--mod-depth` | 100–8000 | — | `voice_set_mod_depth` |
| `--cutoff` | 30–180 | — | `voice_set_cutoff` (compile-time default 200 is above this range; omit the flag to keep it) |
| `--resonance` | 0–180 | — | `voice_set_resonance` |
| `--lfo-depth` | 0–255 | — | `voice_set_lfo_filter_depth` |
| `--filter-mode` | 0–3 | `lp hp bp notch` | `voice_set_filter_mode` |
| `--reverb` | 0–256 | — | `reverb_set_wet` |
| `--delay` | 0–256 | — | `delay_set_wet` |
| `--feedback` | 0–200 | — | `delay_set_feedback` |
| `--comp-threshold` | 8000–30000 | — | `compressor_set_threshold` |

All ranges mirror the live-key adjusters' clamps. Flags are position-
independent (pre-scan), work in live and render modes, and apply after
engine init, before the first sample. Setters consume no PRNG draws.

## 2. Error messages (exact strings, exit 1, stderr, stdout empty)

| Condition | Message |
|-----------|---------|
| numeric flag out of range / malformed | `--gate: expected integer 32..255, got "999"` (pattern: `<flag>: expected integer <min>..<max>, got "<val>"`) |
| bad scale value | `--scale: expected dorian\|lydian\|phrygian\|locrian\|harmminor\|mixolydian or 0..5, got "<val>"` |
| bad filter-mode value | `--filter-mode: expected lp\|hp\|bp\|notch or 0..3, got "<val>"` |
| missing value | `<flag>: missing argument` |

## 3. Resume line (exact format)

```
resume with: --seed <raw-seed>[ --scale <name>][ --bar-ms <n>][ --gate <n>][ --mod-depth <n>][ --cutoff <n>][ --resonance <n>][ --lfo-depth <n>][ --filter-mode <name>][ --reverb <n>][ --delay <n>][ --feedback <n>][ --comp-threshold <n>]
```

- One line, stderr, trailing newline. Fixed parameter order (table order
  above). Named values printed as names.
- `<raw-seed>` is the pre-hash input seed (explicit `--seed` or the
  clock-derived fallback).
- A parameter appears iff the user set it (CLI flag or live key). Internal
  `mutate()` drift is never printed — `--seed` reproduces it from bar 0.
- Snapshot semantics: values as of the user's last action.
- Emission points: clean `q` quit; POSIX signal handler (SIGINT/SIGTERM/
  SIGHUP/SIGQUIT, async-signal-safe, works in `--no-ui`); render mode
  prints the seed-only form at render start.
- NOT captured (declared): MIDI CC tweaks; Windows hard console-kill in
  `--no-ui` mode.

## 4. Behavior guarantees

- **G1**: Explicit-default flags (any subset, `--cutoff` excepted per its
  range) are byte-inert: output identical to the flagless run.
- **G2**: (seed, flags) fully determine output — flagged renders are
  byte-reproducible.
- **G3**: Recall (`paste the resume line`) reproduces the run from bar 0;
  mid-session tweak timing is not replayed (see spec FR-120 for the
  scale-change caveat).
- **G4**: Goldens and all pre-existing flag surfaces are untouched by the
  absence of these flags.
