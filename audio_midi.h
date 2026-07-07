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
 *   - SPSC ring buffer of midi_event_t (256 entries, arena-allocated;
 *     no malloc, per FR-040 + Constitution Memory model).
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
#define MIDI_QUEUE_CAPACITY 256   /* power-of-two for & MASK */
#define MIDI_QUEUE_MASK     (MIDI_QUEUE_CAPACITY - 1)

/* --- Event type discriminator (data-model.md Entity 1) --- */
typedef enum {
    MIDI_EVENT_NONE   = 0,
    MIDI_EVENT_NOTE_ON  = 1,
    MIDI_EVENT_NOTE_OFF = 2,
    MIDI_EVENT_CC       = 3
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
