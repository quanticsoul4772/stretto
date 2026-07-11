# CLI Contract: 003-midi-input (MIDI Input)

**Date**: 2026-07-06 · **Amended**: 2026-07-08 · **Branch**: `003-midi-input` · **Spec**: [spec.md](./spec.md) · **Plan**: [plan.md](./plan.md)

This document is the **executable contract** for the new MIDI input CLI surface. The implementation (`main.c` argv parsing + `audio_midi.c` / platform backends) MUST conform to this grammar; the implementation tests (`tests/unit/test_midi.c`, `tests/test_smoke_live.sh`) MUST assert against these exact strings + exit codes.

> **Amendment 2026-07-08**: reconciled to the as-built surface (PRs #108/#109) and to spec FR-002. Changes: `--midi` with no index is a **wildcard** subscribe (was: alias for index 0); list output format is `<index> <name>` (was: `<index>: <name>`); zero-device list prints an stderr notice with empty stdout (was: a stdout sentinel line); failed startup open now **exits 1** per FR-002 (previously implemented as continue-without-MIDI, which conflated FR-034's mid-session-disconnect semantics with startup); `--midi-list-devices` short-circuits instead of being a mutual-exclusion error; `--midi-channel` requires an open flag and rejects 0 (omit the flag for all channels, per FR-004's 1..16 range); `--render` never opens MIDI (Constitution III — `render_chunk` is the queue consumer, so an open device could inject events into a seeded render).

---

## 1. New CLI flags

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--midi [N]` | optional int | wildcard if N absent | Open MIDI input device at index N (from `--midi-list-devices` output; ALSA `(client<<8)\|port` encoding on Linux, 0-based ordinal on Windows; N=0 = first listed device on both). Omitting N subscribes to every readable input port (ALSA `CAP_READ` + `MIDI_GENERIC` enumerate walk); on WinMM the wildcard opens the first device — Win32 has no MIDI *input* mapper, so one handle = one device. |
| `--no-midi` | flag | (default behavior) | Explicit no-MIDI; same as omitting `--midi`. Exists for symmetry with `--no-ui`. Takes precedence over `--midi*` open flags if both are present. |
| `--midi-default` | flag | (alias) | Alias for `--midi 0` (first listed device; on Linux raw index 0 would decode to the System Timer port, so the backend remaps 0 to the first enumerated device). |
| `--midi-list-devices` | flag | n/a | List available MIDI input devices and exit 0. Short-circuits every other flag (including `--render`): the list is printed and the process exits before any audio path runs. |
| `--midi-channel N` | int | (absent = all channels) | Filter Note On/Off/CC to channel N (1..16, per FR-004). Omit the flag to accept all channels; N=0 is a usage error. Requires `--midi` or `--midi-default`. |

**Flag precedence** (no mutual-exclusion errors; the surface resolves conflicts deterministically):
- `--midi-list-devices` wins over everything: list, exit 0.
- `--no-midi` wins over `--midi` / `--midi-default` / `--midi N`.
- `--midi-channel` without `--midi` / `--midi-default` is a usage error (exit 1) — the filter has nothing to filter.

**Interaction with existing flags**:
- `--midi` + `--seed N` — works; MIDI input is layered on top of the deterministic PRNG
- `--midi` + `--no-ui` — works; MIDI input is layered on top of the headless live mode
- `--midi` + `--render` — MIDI is **never opened** in render mode: `render_chunk` drains the MIDI queue, so an open device could inject events into a seeded render (Constitution III). A stderr notice `MIDI: --midi is ignored in --render mode` is printed and the render proceeds normally, byte-identical to a no-MIDI run.
- `--midi` + positional `<seconds> <out.wav>` — same as the `--render` case

---

## 2. Argument grammar (EBNF)

```ebnf
program        := { flag } [ positional_render ] ;
flag           := "--no-ui" | "--seed" UINT
                |  "--midi" [ UINT ]
                |  "--no-midi"
                |  "--midi-default"
                |  "--midi-list-devices"
                |  "--midi-channel" UINT ;
positional_render := "--render" UINT ( PATH | "-" ) ;   (* "-" = WAV to stdout, added 2026-07-08 *)

UINT           := "0".."9"+ ;
PATH           := any non-flag string ;
```

The `main.c` argv pre-scan (existing pattern) strips `--seed N` and the `--midi*` flags in a single pass. `--midi-list-devices` short-circuits to the list-and-exit path after engine init (`gen_init` / `effects_init`) but before any audio path (`audio_play` / `render_wav`) runs. `--midi-channel N` accepts 1..16 only (FR-004); 0 and out-of-range values are usage errors.

---

## 3. Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success. Includes: `--midi-list-devices` printed and returned (even with zero devices); normal live-mode exit via 'q'; normal render-mode completion (including with ignored `--midi*` flags). |
| 0 | `--help` / `-h` / `--version` (added 2026-07-08): print to **stdout** and exit 0, taking precedence over **every** other flag including `--midi*` and `--render` — the GNU Coding Standards §4.8 "ignore other options and arguments" semantics. `--midi-list-devices --help` therefore prints help, not the device list. |
| 1 | Usage error: unknown flag, missing argument, out-of-range value, `--midi-channel` without an open flag. |
| 1 | MIDI startup error (FR-002): `--midi N` requested but no device at index N; `--midi` (wildcard) / `--midi-default` requested but zero devices enumerated or the backend open failed. |
| 1 | Render error: seconds out of range, output file unwritable, etc. (existing behavior). |

**Specific error messages** (exact strings, for test assertion):

| Condition | Message (to stderr) | Exit |
|-----------|---------------------|------|
| `--midi 99` but device index 99 absent | `MIDI: device index 99 unavailable (see --midi-list-devices)` | 1 |
| `--midi` (wildcard) or `--midi-default` with zero devices / backend open failure | `MIDI: no MIDI input devices found (see --midi-list-devices)` (wildcard) or `MIDI: device index 0 unavailable (see --midi-list-devices)` (default) | 1 |
| `--midi-channel 17` (out of range) | `--midi-channel: expected integer 1..16, got "17"` | 1 |
| `--midi-channel 0` | `--midi-channel: expected integer 1..16, got "0"` (all-channels = omit the flag) | 1 |
| `--midi-channel 5` without `--midi` / `--midi-default` | `--midi-channel: requires --midi or --midi-default` | 1 |
| `--midi-list-devices` with no devices (Linux: no ALSA clients; Windows: midiInGetNumDevs = 0) | stdout empty; stderr: `no MIDI input devices found` | 0 |
| `--midi*` open flag + `--render` | `MIDI: --midi is ignored in --render mode`; render proceeds normally | render path's |

**`--midi-list-devices` output format** (exact strings):

When at least one device is found (stdout):
```
<index 0> <name 0>
<index 1> <name 1>
...
```

`<index>` and `<name>` are separated by a single space (no colon). `<index>` is the value `--midi N` accepts: dense 0-based ordinals on Windows, the ALSA `(client<<8)|port` encoding on Linux (e.g. `3584 Midi Through Port-0` for client 14, port 0) — Linux indexes are therefore NOT consecutive. When zero devices are found, stdout is **empty** (so `wc -l` on stdout equals the device count for scripts) and the one-line notice `no MIDI input devices found` goes to stderr. Capped at 32 devices (`MIDI_LIST_DEVICES_CAP`). `name` is the human-readable "client port" pair from the ALSA client/port info (Linux) or `midiInGetDevCaps.szPname` (Windows), truncated to 63 chars + NUL. Each line is one record. No header row. Trailing newline.

---

## 4. Behavior guarantees

- **G1**: When `--no-midi` is set (explicit or implicit), the synth MUST NOT spawn a MIDI thread, MUST NOT call ALSA / winmm APIs, and MUST NOT allocate the `midi_queue_t` from the arena. (FR-005, FR-050; preserves the byte-identical baseline regression per FR-053.)
- **G2**: When `--midi N` is set and the device opens successfully, the synth MUST begin processing MIDI events within one `render_chunk` of the first event arriving (≤21 ms latency for Note On → audible output, per SC-001).
- **G3**: When the device disconnects mid-session (Q3 / FR-034), the synth MUST continue playing from internal generative state and MUST NOT crash, segfault, or exit. The status row (if `--no-ui` is not set) MAY display "MIDI: disconnected"; in `--no-ui` mode, the disconnect is silent.
- **G4**: When `--midi-channel N` is set (1..16), events on other channels MUST be silently dropped (no fprintf, no malloc). When `--midi-channel 0` (or omitted), all channels are accepted.
- **G5**: When the ring buffer is full (consumer falls behind), new events are silently dropped. The drop count is exposed via `audio_midi_drop_count()` (no UI display in v1).
- **G6**: In `--render` mode the synth MUST NOT open a MIDI device or enable the queue, regardless of `--midi*` flags. `render_chunk` is the queue consumer, so an open device could inject events into a seeded render; skipping the open keeps every seeded render a pure function of (`--seed`, argv) per Constitution Principle III.
- **G7**: A failed startup open (`audio_midi_open` != 0) MUST exit non-zero (FR-002) AND MUST reset `g_enabled = 0` before exiting, so no phantom-device state can exist even transiently. FR-034's continue-without-MIDI semantics apply ONLY to a mid-session disconnect of a successfully-opened device.

---

## 5. Test contract

The following cases are testable via `tests/unit/test_midi.c` (the cross-platform parts) and `tests/test_smoke_live.sh` (the platform-specific smoke):

| ID | Test |
|----|------|
| CT-1 | `--midi-list-devices` with ≥1 device outputs one line per device in the format `<index> <name>` (space-separated; ordinal on Windows, `(client<<8)\|port` on Linux); exits 0. |
| CT-2 | `--midi-list-devices` with 0 devices outputs nothing on stdout, the line `no MIDI input devices found` on stderr; exits 0. |
| CT-3 | `--midi 0` connects to device index 0; a Note On (programmatically enqueued) triggers a synth voice within one render_chunk. |
| CT-4 | `--midi 99` with only 3 devices prints `MIDI: device index 99 unavailable (see --midi-list-devices)` and exits 1. |
| CT-5 | `--midi-channel 17` prints `--midi-channel: expected integer 1..16, got "17"` and exits 1. |
| CT-6 | `--midi --midi-list-devices` short-circuits: prints the device list and exits 0 (no error). |
| CT-7 | `--midi-channel 5` without `--midi` prints `--midi-channel: requires --midi or --midi-default` and exits 1. |
| CT-8 | `--midi 5` (no `--midi-channel`) accepts events on all channels 1..16. |
| CT-9 | `--midi --midi-channel 1` accepts events on channel 1 and silently drops events on channels 2..16. |
| CT-10 | `--no-midi` (explicit) produces byte-identical output to a baseline run with no `--midi` (FR-050, FR-053). |
| CT-11 | No `--midi` (default) produces byte-identical output to a baseline run (FR-050, FR-053). |
| CT-12 | `--render 4 out.wav --midi --seed 0` prints `MIDI: --midi is ignored in --render mode`, never opens a device, and produces byte-identical output to `--render 4 out.wav --seed 0` (G6, Constitution III). |

Cases CT-8, CT-9, CT-10, CT-11 are covered by `tests/unit/test_midi.c` / `tests/test_bitexact.sh` and run in CI on every PR.
Cases CT-3 and the wildcard open path are exercised by `tests/test_smoke_live.sh` (snd-seq-dummy loopback) and auto-skip on CI runners without libasound / snd-seq-dummy.
Cases CT-1, CT-2, CT-4, CT-5, CT-6, CT-7, CT-12 assert `main()`-level argv behavior and are **not yet automated** — `main.c` is process-level (excluded from unit linking and coverage measurement per the Makefile `COV_SRCS_INTERACTIVE` rationale). They are the contract for manual verification and for a future fork+exec integration test; declared here rather than silently untested.
