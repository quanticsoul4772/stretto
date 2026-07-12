/* audio_midi.h - cross-platform MIDI input interface (003-midi-input).
 *
 * Compilation unit selection: Linux picks audio_midi_linux.c (libasound
 * ALSA sequencer backend); Windows picks audio_midi_winmm.c (Win32
 * midiInProc backend). Both backends compile against this header with
 * no platform-specific #ifdef guards - the Makefile selects which
 * .o to include in OBJS / WIN_OBJS so the wrong backend is never
 * linked into the wrong binary. The static dispatch in audio_midi.c
 * picks the right runtime path via audio_midi_open() below.
 *
 * Architecture (per spec.md + data-model.md + research.md D1-D7):
 *   - SPSC ring buffer of midi_event_t (256 entries, BSS-allocated
 *     zero-init; no malloc, per FR-040 + Constitution Memory model.
 *     BSS zero-init preserves the --no-midi byte-identical path
 *     since audio_midi_drain early-returns on (g_enabled == 0).
 *     (Updated 2026-07-06: previously documented as arena-allocated
 *     but the arena mirror call was dropped by code-review Q1.)
 *   - Single producer: platform-specific worker thread (Linux:
 *     pthread_create + blocking snd_seq_event_input, real impl in
 *     audio_midi_linux.c; Windows: midiInProc via winmm, real impl
 *     in audio_midi_winmm.c). Phase 1+2 backend is a no-op stub.
 *   - Single consumer: audio thread, called from render_chunk via
 *     mixer.c. Reads head once per ~21 ms via __atomic_load_n.
 *   - Channel filter (per FR-004 + M1 fix) applied inside drain,
 *     BEFORE dispatch.
 *   - g_enabled opt-out path keeps --no-midi byte-identical to
 *     baseline (per FR-050 / FR-053 / Constitution III v1.0.1).
 */
#ifndef AUDIO_MIDI_H
#define AUDIO_MIDI_H

#include <stdint.h>

/* --- SPSC ring buffer sizes (FR-040 + data-model.md Entity 3) --- */
#define MIDI_QUEUE_CAPACITY      256   /* power-of-two for & MASK */
#define MIDI_QUEUE_MASK          (MIDI_QUEUE_CAPACITY - 1)

/* --- Enumerate-list cap (PR #108 / T034 + T035 contract) ---
 * Both audio_midi_linux_list_devices (snd_seq_query_next_port walk)
 * and the T035 winmm enumerator (midiInGetNumDevs + midiInGetDevCaps
 * walk) cap subscription-count at this constant so a studio rig with
 * >32 controllers can't run away. 32 matches the documented
 * `--midi-list-devices` printf width + the typ. USB-MIDI class-count
 * ceiling on Linux (USB subsystem limit) + midiInputPortCount on
 * Windows. The constant was not present in the merged source
 * despite the spec + PR #108's PR description referencing it; this
 * file restores it so the backends compile cleanly under
 * `make` + `make coverage`. */
#define MIDI_LIST_DEVICES_CAP    32

/* --- Event type discriminator (data-model.md Entity 1) --- */
typedef enum {
    MIDI_EVENT_NONE   = 0,
    MIDI_EVENT_NOTE_ON    = 1,
    MIDI_EVENT_NOTE_OFF   = 2,
    MIDI_EVENT_CC         = 3,
    MIDI_EVENT_PITCH_BEND = 4   /* 072/FR-015: 14-bit bend packed as
                                   key = LSB (bits 0-6),
                                   value = MSB (bits 7-13);
                                   0..16383, center 8192 */
} midi_event_type_t;

/* --- Single MIDI event record (data-model.md Entity 1) --- */
typedef struct {
    uint8_t type;     /* midi_event_type_t; 0 = MIDI_EVENT_NONE / unused */
    uint8_t channel;  /* 1..16 per MIDI 1.0 spec */
    uint8_t key;      /* 0..127 (note number for Note On/Off, controller number for CC) */
    uint8_t value;    /* 0..127 (velocity for Note On, ignored for Note Off, value for CC) */
} midi_event_t;

