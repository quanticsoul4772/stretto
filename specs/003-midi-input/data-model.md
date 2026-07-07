# Data Model: 003-midi-input (MIDI Input)

**Date**: 2026-07-06 · **Branch**: `003-midi-input` · **Spec**: [spec.md](./spec.md) · **Plan**: [plan.md](./plan.md)

This document defines the data structures, relationships, and state transitions for the MIDI input feature. It is the Phase 1 output of `/speckit-plan` and the contract that `audio_midi.c`, `audio_midi_linux.c`, `audio_midi_winmm.c`, and the new `tests/unit/test_midi.c` will code against.

---

## Entity 1: `midi_event_t` (the single MIDI event record)

**Location**: `audio_midi.h`

**Definition**:
```c
typedef struct {
    uint8_t  type;     /* 0=NOTE_ON, 1=NOTE_OFF, 2=CC (see enum below) */
    uint8_t  channel;  /* 1..16 */
    uint8_t  key;      /* 0..127 (Note On/Off) OR 0..127 (CC controller number) */
    uint8_t  value;    /* 0..127 (Note On velocity, Note Off velocity [ignored], CC value) */
} midi_event_t;
```

**Size**: 4 bytes (uint8_t × 4). Natural alignment, no padding. Designed to be lock-free-safe on x86 (any naturally-aligned 4-byte access is atomic on x86).

**Validation rules**:
- `type ∈ {0, 1, 2}` (the three event types we support)
- `channel ∈ [1, 16]`
- `key ∈ [0, 127]`
- `value ∈ [0, 127]`

**Lifecycle**: A `midi_event_t` is created by the platform callback (audio_midi_linux.c / audio_midi_winmm.c), written to the ring buffer, read by the audio thread, dispatched (translated into a `voice_pool_trigger_midi` / `voice_pool_release_midi` / `voice_adjust_cutoff` / etc. call), then dropped. The struct is **not retained** past the dispatch step.

**Enum**:
```c
enum {
    MIDI_EVENT_NOTE_ON  = 0,
    MIDI_EVENT_NOTE_OFF = 1,
    MIDI_EVENT_CC       = 2
};
```

---

## Entity 2: `midi_input_device_t` (enumeration record)

**Location**: `audio_midi.h`

**Definition**:
```c
typedef struct {
    int32_t  index;       /* 0-based, matches --midi <N> CLI argument */
    char     name[64];    /* Human-readable device name; truncated if longer */
} midi_input_device_t;
```

**Size**: 68 bytes (4 + 64). Fits in a stack-allocated array for `--midi-list-devices` output (max ~32 devices practical).

**Relationships**: Each `midi_input_device_t.index` corresponds to one `midi_event_t` stream — the device at index N is the one whose events are enqueued to the ring buffer when `synth --midi N` is set.

**Lifecycle**: Created on demand by `audio_midi_list_devices(midi_input_device_t *out, int32_t *count)` which:
1. Calls the platform-specific enumerate function (`audio_midi_linux.c: enumerate_via_snd_seq_client_info` / `audio_midi_winmm.c: enumerate_via_midiInGetDevCaps`)
2. Populates the output array
3. Sets `*count` to the populated length
4. Returns

The struct is **not retained** past the print step of `--midi-list-devices`.

**Platform-specific note**:
- Linux: ALSA sequencer enumerates all clients/ports that have a `MIDI_GENERIC` or `MIDI_KEYBOARD` capability. The synth's own announce-port (if any) is excluded.
- Windows: `midiInGetDevCaps` returns up to `midiInGetNumDevs()` devices; index is the 0-based positional index.

---

## Entity 3: `midi_queue_t` (the SPSC ring buffer)

**Location**: `audio_midi.h` (public type) + `audio_midi.c` (implementation)

**Definition**:
```c
#define MIDI_QUEUE_CAPACITY 256  /* power of two for fast & mask; per FR-040 */
#define MIDI_QUEUE_MASK     (MIDI_QUEUE_CAPACITY - 1)

typedef struct {
    midi_event_t events[MIDI_QUEUE_CAPACITY];  /* 256 × 4 = 1024 bytes; from arena per FR-040 */
    uint32_t     head;  /* producer-only write; consumer reads with __atomic_load_n(__ATOMIC_ACQUIRE) */
    uint32_t     tail;  /* consumer-only write; producer does not touch */
} midi_queue_t;
```

