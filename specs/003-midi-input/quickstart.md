# Quickstart: 003-midi-input (MIDI Input)

**Date**: 2026-07-06 · **Branch**: `003-midi-input` · **Spec**: [spec.md](./spec.md) · **Plan**: [plan.md](./plan.md)

This document is a hands-on guide for using and developing the MIDI input feature. It complements the [spec](./spec.md) (WHAT/WHY) and the [plan](./plan.md) (architecture) with concrete examples for end users and contributors.

---

## 1. Listener usage

### 1.1 List available MIDI input devices

```bash
./synth --midi-list-devices
```

Output (example, Linux):
```
0: USB MIDI Controller
1: Midi Through Port-0
2: snd-dummy MIDI
```

Output (example, Windows):
```
0: USB MIDI Controller
1: Microsoft GS Wavetable Synth
```

### 1.2 Connect to a specific device

```bash
./synth --midi              # wildcard: subscribe to every readable input port
./synth --midi 1            # device index 1 (from --midi-list-devices)
./synth --midi-default      # explicit device index 0
./synth --midi --no-ui      # headless mode (no oscilloscope, no help row)
```

### 1.3 Filter to a specific MIDI channel

```bash
./synth --midi --midi-channel 1   # only channel 1; ignore 2..16
./synth --midi                    # accept all channels (default; omit --midi-channel)
```

### 1.4 Combined with existing flags

```bash
./synth --midi --seed 42            # reproducible seed + MIDI input
./synth --midi --no-ui              # headless live mode with MIDI
./synth --render 30 out.wav --midi  # render path: MIDI is never opened (stderr notice;
                                    # seeded renders stay byte-identical per Constitution III)
```

### 1.5 Capture / cycle motif replay

The existing `t` key cycles the captured motif (the most recent triggered note). With MIDI input, the captured motif updates automatically as you play — the last MIDI Note On becomes the next motif step.

---

## 2. CC mapping (which knob does what)

