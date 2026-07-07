# CLI Contract: 003-midi-input (MIDI Input)

**Date**: 2026-07-06 · **Branch**: `003-midi-input` · **Spec**: [spec.md](./spec.md) · **Plan**: [plan.md](./plan.md)

This document is the **executable contract** for the new MIDI input CLI surface. The implementation (`main.c` argv parsing + `audio_midi.c` / platform backends) MUST conform to this grammar; the implementation tests (`tests/unit/test_midi.c`, `tests/test_smoke_live.sh`) MUST assert against these exact strings + exit codes.

---

## 1. New CLI flags

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--midi [N]` | optional int | N=0 if absent | Open MIDI input device at index N (0-based). Omitting N is equivalent to `--midi 0`. |
| `--no-midi` | flag | (default behavior) | Explicit no-MIDI; same as omitting `--midi`. Exists for symmetry with `--no-ui`. |
| `--midi-default` | flag | (alias) | Alias for `--midi 0`. |
| `--midi-list-devices` | flag | n/a | List available MIDI input devices and exit 0. Mutually exclusive with all other MIDI flags and with `--render`. |
| `--midi-channel N` | int | 0 (all channels) | Filter Note On/Off/CC to channel N (1..16). N=0 (or omitting the flag) means accept all channels. |

**Mutually exclusive flags** (parse error if both present):
- `--midi` and `--no-midi` (and `--midi-default` is the same as `--midi 0`, so a conflict only arises if `--midi N` is given with N != 0)
- `--midi` and `--midi-list-devices`
- `--no-midi` and `--midi-list-devices`

**Interaction with existing flags** (no conflict):
- `--midi` + `--seed N` — works; MIDI input is layered on top of the deterministic PRNG
- `--midi` + `--no-ui` — works; MIDI input is layered on top of the headless live mode
- `--midi` + `--render` — works; `--render` uses the audio render path, `--midi` is ignored (the audio thread doesn't run; the MIDI thread + ring buffer have no consumer)
- `--midi` + positional `<seconds> <out.wav>` — works; same as `--render` case

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
positional_render := "--render" UINT PATH ;

UINT           := "0".."9"+ ;
PATH           := any non-flag string ;
```

The `main.c` argv pre-scan (existing pattern) strips `--seed N` and processes it. The MIDI flags are processed in a second scan in `main.c`. `--midi-list-devices` short-circuits to the list-and-exit path before `gen_init` / `audio_play` are called.

---

## 3. Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success. Includes: `--midi-list-devices` printed and returned; normal live-mode exit via 'q'; normal render-mode completion. |
| 1 | Usage error: unknown flag, missing argument, out-of-range value, mutually exclusive flags. |
| 1 | MIDI error: `--midi N` requested but no device at index N (or no MIDI subsystem available). |
| 1 | Render error: seconds out of range, output file unwritable, etc. (existing behavior). |

**Specific error messages** (exact strings, for test assertion):

| Condition | Message (to stderr) |
|-----------|---------------------|
| `--midi 99` but only 3 devices connected | `midi: device index 99 not found; run with --midi-list-devices to see available devices` |
| `--midi` but libasound not installed (Linux) | `midi: ALSA sequencer not available; install libasound2-dev or rebuild without --midi` |
| `--midi` but winmm init failed (Windows) | `midi: Win32 midiInOpen failed (code N)` |
| `--midi 0` but no MIDI devices connected | `midi: no MIDI input devices found; run with --midi-list-devices to see what is available` |
| `--midi-channel 17` (out of range) | `midi: --midi-channel must be in 0..16, got 17` (0 = all channels) |
| `--midi` + `--midi-list-devices` | `usage: --midi and --midi-list-devices are mutually exclusive` |
| `--midi-channel 5` without `--midi` | `usage: --midi-channel requires --midi` |
| `--midi-list-devices` with no devices (Linux: no ALSA clients; Windows: midiInGetNumDevs = 0) | stdout: `0 devices found` (one line); exit 0 |

**`--midi-list-devices` output format** (exact strings):

When at least one device is found:
```
0: <name 0>
1: <name 1>
...
N-1: <name N-1>
```

When zero devices are found:
```
0 devices found
```

`name` is the human-readable device name from `snd_seq_get_port_info` (Linux) or `midiInGetDevCaps.szPname` (Windows), truncated to 63 chars + NUL (so the line fits in 80 cols). Each line is one record. No header row. Trailing newline.

---

## 4. Behavior guarantees

- **G1**: When `--no-midi` is set (explicit or implicit), the synth MUST NOT spawn a MIDI thread, MUST NOT call ALSA / winmm APIs, and MUST NOT allocate the `midi_queue_t` from the arena. (FR-005, FR-050; preserves the byte-identical baseline regression per FR-053.)
- **G2**: When `--midi N` is set and the device opens successfully, the synth MUST begin processing MIDI events within one `render_chunk` of the first event arriving (≤21 ms latency for Note On → audible output, per SC-001).
- **G3**: When the device disconnects mid-session (Q3 / FR-034), the synth MUST continue playing from internal generative state and MUST NOT crash, segfault, or exit. The status row (if `--no-ui` is not set) MAY display "MIDI: disconnected"; in `--no-ui` mode, the disconnect is silent.
- **G4**: When `--midi-channel N` is set (1..16), events on other channels MUST be silently dropped (no fprintf, no malloc). When `--midi-channel 0` (or omitted), all channels are accepted.
- **G5**: When the ring buffer is full (consumer falls behind), new events are silently dropped. The drop count is exposed via `audio_midi_drop_count()` (no UI display in v1).

---

## 5. Test contract

The following cases are testable via `tests/unit/test_midi.c` (the cross-platform parts) and `tests/test_smoke_live.sh` (the platform-specific smoke):

| ID | Test |
|----|------|
| CT-1 | `--midi-list-devices` with ≥1 virtual device outputs N+1 lines in the format `0: <name>` ... `N-1: <name>`; exits 0. |
| CT-2 | `--midi-list-devices` with 0 devices outputs the single line `0 devices found`; exits 0. |
| CT-3 | `--midi 0` connects to device index 0; a Note On (programmatically enqueued) triggers a synth voice within one render_chunk. |
| CT-4 | `--midi 99` with only 3 devices prints the exact error message and exits 1. |
| CT-5 | `--midi-channel 17` prints the exact error message and exits 1. |
| CT-6 | `--midi --midi-list-devices` prints the exact "mutually exclusive" message and exits 1. |
| CT-7 | `--midi-channel 5` without `--midi` prints the exact "requires --midi" message and exits 1. |
| CT-8 | `--midi 5` (no `--midi-channel`) accepts events on all channels 1..16. |
| CT-9 | `--midi --midi-channel 1` accepts events on channel 1 and silently drops events on channels 2..16. |
| CT-10 | `--no-midi` (explicit) produces byte-identical output to a baseline run with no `--midi` (FR-050, FR-053). |
| CT-11 | No `--midi` (default) produces byte-identical output to a baseline run (FR-050, FR-053). |

Cases CT-1, CT-2, CT-4, CT-5, CT-6, CT-7, CT-10, CT-11 are in `tests/unit/test_midi.c` or `tests/test_bitexact.sh` (existing) and are run in CI on every PR.
Cases CT-3, CT-8, CT-9 are in `tests/test_smoke_live.sh` (the new loopback step) and are auto-skipped on CI runners without libasound / snd-seq-dummy.