/* --- --midi-list-devices row (data-model.md Entity 2) --- */
typedef struct {
    int32_t index;
    char    name[64];  /* truncated if longer */
} midi_input_device_t;

/* --- CC dispatch types (data-model.md Entity 4 + FR-020 / tasks.md T029) ---
 *
 * cc_target_t enumerates the synth parameters a CC can modulate.
 * The static CC_MAP[128] in audio_midi.c maps each CC number to a
 * target + scale; the dispatch switch in audio_midi_drain() calls
 * the matching voice_* / reverb_* / delay_* / compressor_* adjust
 * entry point with `delta = (V - 64) * scale` (H1 fix). CC_TARGET_NONE
 * entries are silently dropped per Principle VII.
 *
 * Ordering matches data-model.md Entity 4 verbatim so future spec
 * updates slot in at the end without renumbering. */
typedef enum {
    CC_TARGET_NONE = 0,                /* unassigned; CC is ignored */
    CC_TARGET_CUTOFF,                   /* voice_adjust_cutoff(delta) */
    CC_TARGET_RESONANCE,                /* voice_adjust_resonance(delta) */
    CC_TARGET_REVERB_WET,               /* reverb_adjust_wet(delta) */
    CC_TARGET_DELAY_WET,                /* delay_adjust_wet(delta) */
    CC_TARGET_DELAY_FEEDBACK,           /* delay_adjust_feedback(delta) */
    CC_TARGET_FILTER_LFO_DEPTH,         /* voice_adjust_lfo_filter_depth(delta) */
    CC_TARGET_MUTATION_RATE,            /* gen_force_mutate() (future) */
    CC_TARGET_COMPRESSOR_THRESH,        /* compressor_adjust_threshold(delta) */
    CC_TARGET_SUSTAIN,                  /* CC#64 pedal (065): raw VALUE
                                           semantics (>= 64 down, < 64 up),
                                           not the (V-64)*scale delta */
    CC_TARGET_ALL_NOTES_OFF             /* CC#123 (067): Note Off for every
                                           sounding voice on the channel;
                                           value byte deliberately ignored
                                           (MIDI defines it as 0 - liberal
                                           acceptance). Appended last per
                                           the no-renumbering contract. */
} cc_target_t;

/* One entry of the static CC_MAP[128]. target=CC_TARGET_NONE means
 * silently dropped. scale is signed (int8) so the `(V-64)*scale`
 * delta can swing negative; for target=NONE, scale is 0 and ignored.
 * _pad is explicit so the struct stays 4 B (128 entries * 4 = 512 B
 * in `.rodata` per data-model.md field sizing summary). */
typedef struct {
    cc_target_t target;
    int8_t      scale;
    uint8_t     _pad;
} cc_map_entry_t;

/* --- Audio-thread-side API (audio_midi.c) --- */
void     audio_midi_init(int channel_filter);
int      audio_midi_open(int device_index);
void     audio_midi_close(void);
void     audio_midi_drain(void);          /* called from mixer.c render_chunk */
void     audio_midi_enqueue(const midi_event_t *ev);  /* producer-side */
int      audio_midi_list_devices(midi_input_device_t *out, int32_t *count);
uint32_t audio_midi_drop_count(void);

/* --- Platform backend hooks (audio_midi_linux.c / audio_midi_winmm.c) --- *
 * Phase 1+2: both backends return -1 so audio_play() (live mode) does     *
 * not activate any MIDI thread. US1 (T022/T023/T031-T035) replaces these   *
 * with the real pthread_create + snd_seq_event_input loop on Linux and    *
 * midiInOpen + midiInProc callback on Windows, plus the matching list-    *
 * devices enumerators (snd_seq_client_info + midiInGetDevCaps).            *
 */
int  audio_midi_linux_init(int device_index);
void audio_midi_linux_close(void);
int  audio_midi_linux_list_devices(midi_input_device_t *out, int32_t *count);

int  audio_midi_winmm_init(int device_index);
void audio_midi_winmm_close(void);
int  audio_midi_winmm_list_devices(midi_input_device_t *out, int32_t *count);

#endif /* AUDIO_MIDI_H */