**Size**: 1024 + 4 + 4 = 1032 bytes. 8-byte aligned (per Constitution Memory model: 8-byte aligned bump allocator). Allocated from the arena at synth init time (per FR-040, "no `malloc`").

**SPSC invariants** (D3 decision):
- Single producer: the platform callback (Linux: ALSA-managed thread; Windows: midiInProc)
- Single consumer: the audio thread, inside `audio_midi_drain()` called at the start of every `render_chunk`
- Producer writes: `events[head & MASK] = ev; __atomic_store_n(&head, head + 1, __ATOMIC_RELEASE);`
- Consumer reads: `local_head = __atomic_load_n(&head, __ATOMIC_ACQUIRE); while (tail != local_head) { ev = events[tail & MASK]; dispatch(ev); tail++; } store tail back to struct (single-threaded, plain write)`
- No locks; no CAS; the acquire/release fence pair is sufficient for the x86 target (Constitution III v1.0.1).

**Overflow behavior**: If the consumer falls behind and `head - tail >= MIDI_QUEUE_CAPACITY`, the producer drops the new event (silently — no malloc, no fprintf from a callback). The dropped-event count is exposed via `audio_midi_drop_count()` for diagnostics; not surfaced in the UI in v1.

**Relationships**: One `midi_queue_t` per process. Created in `audio_midi_init()` from the arena. Lives in the audio_midi.c module's static state (BSS-residing since it doesn't change size).

**State diagram**:
```
[UNINIT] --audio_midi_init()--> [READY] --audio_midi_open(N)--> [OPEN]
[OPEN]  --callback enqueue-->    [OPEN] (events in queue)
[OPEN]  --audio_midi_drain()-->  [OPEN] (events consumed)
[OPEN]  --audio_midi_close()-->  [READY]  (when --midi was runtime-only)
[OPEN]  --device disconnect-->   [OPEN_DEGRADED] (per Q3: thread exits; synth continues)
[OPEN_DEGRADED]  --audio_midi_close()--> [READY]
[READY] --audio_midi_open(N)-->  [OPEN] (re-arm)
[READY/OPEN] --process exit-->   [UNINIT] (no explicit shutdown needed; arena reset on process exit)
```

---

## Entity 4: `cc_map_entry_t` (the static CC → parameter mapping table)

**Location**: `audio_midi.h` (public type) + `audio_midi.c` (static table)

**Definition**:
```c
typedef enum {
    CC_TARGET_NONE = 0,         /* unassigned; CC is ignored */
    CC_TARGET_CUTOFF,            /* per-voice filter cutoff; call voice_adjust_cutoff(delta) */
    CC_TARGET_RESONANCE,         /* per-voice filter resonance; call voice_adjust_resonance(delta) */
    CC_TARGET_REVERB_WET,        /* reverb wet mix; call reverb_adjust_wet(delta) */
    CC_TARGET_DELAY_WET,         /* delay wet mix; call delay_adjust_wet(delta) */
    CC_TARGET_DELAY_FEEDBACK,    /* delay feedback; call delay_adjust_feedback(delta) */
    CC_TARGET_FILTER_LFO_DEPTH,  /* filter LFO depth; call voice_adjust_lfo_filter_depth(delta) */
    CC_TARGET_MUTATION_RATE,     /* mutates the generative mutation rate; call gen_force_mutate() or similar */
    CC_TARGET_COMPRESSOR_THRESH  /* master compressor threshold; call compressor_adjust_threshold(delta) */
} cc_target_t;

typedef struct {
    cc_target_t target;  /* 0=unassigned, else target id */
    int8_t      scale;   /* 0 if target=NONE; else signed scale applied to (CC value - 64) */
    uint8_t     _pad;
} cc_map_entry_t;
```

**Size**: 4 bytes (1+1+1+1 padding). The table is 128 entries × 4 bytes = 512 bytes in `.rodata`.

