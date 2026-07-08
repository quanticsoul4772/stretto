# MIDI Smoke Runbook (bench validation)

**Audience**: developers + QA bench-testing the `--midi` live surface
on real hardware. Complements `specs/003-midi-input/quickstart.md` (CLI
reference) and `tests/test_smoke_live.sh` (CI loopback smoke) with
**operator-driven** steps and **observable pass/fail signals**.

| Field | Value |
|---|---|
| Capability | [specs/003-midi-input/spec.md](../specs/003-midi-input/spec.md) |
| Architecture | [ARCHITECTURE.md §Live audio backends](../ARCHITECTURE.md#live-audio-backends) |
| CLI surface | `--midi [N]` · `--midi-default` · `--midi-list-devices` · `--midi-channel N` · `--no-midi` |
| Module | `audio_midi.c` (ring buffer + drain + CC dispatch + opt-out); platform backends `audio_midi_linux.c` (ALSA) and `audio_midi_winmm.c` (Win32 midiInProc) |
| Compliance | FR-004 (channel filter in audio thread), FR-033 (audio thread is sole consumer), FR-034 + Clarifications 2026-07-06 Q3 (no auto-reconnect on USB unplug) |
| Exit envelope | `--midi`/`--no-midi`/default live run exits 0 from main.c; SIGTERM = 143 (matches `tests/test_smoke_live.sh` PASS contract) |

## 0. Build prerequisites

```bash
# Linux (Ubuntu/Debian-flavored)
sudo apt install libasound2-dev alsa-utils         # libasound2 headers + amidi/aconnect
make && ./synth --midi-list-devices                # build + probe (expect your USB device listed)

# Windows — MinGW cross-compile from Linux (no extra deps; winmm is core Windows)
make win && ./stretto.exe --midi-list-devices

# macOS — installs ALSA-on-CoreMIDI shim used by this project
brew install alsa-lib                              # provides amidi + aconnect for the macOS ALSA backend
make && ./synth --midi-list-devices
```

> **Note on macOS**: `audio_midi.c` builds the Linux backend on
> `__APPLE__` (`#if defined(__linux__) || defined(__APPLE__)`) so it
> consumes the ALSA-on-CoreMIDI shim from `brew install alsa-lib`.
> `amidi`, `aconnect`, and the `hw:X,Y` port convention therefore work
> verbatim on macOS. The platform-idiomatic path (Audio MIDI Setup +
> IAC) also works because the ALSA backend adapts to CoreMIDI
> transparently.

## 1. Common preflight (all platforms)

| Step | Command | Pass signal |
|---|---|---|
| 1.1 Confirm binary exists | `ls -la synth` (Linux/macOS) / `dir stretto.exe` (Windows) | file present, executable |
| 1.2 Probe devices | `./synth --midi-list-devices` | lines of `index name` printed; your USB MIDI keyboard appears |
| 1.3 No-MIDI regression | `make test` | 16-s golden hash matches `golden/regression_16s.sha256` (FR-050 / FR-053) |
| 1.4 Unit tests | `make test-unit` | all `tests/unit/test_*.c` (incl. `test_midi`) pass |
| 1.5 Loopback smoke (Linux only) | `make test-smoke` | new `[midi loopback] Note On → audio` line passes within 1 s |

**Skipping 1.3 / 1.5 on platforms where they don't apply** is fine — they
are CI-gated, not bench-gated. The bench validates **operator-visible
behavior**, not the regression contract.

## 2. Linux

### 2.1 With real USB MIDI hardware (preferred for bench)

```bash
# 1. Identify your device's sequencer address.
aconnect -l                                 # list ALSA sequencer clients + ports
# Look for "client 128: 'USB MIDI Controller'" (or similar).
# Capture means 0:0 (0-based) corresponds to hw:1,0 in amidi shorthand.

# 2. Watch raw MIDI bytes while playing the keyboard (independent probe).
amidi -d -p hw:1,0 &                        # & bg so you can keep typing
# Press a key — hex like `90 3C 7F` (Note On ch 1, middle C, vel 127)
# means your keyboard → ALSA path is working.

# 3. Start the synth with MIDI, headless (no oscilloscope).
./synth --midi --midi-channel 1 --no-ui &
SYNTH_PID=$!
sleep 1                                     # give the pthread worker a frame to subscribe

# 4. Send a scripted Note On via amidi (-s = send hex bytes).
amidi -p hw:1,0 -s "90 3C 7F"               # middle C velocity 127
# Listen to the audio device (headphones / PulseAudio monitor) — expect
# the melody voice to acknowledge a pitch around C4 within ~21 ms
# (one render_chunk, per FR-033 drain cadence).

# 5. Send a Note Off so the envelope enters release cleanly.
amidi -p hw:1,0 -s "80 3C 00"

# 6. Send a CC sweep across CC#1 (mod wheel, → cutoff) to exercise
# the dispatch + scale formula. V=0..127 → delta = (V-64)*1 → cutoff -64..+63.
amidi -p hw:1,0 -s "B0 01 00"               # CC#1 = 0   (Δ = -64)
amidi -p hw:1,0 -s "B0 01 7F"               # CC#1 = 127 (Δ = +63)
# Listen for the filter sweep.

# 7. Tear down.
kill -TERM $SYNTH_PID
wait $SYNTH_PID 2>/dev/null                  # exit code should be 143 (SIGTERM)
echo "EXIT=$?"
```

**Pass signals**:
- audio responds to Note On within ~21 ms
- audio releases within ~600 ms of the matching Note Off
- filter cutoff audibly tracks the CC#1 sweep
- exit code `140..150` (typically 143 from SIGTERM)

### 2.2 Without hardware: `snd-seq-dummy` loopback

```bash
# 1. Load the kernel module (root).
sudo modprobe snd-seq-dummy
# After this, `aconnect -l` should show client 130: "snd-dummy" or "Midi Through Port-0".

# 2. Subscribe aconnect — link the kernel-provided output to the synth's input.
# (The synth announces its ALSA sequencer port as "stretto" at startup; the name
#  comes from audio_midi_linux.c port-create step. If invisible, skip — most
#  synth builds expose the ring buffer directly via ./synth --midi and accept
#  events from any sender without explicit subscribe routing.)
aconnect -l                                  # confirm "snd-dummy" client exists
./synth --midi --no-ui &
SYNTH_PID=$!

# 3. Find the dummy client's sequencer address. The `hw:` prefix in
#    `amidi -p hw:X,Y` is for hardware card addresses and does NOT
#    apply to virtual sequencer clients (kernel-exposed clients use
#    client:port form directly).
aconnect -l | grep -i dummy                  # usually client 130 port 0
# 4. Substitute the actual address (e.g. "130:0") in the next command.
amidi -p 130:0 -s "90 3C 7F"                 # Note On middle C velocity 127

kill -TERM $SYNTH_PID
wait $SYNTH_PID 2>/dev/null
```

**Auto-skip if `modprobe` fails** (no `sudo`, no kernel module support):
this is the same auto-skip pattern used by `tests/test_smoke_live.sh`.
The bench run is informational, not gating.

### 2.3 Channel filter check

```bash
./synth --midi --midi-channel 1 --no-ui &
SYNTH_PID=$!
sleep 1
amidi -p hw:1,0 -s "90 3C 7F"        # CH 1 → accepted → audio note
amidi -p hw:1,0 -s "91 3C 7F"        # CH 2 → silently dropped (FR-004, drain-side)
# (The second note should produce NO audible response even though the
#  keyboard on channel 2 fires correctly.)
kill -TERM $SYNTH_PID
```

## 3. Windows

### 3.1 Install loopMIDI + MIDI-OX (one-time)

1. **loopMIDI** — Tobias Erichsen's virtual loopback: <https://www.tobias-erichsen.de/software/loopmidi.html>
   - Install, launch, click "+" to add a new port (e.g. `stretto-in`).
2. **MIDI-OX** — music-class software's MIDI monitor: <http://www.midiox.com/>
   - Install; use it as the sender for the bench (Option → MIDI Devices → `stretto-in`).

### 3.2 Run the bench

```cmd
REM 1. List devices.
stretto.exe --midi-list-devices
REM Expect lines like:
REM   0 stretto-in
REM   1 Microsoft GS Wavetable Synth

REM 2. Start the synth (headless) on the loopMIDI port.
stretto.exe --midi 0 --no-ui

REM 3. In MIDI-OX, pick Options > MIDI Devices > MIDI Input = stretto-in.
REM    Click a Note On in the Input/Stamp pane (or use Actions >
REM    SysEx / Notes). Middle C velocity 127 fires — expect audio
REM    from `stretto.exe`'s PulseAudio / waveOut output within ~21 ms.

REM 4. For CC, MIDI-OX > Actions > Controller... pick CC 1 (Mod Wheel)
REM    and slide from 0 to 127. Expect a real-time filter sweep.

REM 5. Stop with Ctrl-Break (sends SIGTERM-equivalent on Windows; the
REM    process exits 0 if clean, otherwise Windows terminates with the
REM    shell's last-error code -- 0xC000013A STATUS_CONTROL_C_EXIT for
REM    Ctrl-C; the bench treats 0 + any normal Windows signal exit as
REM    PASS, matching the convention in tests/test_smoke_live.sh).
```


## 4. macOS

### 4.1 Platform-idiomatic: Audio MIDI Setup IAC bus

1. **Audio MIDI Setup** → Window → Show MIDI Studio.
2. Double-click **IAC Driver** → check "Device is online" → add a port named `stretto-in`.
3. Probe: `./synth --midi-list-devices` — confirm `stretto-in` is listed.
4. Launch: `./synth --midi --no-ui &`.
5. Send: any Mac tool that emits MIDI to the IAC bus (e.g. **MIDI Monitor** by GlenDark · "MIDI-OX-for-Mac"-class apps · or GarageBand's on-screen keyboard routed to the IAC). The synth consumes from the IAC bridge natively because the ALSA backend adapts to CoreMIDI.
6. Stop: `kill -TERM $PPID` — expect exit code 143.

### 4.2 Cross-platform shortcut: brew `alsa-lib` (recommended for bench)

```bash
brew install alsa-lib
./synth --midi-list-devices                 # works via the ALSA-on-CoreMIDI shim
./synth --midi --midi-channel 1 --no-ui &
SYNTH_PID=$!

# amidi + aconnect now work identically to Linux.
# Identify your IAC port:
aconnect -l | grep -i stretch                 # expect a "stretto" sequencer client
# Or plug a USB MIDI keyboard and look for its vendor tag.

# Send a Note On (the ALSA shim routes to CoreMIDI under the hood).
amidi -p hw:0,0 -s "90 3C 7F"
amidi -p hw:0,0 -s "80 3C 00"

kill -TERM $SYNTH_PID
wait $SYNTH_PID 2>/dev/null
```

> **Why route through brew alsa-lib**: it gives identical script
> surface across Linux + macOS for bench automation (no per-platform
> branch in CI scripts), and the synth's `__APPLE__` build branch
> consumes libasound from brew exactly the way it consumes the system
> libasound on Linux.

## 5. Graceful disconnect (FR-034 + Q3 — no auto-reconnect)

The synth must continue playing audio after a mid-session cable
unplug, must NOT crash, must NOT reset CC-modulated parameters,
and must exit cleanly on SIGTERM with `q.tail == q.head`

(empty ring buffer) and `g_drops == 0`.

### 5.1 Recipe (Linux, USB keyboard unplug)

```bash
./synth --midi --no-ui &
SYNTH_PID=$!
sleep 1

# Warm up: send a few notes and a CC to confirm the path is active.
amidi -p hw:1,0 -s "B0 01 40"               # CC#1 = 64  (Δ = 0; baseline cutoff)
amidi -p hw:1,0 -s "B0 01 7F"               # CC#1 = 127 (Δ = +63; cutoff opens)
amidi -p hw:1,0 -s "90 3C 7F"               # Note On C4
amidi -p hw:1,0 -s "80 3C 00"               # Note Off C4
sleep 0.5

# *** PHYSICALLY UNPLUG the USB MIDI cable ***
# The ALSA backend's snd_seq_event_input loop sees
# SND_SEQ_EVENT_PORT_EXIT for the subscribed source and calls
# audio_midi_linux_close() per T024 + FR-034.
# The audio thread's audio_midi_drain() continues to no-op on an
# empty queue — synth keeps playing from internal generative state
# (the melody still walks the L-system, drums still fire, etc.).
# CC-modulated parameters (cutoff in this case) RETAIN LAST VALUE
# per FR-034 — no jarring reset.

# Verify: the audio keeps coming. Wait 2 more seconds.
sleep 2

# Now exit cleanly.
kill -TERM $SYNTH_PID
wait $SYNTH_PID 2>/dev/null
RC=$?
echo "exit code = $RC (expect 143)"
```

**Pass signals for the disconnect test**:
- exit code 143 (SIGTERM) — no segfault
- audio continues for the full 2 s window between unplug and SIGTERM
- cutoff (the CC#1-driven parameter) does NOT reset to default
- `audio_midi_drain()` early-returns on `q.tail == q.head == head`

### 5.2 Recipe (Windows, MIDI-OX "Stop" toggle)

1. Start `stretto.exe --midi 0 --no-ui`.
2. In MIDI-OX × Input/Output → MIDI Devices → set Output = stretto-in. Send a Note On + a CC#1 sweep to warm up.
3. In MIDI-OX: **stop** the Output to `stretto-in` (right-click → Stop). The loopMIDI port stays open but `midiInProc` on the synth receives a `MM_MIM_CLOSE` for that input and `audio_midi_winmm_close()` fires per T024.
4. Audio continues (no reset on closed input, FR-034).
5. `Ctrl-Break` the synth — expected exit code 0 / 143 / `STATUS_CONTROL_C_EXIT` (0xC000013A).

### 5.3 Recipe (macOS, unplug USB)

Same as 5.1 using `amidi`/`aconnect` from `brew install alsa-lib`. Audio MIDI Setup's IAC bus cannot be "unplugged" so this branch validates the USB keyboard case; the audio-disconnect path through CoreMIDI is exercised by `SND_SEQ_EVENT_PORT_EXIT` semantics inside the shim.

### 5.4 Failure modes

| Mode | Symptom | Root cause | Fix |
|---|---|---|---|
| Segfault on close | exit code 139 | producer thread torn-down race — audio thread still draining | ensure `audio_midi_close()` sets `g_enabled = 0` BEFORE the backend-specific close (already enforced in audio_midi.c) |
| CC parameters jump to default on unplug | reset to baseline | FR-034 violation: the dispatch path's CC-modulated globals should retain last value across disconnect | profile `voice_get_cutoff()` etc. before / after the unplug; values should be bit-identical |
| Audio thread never drains tail == head | `q.tail` advances monotonically while `q.head` doesn't | producer race on out-of-order release pairs or audio-thread starvation (rare — render_chunk blocked >1024 frames) | rerun under `make debug` + `gdb -p $SYNTH_PID` with a one-shot `break audio_midi_drain`; compare `q.head` advance against the producer's input rate. If the bug reproduces a small number of times, add a unit test in `test_midi.c`; otherwise file as a deep-engine issue and rely on `make test-unit` for the rest |

## 6. Post-drain queue invariants (FR-033)

Every `audio_midi_drain()` call (issued at the top of `render_chunk`,
~every 21 ms at 48 kHz × 1024-frame chunks) MUST leave the ring buffer
in one of two steady states:

| State | `q.head` | `q.tail` | Means |
|---|---|---|---|
| **Idle (most chunks)** | == `q.tail` | 0 | no events queued between chunks; `local_head == tail` on entry, loop never enters |
| **Drain-just-completed** | > `q.tail` pre-call, equalized post-call | `local_head` (acquired at entry) | exactly the events that were enqueued since last drain; `__atomic_store_n(&q.tail, local_head, REL)` after the loop |

**Bench validation** (operator-visible; no instrumentation required):

At normal MIDI activity (≤50 events/sec), `q.tail == q.head` after every
drain. Audible signals to monitor:
- Each Note On produces a voice within ~21 ms (one render_chunk).
- No audio dropouts under sustained key-mashing at a comfortable rate.
- Overflow would require a sustained >12 kHz event rate (router-class
  hardware like the MIDI Fighter with macro-burst modes), which is
  not a normal bench case.

For per-event ground truth, the unit tests `T031` (channel filter) +
`T033` (Note On/Off live dispatch) exercise the drain at high rates;
`make test-unit` is the strict invariant check. Manual gdb on the
runbook is fragile (the file-static `q.head`/`q.tail` need the
namespace-qualified `'audio_midi.c'::q.head` form with a debug build,
and the breakpoint setup is operationally awkward at the bench). The
canonical `audio_midi_drop_count()` accessor is callable from gdb directly:
`gdb -p $SYNTH_PID -batch -ex "p audio_midi_drop_count()"` returns
the monotonic overflow counter without attaching to audio_midi_drain.

**Normal-load invariant**: at typical MIDI activity (≤50 events/sec),
`q.tail == q.head` after every drain (`g_drops == 0`). 256-entry ring ×
~48 Hz drain rate = comfortable headroom for rapid key-mashing.

**Overflow signal**: `audio_midi_drop_count() > 0` means events were
silently dropped (per FR-040 + Principle VII: no `fprintf` from
callback). Benign on burst-mash but worth flagging if it recurs on
benign input. The drop counter is monotonic; pass `--no-ui` builds can
read it via additional instrumentation (see ARCHITECTURE.md Testing).

## 7. CC mapping verification matrix

| CC# | MIDI 1.0 name | Synth destination | Δ formula | Expected audible change |
|---|---|---|---|---|
| 1   | Mod Wheel           | `voice_adjust_cutoff(Δ)`     | `(V-64)*1`  | ±0..±63 cutoff; filter sweep |
| 7   | Channel Volume      | `compressor_adjust_threshold(Δ)` | `(V-64)*60` | ±0..±3780 around 20000 default; quiet/loud sections balance shifts |
| 71  | Resonance / Timbre  | `voice_adjust_resonance(Δ)`  | `(V-64)*1`  | ±0..±63 resonance; ringing |
| 74  | Brightness / Cutoff | `voice_adjust_cutoff(Δ)`     | `(V-64)*1`  | sums on top of CC#1 (FR-022) |
| 91  | Reverb Send         | `reverb_adjust_wet(Δ)`       | `(V-64)*1`  | spatial wetness |
| 93  | Chorus / Delay Send | `delay_adjust_wet(Δ)`        | `(V-64)*1`  | echo density |
| 0, 10, 16, 17, 19, 64, 123 | (various / unassigned) | none | n/a | silent per Principle VII |

Compare against the unit tests in `tests/unit/test_midi.c`:
- `T025` — the full CC routing matrix from the table above (6 routed
  + 4 `CC_TARGET_NONE` sub-tests; exact sub-letter IDs are in
  `test_midi.c`'s source comments).
- `T031` — channel filter with `cc#1` on/off-channel events.
- `T033` — Note On / Note Off live dispatch (the V>0 and V=0 arms).

If a bench CC sweep contradicts the unit test, the unit test is the
ground truth — file a bug against the bench or the dispatch
implementation, not against CC_MAP (which is spec-frozen).

## 8. Exit-code contract reference

| Cause | Exit code | Bench classification |
|---|---|---|
| `audio_play()` returns 0 (clean return) | 0 | PASS |
| `Ctrl-C` / SIGINT from terminal | 130 | PASS (operator TTY only — CI is stricter) |
| `kill -TERM` / process kill | 143 | PASS |
| `timeout` expired | 124 | PASS (matches `test_smoke_live.sh` PASS envelope) |
| Segfault | 139 (128 + SIGSEGV) | FAIL — reproductions welcome |
| Bus error | 138 | FAIL |
| `--render` bad args | 1 | FAIL (cfg error, not audio) |
| Seed parsing error (`--seed abc`) | 1 | FAIL (cfg error, not audio) |

The strict PASS set (`0 / 124 / 143`) mirrors `tests/test_smoke_live.sh`.
The "operator TTY" row (130/SIGINT) is included here for bench runs but
NOT in the CI envelope — flag 130 in CI as a separate exit (the test
schema was written for unattended timeouts, not for typed Ctrl-C). Anything
outside `{0, 124, 130, 143}` is actionable; report with the failing seq
from argv + the last 20 lines of stderr.

## 9. Cross-references

- **CLI** — `specs/003-midi-input/quickstart.md §1, §3`
- **CC routing** — `audio_midi.c:CC_MAP[128]` (static const table) + `tests/unit/test_midi.c:T025a-k`
- **Disconnect handling** — `specs/003-midi-input/spec.md` FR-034 + tasks.md T024
- **Queue model** — `specs/003-midi-input/data-model.md` Entity 3 (ring buffer shape)
- **Concurrency contract** — `audio_midi.c` file header §Concurrency
- **Opt-out preservation** — `specs/003-midi-input/spec.md` FR-050 / FR-053 + Constitution III v1.0.1
- **In-automation smoke** — `tests/test_smoke_live.sh` virtual loopback step (Linux only; auto-skip if no `snd-seq-dummy`)
