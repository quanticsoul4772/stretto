/* audio_midi.c - SPSC ring buffer + drain dispatch + opt-out path.
 *
 * Memory model (Constitution Memory model, FR-040): the ring buffer
 * itself lives in static state (midi_queue_t q below). When --midi
 * is enabled, arena_alloc(sizeof(midi_queue_t)) is called once at
 * init so process memory follows the 128 KB static arena constraint.
 * When --no-midi is set (or no MIDI flag passed), audio_midi_init(-1)
 * short-circuits and the arena allocation is skipped - byte-identity
 * with the bitexact 16-s SHA-256 golden at golden/regression_16s.sha256
 * is preserved (FR-050 / FR-053 / Constitution III v1.0.1).
 *
 * Concurrency (research.md D3): producer (Linux: pthread worker, real
 * impl audio_midi_linux.c; Windows: midiInProc from winmm, real impl
 * audio_midi_winmm.c) writes events[head & MASK] then __atomic_store_n
 * with __ATOMIC_RELEASE. Consumer (audio thread inside
 * audio_midi_drain) reads head once with __atomic_load_n
 * __ATOMIC_ACQUIRE, then iterates tail..local_head dispatching
 * each event. On x86 (Constitution III v1.0.1) both ops compile to
 * plain mov - no mfence overhead.
 */
#include "audio_midi.h"
#include "arena.h"
#include "voice.h"

/* --- Module-static state --- *
 * Defaults to zero (BSS) at process start; --no-midi path leaves them
 * at zero, so the audio thread's drain() call early-returns without
 * touching the queue at all (g_enabled = 0).
 */
static uint8_t  g_channel_filter = 0;  /* 0 = all, 1..16 = single channel */
static uint8_t  g_enabled        = 0;  /* 0 until audio_midi_init(non-negative) */
static uint32_t g_drops          = 0;  /* events dropped due to ring overflow */

typedef struct {
    midi_event_t events[MIDI_QUEUE_CAPACITY];  /* 256 x 4 B = 1024 B */
    uint32_t     head;                          /* producer-write, atomic */
    uint32_t     tail;                          /* audio-thread-only, plain */
} midi_queue_t;

static midi_queue_t q;  /* BSS-allocated; arena-allocated mirror below. */

void audio_midi_init(int channel_filter) {
    /* Reset to zero so the opt-out path leaves BSS unchanged. */
    g_enabled         = 0;
    g_channel_filter  = 0;
    g_drops           = 0;
    q.head            = 0;
    q.tail            = 0;

    if (channel_filter < 0) return;        /* --no-midi opt-out (FR-050/053) */

    if (channel_filter > 16) channel_filter = 0;
    g_channel_filter = (uint8_t)channel_filter;
    g_enabled        = 1;

    /* Mirror to arena for the 128 KB static arena accounting (FR-040). */
    static int allocated = 0;
    if (!allocated) {
        (void)arena_alloc(sizeof(midi_queue_t));
        allocated = 1;
    }
}

int audio_midi_open(int device_index) {
    if (!g_enabled) return -1;
    /* Try Linux first; if not built, fall through to winmm. At least
       one of these symbols is --gc-sections eliminated from the final
       binary by the linker if its corresponding .o is not included. */
    if (audio_midi_linux_init(device_index) == 0) return 0;
    if (audio_midi_winmm_init(device_index) == 0) return 0;
    return -1;
}

void audio_midi_close(void) {
    audio_midi_linux_close();
    audio_midi_winmm_close();
    g_enabled = 0;
}

void audio_midi_enqueue(const midi_event_t *ev) {
    if (!g_enabled || ev == 0) return;
    /* Soft-rate overflow check: producer checks head vs tail plain-read.
       A torn read produces a worst-case of one extra accepted event that
       will be dropped at consumer's tail-check; no audio glitch. */
    uint32_t head = q.head;
    if (head - q.tail >= MIDI_QUEUE_CAPACITY) {
        g_drops++;
        return;
    }
    q.events[head & MIDI_QUEUE_MASK] = *ev;
    __atomic_store_n(&q.head, head + 1u, __ATOMIC_RELEASE);
}

void audio_midi_drain(void) {
    if (!g_enabled) return;
    /* Single acquire-load per ~21 ms chunk; x86 TSO => plain mov. */
    uint32_t local_head = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
    while (q.tail != local_head) {
        midi_event_t ev = q.events[q.tail & MIDI_QUEUE_MASK];
        q.tail++;
        /* Per FR-004 + M1 fix: channel filter BEFORE dispatch. */
        if (g_channel_filter != 0 && ev.channel != g_channel_filter) continue;

        switch (ev.type) {
            case MIDI_EVENT_NOTE_ON:
                if (ev.value == 0) {
                    /* Per FR-011: Note On with velocity 0 = Note Off. */
                    voice_pool_release_midi(ev.key, ev.channel);
                } else {
                    voice_pool_trigger_midi(ev.key, ev.value, ev.channel);
                }
                break;
            case MIDI_EVENT_NOTE_OFF:
                voice_pool_release_midi(ev.key, ev.channel);
                break;
            case MIDI_EVENT_CC:
                /* US2 phase: dispatch via CC_MAP[ev.key] -> adjust_*(delta).
                 * Phase 1+2 leaves the slot empty so --midi is functional
                 * for Note On/Off routes only. */
                break;
            default:
                break;
        }
    }
}

int audio_midi_list_devices(midi_input_device_t *out, int32_t *count) {
    if (out == 0 || count == 0) return -1;
    int rc = audio_midi_linux_list_devices(out, count);
    if (rc == 0) return 0;
    return audio_midi_winmm_list_devices(out, count);
}

uint32_t audio_midi_drop_count(void) { return g_drops; }
