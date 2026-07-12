# Windows Live-Path Smoke Runbook (native validation)

**Audience**: developers validating `stretto.exe`'s **native Windows
live path** — waveOut audio, the real-console UI, keystroke handling,
and the winmm MIDI surface. Complements `tests/test_crossplatform.sh`
(render bit-exactness, which never exercises the live path) and
`scripts/midi-smoke.md` (MIDI bench validation).

First systematic run: 2026-07-11 (069 arc) on Windows 11 — all
automated checks passed; findings recorded per section below.

| Field | Value |
|---|---|
| Surface | waveOut live audio (`audio_winmm.c`), console UI (`ui.c` `_WIN32` half), winmm MIDI (`audio_midi_winmm.c`) |
| Automation | `scripts/windows_smoke.py` (this runbook's automated half; needs Python 3 + optionally `pip install pywinpty` for the real-console checks) |
| Manual-only | Ctrl-C-as-keystroke (§6) — pty layers cannot emulate keyboard Ctrl-C |

## ⚠ The spawn rule (learned twice, kept forever)

The synth is a **live audio** program: a leaked process is not silent
CI debris, it is sound in the operator's room. Every spawn in every
harness gets a hard deadline and cleanup that runs on every path:

```
taskkill //F //IM stretto.exe     # the unconditional cleanup (Git Bash)
taskkill /F /IM stretto.exe       # (cmd / PowerShell)
```

`scripts/windows_smoke.py` enforces this with per-phase timeouts and a
`finally:` taskkill. Do not write a synth-spawning check without one.

## 1. Build

```bash
# from WSL (mingw cross-compile; no Windows toolchain needed)
make win        # -> stretto.exe at the repo root, runs natively
```

## 2. Automated half

```bash
# from Git Bash on the Windows host, repo root:
python scripts/windows_smoke.py
```

Covers, each with observable pass/fail and a deadline:

| # | Check | Expectation |
|---|---|---|
| 1 | `--version` / `--help` | exit 0, `stretto <ver>` / `usage:` on stdout |
| 2 | Native-FS render determinism | two `--render 4 --seed 42` runs byte-identical; if a WSL `synth` sits in the repo root, its hash must match too (cross-platform bit-exactness, live re-check) |
| 3 | stdout-dash piping | `--render 2 - \| file` byte-identical to file render (Git Bash pipes are binary-safe; PowerShell < 7.4 is NOT — documented in README) |
| 4 | winmm MIDI, zero-input-device host | `--midi-list-devices` → contract message, exit 0; `--midi` / `--midi N` / `--midi-default` → exact contract errors, exit 1 |
| 5 | waveOut live smoke | `--no-ui` survives 6 s (device opened, no crash), then is terminated BY THE HARNESS (expected — TerminateProcess, no exit-code assertion) |
| 6 | Real-console UI (needs pywinpty) | ConPTY session: ANSI escapes + status row render (UI mode engaged, not degrade); `s` then `q` → clean exit 0; resume line contains `--seed` and `--scale lydian` |
| 7 | NO_COLOR (needs pywinpty) | ConPTY with NO_COLOR=1: status row renders with zero SGR sequences; `q` exits 0 |

## 3. Known CRT quirk (not a bug)

The headless-degrade notice ("stdin/stdout is not a terminal") IS
printed on Windows, but MS CRT **fully buffers redirected stderr**
(glibc never buffers stderr) — so a hard-killed process discards it.
Normal exits flush. If a redirected-stderr capture looks empty after a
kill, that is the buffering, not a missing notice.

## 4. ConPTY input limits (why §6 exists)

Writing bytes to a pseudo-console's input pipe is not pressing keys:
conhost's input layer decides what the client sees. Printable keys and
`q` pass through; a raw `0x03` does **not** arrive as a Ctrl-C
keystroke (verified 2026-07-11 — it is swallowed: neither a KEY_EVENT
nor a CTRL_C_EVENT reaches the app). Any automated "Ctrl-C test" via a
pty is therefore testing the pty, not the app.

## 5. What the 2026-07-11 run established

- Real-console UI, live keys, resume line, waveOut playback, all
  winmm MIDI failure paths, native render determinism, stdout piping:
  **all pass**; no code changes needed.
- The winmm wildcard/index/default error paths matched the
  specs/003 contract strings exactly on real Windows — the first
  in-the-wild exercise of the 059 `WAVE_MAPPER` removal.

## 6. Manual-only: Ctrl-C as keystroke

In a real Windows Terminal / conhost window:

```
.\stretto.exe
# wait for audio + oscilloscope, then press Ctrl-C
```

PASS: clean quit, terminal restored, `resume with: --seed N` printed,
no crash dialog. The handling is `ui.c`'s `ui_term_read_key` (0x03 →
`q`, reachable because raw mode clears `ENABLE_PROCESSED_INPUT`).
Status: **not yet human-verified** as of 2026-07-11; the code path is
present and the equivalent `q` path passes under ConPTY.