**Static table** (`audio_midi.c`):
```c
static const cc_map_entry_t CC_MAP[128] = {
    /* 0..15: standard controllers */
    [0]  = { .target = CC_TARGET_NONE },                   /* Bank Select (MSB) */
    [1]  = { .target = CC_TARGET_CUTOFF,           .scale = +1  },  /* Mod Wheel */
    [7]  = { .target = CC_TARGET_COMPRESSOR_THRESH,.scale = +60 },  /* Channel Volume */
    [10] = { .target = CC_TARGET_NONE },                   /* Pan */
    ...
    [16] = { .target = CC_TARGET_NONE },                   /* General Purpose 1 - "all notes off" alias */
    [17] = { .target = CC_TARGET_NONE },                   /* General Purpose 2 */
    [19] = { .target = CC_TARGET_NONE },                   /* General Purpose 4 */
    [64] = { .target = CC_TARGET_NONE },                   /* Sustain Pedal (out of scope v1) */
    [71] = { .target = CC_TARGET_RESONANCE,        .scale = +1  },  /* Resonance / Timbre */
    [74] = { .target = CC_TARGET_CUTOFF,           .scale = +1  },  /* Brightness / Cutoff */
    [91] = { .target = CC_TARGET_REVERB_WET,       .scale = +1  },  /* Reverb Send */
    [93] = { .target = CC_TARGET_DELAY_WET,        .scale = +1  },  /* Chorus / Delay Send (often used for delay) */
    [123]= { .target = CC_TARGET_NONE },                   /* All Notes Off */
    /* ... all other indices: { .target = CC_TARGET_NONE } ... */
};
```

**Validation rules**:
- `target ∈ {CC_TARGET_NONE, ...}` (enum)
- `scale ∈ [-128, +127]` (int8_t)
- CCs with `target = CC_TARGET_NONE` are silently dropped

**Lifecycle**: Static compile-time constant. No runtime mutation in v1. (A future spec may add user-configurable mappings.)

**CC dispatch** (per FR-020, FR-021, FR-022):
- For a CC message with controller C, value V (consumed in the audio thread inside `audio_midi_drain()`):
  - **Channel filter** (per FR-004, M1 fix): if `--midi-channel N` is set (1..16) and the event's `channel` field != N, the event is silently dropped here, BEFORE the CC_MAP lookup. Default (`N=0`) means accept all channels.
  - `entry = CC_MAP[C]`
  - If `entry.target == CC_TARGET_NONE`: drop
  - Else: `delta = (V - 64) * entry.scale`
  - Dispatch: call the appropriate `voice_adjust_cutoff(delta)` / `reverb_adjust_wet(delta)` / etc.
- Multiple CCs targeting the same parameter sum additively (per FR-022), because each CC dispatch calls the `adjust_*` function with a delta and those functions are additively composable.

**Channel filter placement rationale** (M1 fix): the filter runs in the **audio-thread consumer** (inside `audio_midi_drain()`), NOT in the producer-side platform callback. This honors FR-033 (the audio thread is the only consumer; the callback must not call into the audio thread directly) and keeps the producer side channel-agnostic — the ALSA / winmm callbacks enqueue every event they've parsed, regardless of `--midi-channel N`. The same channel filter applies uniformly to Note On (before `voice_pool_trigger_midi`), Note Off (before `voice_pool_release_midi`), and CC (before `CC_MAP[C]` lookup); the drain function is the single point where the filter is consulted so the three event-type paths commit to identical behavior.

---

## Entity 5: `voice_pool_trigger_midi` / `voice_pool_release_midi` (voice pool extension)

**Location**: `voice.h` (declarations) + `voice.c` (implementations)

**New fields on `Voice`** (per D4 decision):
```c
typedef struct {
    /* ... all existing fields ... */
    uint8_t trigger_key;       /* MIDI key 0..127 that started this voice; 0xFF = not a MIDI voice */
    uint8_t trigger_channel;   /* MIDI channel 1..16; 0 = not a MIDI voice */
} Voice;
```