| CC# | Name | Mapped to | Behavior |
|---|---|---|---|
| **1** | Mod Wheel | per-voice filter cutoff | additive ±20 units |
| **7** | Channel Volume | master compressor threshold | additive ±4000 units (around default 20000) |
| **71** | Resonance / Timbre | per-voice filter resonance | additive ±20 units |
| **74** | Brightness / Cutoff | per-voice filter cutoff | additive ±20 units (sums with CC#1) |
| **91** | Reverb Send | master reverb wet mix | additive ±32 units |
| **93** | Chorus / Delay Send | master delay wet mix | additive ±32 units |
| 16, 17, 19 | (GP1-4) | unassigned | ignored |
| **123** | All Notes Off | releases the channel's notes (pedal-held survive until pedal-up) | value-independent |
| (any other) | (unassigned) | unassigned | ignored |

The mapping is intentionally hard-coded in v1 (per Constitution Principle VII — no partial features). A future spec may add user-configurable mappings (e.g., a `~/.stretto/midi-map.conf`).

---

## 3. Key map (existing keystroke commands, unchanged)

The existing `keys.c` keyboard dispatcher is unaffected by `--midi`. Both input paths are active simultaneously (a Note On and a keypress both update synth state).

| Key | Command |
|---|---|
| `Space` | Force mutation |
| `+` / `-` | Tempo ±10 |
| `[` / `]` | FM mod depth -200 / +200 |
| `s` | Cycle scale |
| `g` / `G` | Gate length ±16 |
| `d` / `D` | Delay wet ±16 |
| `f` / `F` | Delay feedback ±16 |
| `r` / `R` | Reverb wet ±16 |
| `c` / `C` | Filter cutoff ±10 |
| `n` / `N` | Filter resonance ±10 |
| `m` / `M` | Filter LFO depth ±8 |
| `t` | Cycle filter mode (LP / HP / BP / notch) |
| `l` / `L` | Compressor threshold ±1000 |
| `?` | Toggle help row |
| `q` | Quit |

---

## 4. Build commands

Standard Stretto build flow, with the new `-lasound` linker flag:

```bash
# Linux build
make                                    # default target: synth (strips to ~24 KB)
make size                               # warn if past 24576 byte STRIP_TARGET

# Linux tests
make test                               # bit-exact 16-s SHA-256 regression (gates --no-midi byte-identity)
make test-unit                          # all per-module tests (incl. new test_midi)
make test-multiseed                     # 4-seed audio-characteristic bounds
make test-smoke                         # live-mode 2s + new virtual-MIDI loopback step

# Linux coverage
make coverage                           # per-file line coverage; new audio_midi.c must be ≥90%

# Windows cross-compile
make win                                # stretto.exe (already-linked winmm covers new midiIn* needs)
make winpack                            # UPX-pack; must stay ≤48 KB

# Debug build
make debug                              # synth_debug (-O0 -g -DDEBUG)
```

---

## 5. Platform notes

### 5.1 Linux

**Required runtime dependency**: `libasound2` (the shared library, not the headers). Preinstalled on `ubuntu-latest` GitHub Actions runners and on all mainstream desktop distros.

**Required build dependency**: `libasound2-dev` (headers + shared library). Install with:
```bash
sudo apt install libasound2-dev
```

**Static linking is not supported** — would blow the 24 KB binary budget by ~1 MB. The Makefile uses dynamic linking via `-lasound` (mirrors the existing `-lpulse` pattern).

**MIDI subsystem check**:
```bash
aconnect -l             # list ALSA sequencer clients/ports
amidi -d -p <client>:<port>  # dump MIDI input from a port (useful for debugging)
```

### 5.2 Windows

**Required runtime dependency**: none beyond the standard `winmm` (which is part of every Windows install). The MinGW cross-compile links `-lwinmm` (already in the Makefile for `waveOut*`).

**Device enumeration**: `midiInGetDevCaps()` is called at synth startup when `--midi-list-devices` is given. On modern Windows 10/11 this typically returns the Microsoft GS Wavetable Synth (index 0) and any class-compliant USB MIDI devices.

**MIDI subsystem check**:
- Settings → Sound → MIDI devices (Windows 10/11 Settings app)
- Or `mmdiag /m` (from Windows SDK)

### 5.3 MIDI loopback for the smoke test

The new smoke-test step on Linux uses `snd-seq-dummy` (a kernel module) + `amidi` (a userspace tool) to create a virtual MIDI loopback. Both are part of the standard `alsa-utils` and `linux-modules` packages.

**Setup**:
```bash
sudo modprobe snd-seq-dummy     # provides "snd-dummy MIDI" sequencer client
# snd-seq-dummy registers a client named "snd-dummy" with one MIDI port
```

The smoke test auto-skips this step if the module isn't loadable (e.g., on CI runners without `sudo` or without kernel module support).

---

## 6. Verifying the feature

After building, verify with:

```bash
# 1. Sanity: bit-exact regression still passes (the --no-midi path is byte-identical to baseline)
make test

# 2. New unit tests
make test-unit   # test_midi should appear in the output

# 3. New smoke test step (if libasound + snd-seq-dummy available)
make test-smoke  # look for the new "[midi loopback] Note On → audio" line

# 4. Manual: connect a USB MIDI controller and play
./synth --midi-list-devices   # confirm your device is at index 0 (or 1, etc.)
./synth --midi                # play!
```

If your device isn't auto-detected, check the OS-level MIDI subsystem (see Platform Notes above).

---

## 7. What this feature does NOT do (Out of Scope for v1)

- No MIDI output (clock out, note forwarding, Thru)
- No MPE / MIDI 2.0 / per-note expression
- No SysEx configuration
- No MIDI file playback
- No user-configurable CC mapping
- Pitch bend: +/-2 semitones, per channel (072/FR-015). No aftertouch or channel pressure
- No auto-reconnect on USB unplug/replug
- No `--midi-record` to capture output as a MIDI file

These are tracked in the spec's `## Out of Scope` section. A future spec may add them; the v1 codebase is structured (cleanly modular, Constitution V) to make additions localized.
