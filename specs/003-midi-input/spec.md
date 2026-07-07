# Feature Specification: MIDI Input

**Feature Branch**: `003-midi-input`

**Created**: 2026-07-06

**Last Updated**: 2026-07-06

**Status**: Draft

**Input**: User description: "(empty — see Assumptions for the informed guess)"

## Overview

Stretto's live mode is currently controlled by single-keystroke commands parsed from terminal stdin (`keys.c`). This spec proposes **MIDI input as an external control surface** that mirrors the existing `keys.c` keyboard handler with USB-MIDI devices (USB MIDI keyboards, controllers, foot pedals). Listener launches `synth --midi`, presses keys on a connected USB MIDI controller, and hears the synth respond according to the same generative state machine that drives keyboard input.

The proposal is the first natural extension of the live-mode input surface. It was inferred from the baseline spec's "Out of Scope" list which calls out "MIDI input or output" as a future capability. This feature covers the input side only (output / Thru / clock are out of scope; see "Out of Scope" below).

## Clarifications

### Session 2026-07-06

- Q1: How should MIDI keyboard input behave when multiple keys are held at the same time? → A: **Polyphonic, 11 voices with voice stealing**. Each Note On allocates a free voice from the existing 11-voice pool (`N_VOICES` in `voice.h:8`); when all 11 voices are busy, the **oldest voice currently in release (idle)** is stolen for the new note via the existing `voice_pool_trigger_role` slot-selection logic. If no voice is in release, the oldest voice (regardless of state) is stolen. The pool is treated as a single non-role-scoped pool for MIDI input (the role-scoping used by the generative engine is bypassed).
- Q2: Which threading model should the MIDI handler use? → A: **Callback-style dispatch — audio thread is the only consumer; platform-specific worker threads are the only producers.** Linux uses `pthread_create()` + a worker thread that loops on blocking `snd_seq_event_input()` and parses each event into a `midi_event_t` for the shared ring buffer (preflight correction 2026-07-06: replaced the earlier `snd_seq_create_thread()` reference, which is NOT a stock libasound API; `snd_seq_dispatch_set_callback` exists but requires a poll loop on the audio thread which would violate FR-033, so the standard pthread + blocking-input pattern is used instead). Windows uses `midiInProc` (a Win32 system-managed thread is created by `midiInStart()` automatically — no extra pthread). Both producers enqueue to the **same shared ring buffer**; the audio thread (existing in `render_chunk`) is the **only consumer** and reads the buffer once per `~21 ms` chunk via an atomic acquire-load. This keeps the producer/consumer pattern semantically identical across platforms (one producer, one consumer, SPSC inside the audio thread), and means the audio thread never blocks on platform-specific I/O. The added pthread on Linux adds a few hundred bytes to BSS (the pthread struct), well within the 24 KB Linux budget.
- Q3: What should happen when a MIDI device is unplugged mid-session and replugged? → A: **No auto-reconnect (matches the spec's existing edge case)**. On disconnect: the MIDI thread exits cleanly; the synth continues playing from internal generative state (existing voices finish their envelopes); CC-modulated parameters retain their last value. To use MIDI again, the user must restart the synth with `--midi`. If a *different* device appears at the same index, it is NOT auto-detected — restart required. Keeps v1 small per Constitution Principle VII.
- Q4: How should the synth initialize MIDI CC values at startup? → A: **Assume CC=0 until first message**. All CCs start at 0 at synth launch. The first CC message from the controller sets the actual value. A knob moved on the controller BEFORE launch is NOT reflected until the user wiggles it (the next CC message updates the synth). This is standard behavior for most software synths and avoids a controller-inquiry round-trip (which not all controllers support).

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Trigger notes from a USB MIDI keyboard (Priority: P1)

Listener launches `synth --midi`, presses a key on a connected USB MIDI controller, and hears the synth play a note that respects the active scale + chord. The note is voiced using the active section's selected chord voice type (wavetable in INTRO/RESOLVE, additive in BODY, FM in TENSION).

**Why this priority**: Most natural "external control" extension. The live keystroke handler already proves the routing; MIDI is the next input device.

**Independent Test**: Launch with `--midi`, press middle C on a USB MIDI controller, listen for audible note within one audio buffer (~21 ms).

**Acceptance Scenarios**:

1. **Given** the synth running with `--midi` and a USB MIDI controller connected, **When** the listener presses a single key, **Then** the synth triggers a note within one audio buffer (~21 ms) voiced in the active section's synthesis type.
2. **Given** the active scale is D Dorian and a key press arrives, **When** the synth computes the note, **Then** the resulting MIDI pitch is in D Dorian (the active scale, not raw MIDI semitones).
3. **Given** the listener releases the key, **When** the synth processes the Note Off, **Then** the corresponding voice's envelope enters release (no abrupt silence mid-sustain).
4. **Given** the synth running without `--midi`, **When** the listener connects a MIDI device, **Then** the synth runs unchanged (no MIDI thread, no polling overhead, no CPU cost).

### User Story 2 - Modulate live parameters via MIDI CC (Priority: P2)

Listener routes their MIDI controller's knobs/faders to the synth's existing live parameters (cutoff, resonance, reverb wet, delay wet, delay feedback, mutation rate, filter LFO depth) via standard CC numbers. The mapping is a small static table that ships in v1.

**Why this priority**: Common workflow for live performance with hardware controllers. Builds on US1's MIDI plumbing.

**Independent Test**: With the synth running, send CC#1 (mod wheel) from a controller and observe the filter cutoff updates in the status row within one buffer.

**Acceptance Scenarios**:

1. **Given** the synth running with `--midi` and a controller connected, **When** the listener sends CC#1, **Then** the per-voice filter cutoff updates by an amount proportional to the CC value (0..127), clamped to the user-tunable range.
2. **Given** the listener is modulating cutoff via CC#1, **When** the synth receives CC#74, **Then** the cutoff is also modulated additively. Multiple CCs that target the same parameter sum (with each CC's contribution scaled by its source channel attenuation).

### User Story 3 - List and select MIDI input devices (Priority: P3)

Listener runs `synth --midi-list-devices` to see a numbered list of available MIDI input devices. They then run `synth --midi` (default) or `synth --midi <n>` to bind to a specific device by index.

**Why this priority**: Diagnostic + multi-device environments. Default behavior (first available device) covers the typical case.

**Independent Test**: Run `--midi-list-devices` with at least one MIDI input connected; output lists the device names. Run `--midi` (no number); output reports which device was selected.

**Acceptance Scenarios**:

1. **Given** the synth running with `--midi-list-devices` and at least one MIDI input device, **When** the list is printed, **Then** each line shows the device index and human-readable name.
2. **Given** no MIDI input device connected, **When** the listener runs `--midi`, **Then** the synth exits with an error message explaining that no MIDI input was found and the user can run `--midi-list-devices` to see what is available.

### Edge Cases

- **MIDI device disconnects mid-session (per Clarifications 2026-07-06 Q3)**: the MIDI thread exits cleanly; the synth continues playing from internal state; CC-modulated parameters retain their last value. To use MIDI again, restart the synth with `--midi`. A different device at the same index is NOT auto-detected.
- **Rapid key mashing (>10 notes/sec)**: each Note On enqueues; voice pool steals oldest idle voice via the existing role-scoped slot selection in `voice_pool_trigger_role`.
- **MIDI clock overflow / jitter**: events are queued; consumer reads them in `render_chunk`; no audio glitch because the audio thread is never blocked on MIDI I/O.
- **Note On with velocity 0**: interpreted as Note Off per the MIDI 1.0 spec.
- **Note Off for an unmatched key**: no-op (no orphan release).
- **MIDI channel mismatch**: by default, all channels accepted; `--midi-channel N` filters to a single channel.

## Requirements *(mandatory)*

### Functional Requirements

#### MIDI input plumbing
- **FR-001**: System MUST accept a new `--midi [N]` CLI flag; when set, the synth opens MIDI input device index N (0-based) for read-only.
- **FR-002**: System MUST accept `--midi-default` (alias for `--midi 0`); when no MIDI device is connected, the synth exits with a non-zero code and a clear error message.
- **FR-003**: System MUST accept `--midi-list-devices`; in this mode the synth prints available input device names + indices and exits 0.
- **FR-004**: System MUST accept `--midi-channel N` (1..16); only Note On/Off/CC messages on channel N trigger the synth; default = accept all channels. **Channel filter placement** (M1 fix): the filter is applied in the **audio thread inside `audio_midi_drain()`**, BEFORE dispatch — i.e., before `voice_pool_trigger_midi`, `voice_pool_release_midi`, and `CC_MAP[C]` lookup. Non-matching-channel events are silently dropped at drain time with no `fprintf` and no `adjust_*` call. The producer-side platform callback (ALSA / midiInProc) enqueues every channel's events unconditionally because callback-time filtering would force a duplicate channel-state read in each platform's callback thread and would break the audio-thread-only consumer invariant (FR-033). Channel-filter precedence is fully modeled in `data-model.md` Entity 4 (M1 fix).
- **FR-005**: When `--midi` is not set, the synth MUST NOT poll for MIDI input and MUST NOT consume any CPU for MIDI I/O.
- **FR-006**: System MUST add a new `--no-midi` flag (default behavior, alias for omitting `--midi`); explicit form exists for symmetry with `--no-ui`.

#### MIDI → synth routing (Note On / Off)
- **FR-010**: A Note On message on the active channel with key K (0..127) and velocity V (0..127) MUST trigger a synth note:
  - **Polyphony model (per Clarifications 2026-07-06 Q1)**: the MIDI handler is **polyphonic with up to 11 voices and voice stealing** — each Note On allocates a free voice from the existing 11-voice pool (`N_VOICES` in `voice.h:8`); when all 11 voices are busy, the **oldest voice currently in release (idle)** is stolen for the new note via the existing `voice_pool_trigger_role` slot-selection logic. If no voice is in release, the oldest voice (regardless of state) is stolen. The pool is treated as a single non-role-scoped pool for MIDI input (the role-scoping used by the generative engine is bypassed).
  - MIDI pitch class K % 7 maps to a scale degree in the active scale (`SCALES[cur_scale][K % 7]`).
  - **Octave offset = `K / 7 - 5`, clamped to `[-2, +4]` octaves** (i.e., `[-24, +48]` semitones) — clamp keeps playable range within the existing voice pool's comfortable envelope (engine's `note_table.h` covers ~MIDI 24..108 semitones). Clamp range matches `data-model.md` Entity 5 step 4 (H2 fix).
  - Synth voice type = `VOICE_FM` (mirroring the live-mode melody handler's per-step FM/KS alternation; fixed at FM for MIDI since external triggers are not on the 16-step Euclidean grid).
  - Velocity V scales the synth voice's peak amplitude by `V / 127` (clamped to `[64, 32767]` to keep dynamics musical).
- **FR-011**: A Note On with velocity 0 MUST be interpreted as Note Off (per the MIDI spec) and trigger envelope release.
- **FR-012**: A Note Off message MUST identify the matching held voice by (key, channel) and trigger its envelope release.
- **FR-013**: If the matching voice is already in release, the Note Off MUST be a no-op (don't restart the release).
- **FR-014**: The velocity of the Note On that triggered the voice (not the Note Off) MUST remain effective through the voice's life (V/127 amplitude scale).

#### MIDI → synth routing (CC → live parameter)
- **FR-020**: A CC message on the active channel with controller C (0..127) and value V (0..127) MUST modulate a live parameter via a static mapping table:
  - **CC initial state (per Clarifications 2026-07-06 Q4)**: all CC values start at 0 at synth launch. The first CC message from the controller sets the actual value. A knob moved on the controller BEFORE launch is NOT reflected until the user wiggles it (the next CC message updates the synth).
  - **CC dispatch formula** (per `data-model.md` Entity 4, authoritative for exact magnitudes — H1 fix): for each CC message, the dispatch routine reads `CC_MAP[C]` from the static 128-entry table, computes `delta = (V - 64) * scale`, and applies the delta to the target via the existing `adjust_*` API (e.g., `voice_adjust_cutoff(delta)`, `voice_adjust_resonance(delta)`). `CC_TARGET_NONE` entries are silently dropped. Multiple CCs targeting the same parameter sum additively per FR-022 because each `adjust_*` call composes additively over the precedential call.
  - **Controller → target + scale bindings** (full table lives in `data-model.md` Entity 4; the additive swing per CC is bounded by `|63 * scale|` since `V ∈ [0, 127]` and the formula is centered on `64`):
    - CC#1 (mod wheel) → per-voice filter cutoff; `scale = +1`; max swing ±63 against the user-tunable base `[30, 180]`.
    - CC#7 (channel volume) → master bus compressor threshold; `scale = +60`; max swing ≈ ±3780 against the default threshold (effects.c composer state).
    - CC#71 (resonance / timbre) → per-voice filter resonance; `scale = +1`; max swing ±63 against the user-tunable base `[0, 180]`.
    - CC#74 (brightness / cutoff) → per-voice filter cutoff; `scale = +1`; max swing ±63; sums with CC#1 per FR-022.
    - CC#91 (reverb send) → master reverb wet mix; `scale = +1`; max swing ±63.
    - CC#93 (chorus or delay send) → master delay wet mix; `scale = +1`; max swing ±63.
  - **Unassigned CCs** (kept at `CC_TARGET_NONE` for v1 per Principle VII): CC#16 / CC#17 / CC#19 (General Purpose 1–4) and CC#123 (All Notes Off — the MIDI 1.0 standard all-notes-off message). All silently dropped with no `fprintf` and no callback overhead.
- **FR-021**: When a CC message arrives, the synth MUST apply the modulation in the next `render_chunk` (≤1 buffer, ~21 ms).
- **FR-022**: Multiple CCs targeting the same parameter MUST sum additively (capped at the parameter's documented bound).

#### Cross-platform
- **FR-030**: Linux implementation MUST use the ALSA sequencer API (`snd_seq_*`) to enumerate + open MIDI input. Input MUST be read via a single `pthread_create()` worker thread looping on blocking `snd_seq_event_input()` (preflight correction 2026-07-06: `snd_seq_create_thread()` is not a stock libasound API and was the original plan; the corrected function-pair is `pthread_create` + `snd_seq_event_input` working in tandem — see `research.md` D1 + `data-model.md` Relationships summary for the full pattern). The worker parses each `snd_seq_event_t` (structured event payloads; libasound provides `ev->type` and `ev->data.{note,control}.{channel,note/param,velocity/value}` directly) into a `midi_event_t` and calls `audio_midi_enqueue()` to write to the shared ring buffer (per Clarifications 2026-07-06 Q2). The audio thread MUST NOT call ALSA APIs directly; it reads the ring buffer at the start of every `render_chunk` via `__atomic_load_n` (per FR-033).
- **FR-031**: Windows implementation MUST use the Win32 MIDI API (`midiInOpen` / `midiInStart` / `midiInProc` callback). The `midiInProc` callback enqueues events to the same shared ring buffer used on Linux.
- **FR-032**: Both platforms MUST use the same internal `midi_event_t` representation (channel, type, key, value) AND the same producer-side enqueue pattern (platform callback → ring buffer), so the synth-side consumer (audio thread in `render_chunk`) is platform-independent.
- **FR-033**: The audio thread MUST drain the ring buffer at the start of each `render_chunk` call via an atomic-claim of the head pointer (single-consumer). The audio thread MUST NOT call ALSA APIs (Linux) or winmm APIs (Windows) directly; the audio thread is never blocked on platform-specific I/O.
- **FR-034**: When the MIDI device disconnects mid-session (per Clarifications 2026-07-06 Q3), the MIDI thread MUST exit cleanly; the synth MUST continue playing from internal generative state; CC-modulated parameters MUST retain their last value; the synth MUST NOT attempt to re-open the device automatically. To use MIDI again, the user MUST restart the synth with `--midi`. A different device that appears at the same index is NOT auto-detected.

#### Memory + budget
- **FR-040**: MIDI event queue MUST be a fixed-size ring buffer (256 events), allocated from the 128 KB static arena. No `malloc`.
- **FR-041**: Linux binary stays within the existing 24 KB stripped soft target (Makefile `size` warns past it). Windows packed binary stays within the existing 48 KB hard budget (Constitution Principle I).
- **FR-042**: MIDI thread / callback MUST NOT read `time()` (Constitution Principle III).

#### Determinism + tests
- **FR-050**: Given `--no-midi` (the default), the synth's audio output MUST be byte-identical to a run without `--midi`. (Per Constitution Principle III v1.0.1 — adding MIDI as a code path MUST NOT alter the no-MIDI path's bytes.)
- **FR-051**: System MUST have unit tests covering: scale-degree mapping math, velocity scaling, CC mapping, ring buffer enqueue/dequeue, channel filtering, no-MIDI path is byte-identical.
- **FR-052**: System MUST have a smoke test that opens a virtual MIDI loopback device (Linux: `snd_seq_dummy` or a virmidi kernel module) and verifies a Note On causes a non-zero audio buffer within 1 second.
- **FR-053**: The baseline bit-exact 16-second SHA-256 regression (`golden/regression_16s.sha256`) MUST continue to pass with `--no-midi` (the default).
- **FR-054**: New module's line coverage MUST meet the existing per-file gate: ≥90% for any new module under `tests/unit/`.

### Key Entities

- **MIDI event**: a single (channel, type, key/value) tuple. Type ∈ `{NOTE_ON, NOTE_OFF, CC}`. Channel ∈ [1, 16]. Key/value ∈ [0, 127]. Timestamps are platform-relative; not used for sequencing in v1.
- **MIDI input device**: a named endpoint (e.g. "USB MIDI Controller 0", "BSP 1") that yields a stream of MIDI events. On Linux: an ALSA sequencer client+port pair. On Windows: a Win32 MIDI input handle.
- **MIDI event queue**: a fixed-size ring buffer of `midi_event_t` written by the platform MIDI thread (or ALSA / Win32 callback), read by the audio thread inside `render_chunk`. Single-producer, single-consumer; size 256 (configurable later).
- **CC mapping table**: static `cc_map_t[128]` array (one slot per controller number) that routes a CC to a target live parameter and an applied scale + clamp.
- **Voice struct (MIDI extension)** (per `research.md` D4 decision — H3 fix): two new fields are added to the existing `Voice` struct in `voice.h`: `trigger_key` (`uint8_t`, the MIDI key 0..127 that started this voice; sentinel `0xFF` = not a MIDI voice) and `trigger_channel` (`uint8_t`, MIDI channel 1..16; sentinel `0` = not a MIDI voice, set by the generative engine path). These fields enable Note-Off matching (FR-012) by tagging each voice with the `(key, channel)` pair that triggered it. The two code paths (generative scheduling vs MIDI input) set distinct sentinel values so the structs are clearly distinguishable in code review without affecting audio. Full struct extension + dispatch semantics live in `data-model.md` Entity 5.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Pressing a single key on a connected USB MIDI controller while the synth runs with `--midi` produces an audible note within one audio buffer (~21 ms). **VALIDATED** by listening.
- **SC-002**: 4-seed 16-second renders with `--no-midi` produce byte-identical sha256 hashes to the current golden (i.e., the baseline regression still passes). **VALIDATED** by `make test`.
- **SC-003**: Windows packed binary stays ≤48 KB on every PR. **VALIDATED** by `make win && make winpack`.
- **SC-004**: ≥90% line coverage on any new module under `tests/unit/`. **VALIDATED** by `make coverage`.
- **SC-005**: ≥4 new tests in `tests/unit/test_midi.c` covering scale-degree mapping, velocity scaling, CC mapping, ring buffer enqueue/dequeue, channel filtering. **VALIDATED** by `make test-unit`.
- **SC-006**: Smoke test that opens a virtual MIDI loopback device and verifies a Note On produces non-zero audio in ≤1 second. **VALIDATED** by `make test-smoke` (with a new loopback setup step on Linux only).
- **SC-007**: Live keystroke handling continues to work when `--midi` is set (both code paths share `keys_dispatch`-style state updates). **VALIDATED** by `tests/unit/test_keys.c` continuing to pass.

## Assumptions

- `$ARGUMENTS` to `/speckit-specify` was empty, so the feature description was inferred from the recent conversation and the baseline spec's "Out of Scope" list. The proposal is **MIDI input as an external control surface**. This is documented here per the skill's "make informed guesses" guideline; the user can re-invoke `/speckit-specify` with a more specific description if MIDI input is not what they wanted.
- The informed-guess choice is **MIDI input only** (Note On / Note Off / CC). MIDI output (clock, Thru, note forwarding) is **out of scope** for v1.
- Listener has a USB MIDI controller already connected before launching `--midi`. The synth does not require a specific controller model; any class-compliant device works.
- The MIDI handler mirrors the existing live keystroke handler's structure (read from input source, enqueue events, route to a dispatcher) so the routing layer is platform-independent.
- The audio engine's voice pool, generative state, and effects chain are unchanged. MIDI events are an additional input source layered on top of the existing trigger model.
- Linux MIDI input is via the ALSA sequencer API (already a libasound dep on Linux). Windows uses `winmm` (already linked for `waveOut`).
- Per Constitution Principle III (Deterministic, v1.0.1), the MIDI thread MUST NOT introduce a clock read; the audio thread's `time()` is not invoked when MIDI is active.
- The first cut maps Note On to a single FM voice (mimicking the live-mode melody handler's per-step FM/KS alternation, simplified). Future specs may extend to polyphonic multivoice MIDI keyboard behavior or a dedicated MIDI melody generator.
- The CC mapping table (FR-020) is intentionally hard-coded in v1. A future spec may add user-configurable mappings, but the static table keeps the v1 PR small per Constitution Principle VII.

## Out of Scope

- MIDI output (clock out, note forwarding, Thru).
- MPE / MIDI 2.0 / per-note expression.
- MIDI SysEx configuration.
- MIDI file playback.
- Saving / loading MIDI mappings (the static table in v1 is hard-coded).
- Visual feedback for incoming MIDI (already implicit in the status row's existing parameter fields).
- Pitch bend, aftertouch, channel pressure.
- Note On → generative trigger (i.e., not just direct voice trigger; v1 is purely a direct voice trigger, not a generative-seed input).
- `--midi-record` to capture generated output as MIDI file.

## Constitution Compliance

| Principle | Status | Note |
|---|---|---|
| I. Tiny Native Binary | ✅ expected | New modules add ~1-2 KB to the unpacked Linux binary; Windows packed stays within 48 KB. |
| II. C99 Only | ✅ expected | Pure C99; libasound on Linux, winmm on Windows. |
| **III. Deterministic (NON-NEGOTIABLE, v1.0.1)** | ✅ expected | MIDI thread does not read `time()`; with no MIDI input, output is byte-identical to baseline. |
| IV. Ambient + Algorithmic Aesthetic | ✅ expected | MIDI is an *input*; doesn't alter the generative aesthetic. |
| V. Cleanly Modular | ✅ expected | `audio_midi.c` / `audio_midi.h` + platform backends, mirroring the existing `audio_pulse` / `audio_winmm` split. |
| VI. Test Discipline (NON-NEGOTIABLE) | ✅ expected | New `tests/unit/test_midi.c`; per-file coverage gates met; new smoke test for loopback. |
| VII. No Partial Features | ✅ expected | Single PR: CC mapping, Note On/Off routing, device listing, unit + smoke tests. |
| VIII. Document Why, Not What | ✅ expected | This spec documents the informed guess + the rationale; the WHAT lives in the implementation PR. |
| IX. Cross-Platform From Day One | ✅ expected | ALSA sequencer on Linux + Win32 MIDI on Windows, same internal representation. |
| X. Generative > Random | n/a | MIDI is an input control surface, not a generative feature. |
