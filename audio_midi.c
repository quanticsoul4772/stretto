/* audio_midi.c - SPSC ring buffer + drain dispatch + opt-out path.
 *
 * Memory model (Constitution Memory model, FR-040): the ring buffer
 * lives in BSS as static midi_queue_t q below (zero-init at process
 * start). audio_midi_init(-1) for --no-midi short-circuits without
 * touching q; the audio thread's drain() then early-returns on
 * (g_enabled == 0), preserving the bitexact 16-s SHA-256 golden at
 * golden/regression_16s.sha256 (FR-050 / FR-053 / Constitution III
 * v1.0.1). The arena footprint is NOT consumed by the queue
 * (code-review Q1 fix 2026-07-06: dropped the earlier
 * `arena_alloc(sizeof(midi_queue_t))` mirror which discarded its
 * result and consumed 1024 B of arena for no purpose).
 *
 * Concurrency (research.md D3 corrected 2026-07-06): producer (Linux:
 * pthread worker in audio_midi_linux.c; Windows: midiInProc callback
 * from audio_midi_winmm.c) writes events[head & MASK] then
 * release-stores head. Before that it acquire-loads tail for the
 * soft-rate overflow check. Consumer (audio thread inside
 * audio_midi_drain) acquire-loads head once, then loops acquire-
 * loading tail and release-storing tail+1 after each dispatch.
 * All cross-thread accesses to q.head + q.tail go through
 * __atomic_*_n - required by the C standard (C99 §6.5p7 and
 * C11 §5.1.2.3 cross-thread access); on x86 (Constitution III
 * v1.0.1) the compiler emits plain mov with no mfence overhead.
 */
#include "audio_midi.h"
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

}

int audio_midi_open(int device_index) {
    if (!g_enabled) return -1;
    /* Platform selection via Makefile-compiled backend: only the .o
     * whose platform matches this build is linked. The #ifdef here
     * mirrors the .objs selection in the Makefile - both backends are
     * declared in audio_midi.h, but only one is linked into a given
     * binary, so the ifdef-guard prevents the compiler from emitting
     * an unresolved-symbol reference at link time. (code-review Q2
     * fix 2026-07-06: previously called both backends unconditionally
     * - --gc-sections cannot eliminate an unresolved direct reference
     * before link resolution, so Linux builds errored on
     * audio_midi_winmm_* and Windows builds errored on
     * audio_midi_linux_*.) */
#if defined(__linux__) || defined(__APPLE__)
    return audio_midi_linux_init(device_index);
#elif defined(_WIN32)
    return audio_midi_winmm_init(device_index);
#else
    (void)device_index;
    return -1;
#endif
}

void audio_midi_close(void) {
#if defined(__linux__) || defined(__APPLE__)
    audio_midi_linux_close();
#elif defined(_WIN32)
    audio_midi_winmm_close();
#endif
    g_enabled = 0;
}

void audio_midi_enqueue(const midi_event_t *ev) {
    if (!g_enabled || ev == 0) return;
    /* Soft-rate overflow check: producer acquire-loads tail to compare
       against its own (plain) head. Worst-case torn read (between
       consumer's advance of tail and this load) accepts one extra
       event that the consumer's per-iteration tail-check catches at
       dispatch; no audio glitch. (code-review Q3 fix 2026-07-06:
       replaced the plain read of q.tail with __atomic_load_n to make
       cross-thread access well-defined per C99 §6.5p7 / C11 §5.1.2.3
       - x86 TSO would have made the plain read benign at runtime,
       but the standard says it is UB.) */
    uint32_t head = q.head;
    uint32_t tail = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE);
    if (head - tail >= MIDI_QUEUE_CAPACITY) {
        g_drops++;
        return;
    }
    q.events[head & MIDI_QUEUE_MASK] = *ev;
    __atomic_store_n(&q.head, head + 1u, __ATOMIC_RELEASE);
}

void audio_midi_drain(void) {
    if (!g_enabled) return;
    /* Single acquire-load per ~21 ms chunk; x86 TSO => plain mov.
     * Loop body acquire-loads tail, dispatches one event, release-
     * stores tail+1. Splitting tail into a local replaces the
     * former plain load+store which was UB per C99 §6.5p7 and
     * C11 §5.1.2.3 (code-review Q3 fix 2026-07-06). */
    uint32_t local_head = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
    uint32_t tail;
    while ((tail = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE)) != local_head) {
        midi_event_t ev = q.events[tail & MIDI_QUEUE_MASK];
        __atomic_store_n(&q.tail, tail + 1u, __ATOMIC_RELEASE);
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
#if defined(__linux__) || defined(__APPLE__)
    return audio_midi_linux_list_devices(out, count);
#elif defined(_WIN32)
    return audio_midi_winmm_list_devices(out, count);
#else
    (void)out; (void)count;
    return -1;
#endif
}

uint32_t audio_midi_drop_count(void) { return g_drops; }