**New declarations** (in `voice.h`):
```c
/* MIDI-triggered voice (non-role-scoped, polyphonic 11 with voice stealing, matches `N_VOICES` in `voice.h:8`).
   velocity 1..127; on velocity=0 the spec requires Note-Off behavior, so
   callers should dispatch to voice_pool_release_midi instead. */
void voice_pool_trigger_midi(uint8_t note, uint8_t velocity, uint8_t channel);

/* MIDI Note-Off: find the voice triggered with (key, channel) and request
   release. No-op if not found or already in release. */
void voice_pool_release_midi(uint8_t key, uint8_t channel);
```

**Implementation** (`voice.c`):

`voice_pool_trigger_midi`:
1. Walk all N_VOICES (= 11) voices. Pick the first one where `env_phase == ENV_OFF` (idle).
2. If none idle, pick the one with `env_phase == ENV_R` (release) and **minimum `env_time`** (oldest in release per Q1). This is the Q1 voice-stealing rule.
3. If none in release either, pick the voice with minimum `env_time` regardless of phase (Q1 fallback).
4. Call `voice_trigger()` on the chosen voice with `note` mapped to the active scale:
   - `scaled_note = SCALES[cur_scale][note % 7] + (note / 7 - 5) * 12` (per FR-010; octave offset clamped to [-2*12, +4*12] to keep the playable range reasonable; will be tuned empirically)
5. Set `voice->type = VOICE_FM` (per FR-010 — MIDI fixed at FM since external triggers are not on the 16-step Euclidean grid).
6. Scale `env_amp` peak by `velocity / 127` clamped to `[64, 32767]` (per FR-010).
7. Set `voice->trigger_key = note` and `voice->trigger_channel = channel`.

`voice_pool_release_midi`:
1. Walk all N_VOICES voices. Find the first one where `trigger_key == key && trigger_channel == channel && env_phase != ENV_OFF`.
2. If found, set `env_phase = ENV_R; env_time = 0;` (matches the existing release path in `voice_trigger`'s `glide_advance` precedent and in `env_step`'s ENV_R case).
3. If `env_phase` is already `ENV_R`, no-op (per FR-013).
4. If no matching voice, no-op (per the spec's "Note Off for an unmatched key" edge case).

**State transitions** for a MIDI voice:
```
[OFF (idle)]  --voice_pool_trigger_midi()--> [ENV_A (attack)]
[ENV_A]        --env_time >= attack_n-->      [ENV_D (decay)]  (non-drum)
[ENV_D]        --env_time >= decay_n-->       [ENV_R (sustain → release)]
[ENV_R]        --env_time >= release_n-->     [OFF] (voice free)
[any]          --voice_pool_release_midi()--> [ENV_R] (immediate release, no-op if already ENV_R or OFF)
```

---

## Relationships summary

```
synth --midi [N]
  └─> audio_midi_open(N)
        └─> [platform] audio_midi_linux.c: snd_seq_open + snd_seq_create_thread(callback)
        └─> [platform] audio_midi_winmm.c: midiInOpen + midiInStart
              └─> callback: enqueue midi_event_t to midi_queue_t
                                          │
                                          ▼
                          render_chunk(buf, n)
                            └─> audio_midi_drain()
                                  ├─> CHANNEL FILTER (per FR-004, M1 fix): drop events whose channel != --midi-channel N (1..16; N=0 = accept all)
                                  ├─> NOTE_ON     → voice_pool_trigger_midi(note, vel, ch)
                                  ├─> NOTE_OFF    → voice_pool_release_midi(key, ch)
                                  └─> CC          → CC_MAP[C] → voice/reverb/delay/compressor adjust_*
```

---

## Field sizing summary (for memory budget verification)

| Structure | Size | Lifetime | Where |
|---|---|---|---|
| `midi_event_t` | 4 B | per event | stack/queue |
| `midi_input_device_t` | 68 B | per `--midi-list-devices` call | stack |
| `midi_queue_t` | 1032 B | process lifetime (allocated once) | arena |
| `CC_MAP[128]` | 512 B | process lifetime | `.rodata` |
| `Voice.trigger_key` | +1 B per voice | per voice | BSS (voice pool) |
| `Voice.trigger_channel` | +1 B per voice | per voice | BSS (voice pool) |
| **Total MIDI-specific data** | **~1.6 KB** | — | — |

Well within the 128 KB arena budget (currently ~50-60 KB used by voice pool + reverb + delay). No impact on the existing memory budget.
