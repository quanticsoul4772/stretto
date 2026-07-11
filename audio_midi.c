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
#include "effects.h"    /* T030 CC dispatch reaches voice/reverb/delay/compressor */
#include "gen.h"

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

static midi_queue_t q;  /* BSS-allocated (the 128 KB arena is untouched by MIDI). */

/* CC_MAP[128] - the static CC -> synth-parameter routing table (per
 * data-model.md Entity 4 + tasks.md T029). One entry per MIDI CC
 * number; unspecified indices are zero-initialized by the designated
 * initializer (C99 §6.7.8 p21) so they default to { target=NONE,
 * scale=0 } and the dispatch below treats them as silently dropped.
 * Total footprint: 128 * sizeof(cc_map_entry_t) bytes in .rodata
 * (data-model.md budgets 512 B; actual may be 8 B/entry on platforms
 * where cc_target_t is enum-int-sized -- documented as a known spec
 * drift so a future budget bump can lift it cleanly).
 *
 * Scale is signed (int8) so the (V - 64) * scale delta can swing
 * negative within `voice_get_*` clamp ranges. CC#7's +60 scale makes
 * (V-64)*60 reach roughly +/-3780 on V swing 0..127 against the
 * compressor's [8000, 30000] threshold range -- well-matched to the
 * documented ~+/-63 swing on the +/-1-scale CCs against the user's
 * narrower [30, 180] voice_cutoff / [0, 180] voice_resonance clamps. */
static const cc_map_entry_t CC_MAP[128] = {
    [0]   = { .target = CC_TARGET_NONE },                            /* Bank Select (MSB) - unassigned */
    [1]   = { .target = CC_TARGET_CUTOFF,           .scale = +1  },  /* Mod Wheel */
    [7]   = { .target = CC_TARGET_COMPRESSOR_THRESH,.scale = +60 },  /* Channel Volume (MIDI 1.0 standard name; synth routes to compressor threshold per data-model.md Entity 4) */
    [10]  = { .target = CC_TARGET_NONE },                            /* Pan - unassigned */
    [16]  = { .target = CC_TARGET_NONE },                            /* General Purpose 1 - silently dropped per Principle VII */
    [17]  = { .target = CC_TARGET_NONE },                            /* General Purpose 2 */
    [19]  = { .target = CC_TARGET_NONE },                            /* General Purpose 4 */
    [64]  = { .target = CC_TARGET_NONE },                            /* Sustain Pedal - silently dropped per Principle VII (Phase 2 reserved) */
    [71]  = { .target = CC_TARGET_RESONANCE,        .scale = +1  },  /* Resonance / Timbre */
    [74]  = { .target = CC_TARGET_CUTOFF,           .scale = +1  },  /* Brightness / Cutoff */
    [91]  = { .target = CC_TARGET_REVERB_WET,       .scale = +1  },  /* Reverb Send */
    [93]  = { .target = CC_TARGET_DELAY_WET,        .scale = +1  },  /* Delay / Chorus Send */
    [123] = { .target = CC_TARGET_NONE }                             /* All Notes Off - silently dropped per Principle VII */
};

/* FR-010 scale-degree + octave-clamp mapper. Computes the absolute
   MIDI note corresponding to raw input key K:
     1) letter-degree = K % 7  picks one of 7 modal pitches in the
        current scale via gen_get_scale_note(scale_idx, degree)
        (= SCALES[cur_scale][K%7] absolute MIDI note);
     2) octave_offset = K/7 - 5  clamped to [-2, +4] per spec;
     3) mapped note = base_degree_pitch + (octave * 12)  clamped to
        uint8 [0, 127] so it safely indexes note_phase_inc[128] in
        voice.c.
   Identical to the static `midi_scale_map()` helper in
   tests/unit/test_midi.c (T015 asserts the same formula). Drain
   exercises it on the live path; static-helper tests cover the
   math, channel-filter+pop loop exercises the call-shape. */
static uint8_t map_midi_note(uint8_t key) {
    int oct_raw     = (int)(key / 7u) - 5;
    int oct_clamped = oct_raw < -2 ? -2 : (oct_raw > 4 ? 4 : oct_raw);
    int mapped      = (int)gen_get_scale_note(gen_get_scale(), key % 7u)
                    + (oct_clamped * 12);
    if (mapped <   0) mapped =   0;
    if (mapped > 127) mapped = 127;
    return (uint8_t)mapped;
}

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
    int rc = audio_midi_linux_init(device_index);
#elif defined(_WIN32)
    int rc = audio_midi_winmm_init(device_index);
#else
    (void)device_index;
    int rc = -1;
#endif
    /* FR-001 + FR-002 + FR-034 mirror: if the platform backend
     * failed to bind (no hw plugged in, libasound missing, the
     * addressed MIDI controller is unplugged, midiInOpen errored,
     * etc.), reset g_enabled so audio_midi_enqueue /
     * audio_midi_drain short-circuit on the next audio-thread tick.
     * Without this, audio_midi_init()'s optimistic g_enabled = 1
     * would persist past a failed audio_midi_open() and leave the
     * synth believing a phantom device is bound -- a byte-divergence
     * from the FR-050/FR-053 --no-midi baseline and a misleading CC
     * surface for the operator (CCs would still modulate params even
     * though no controller is producing them, because the ring is
     * never drained). main.c's "MIDI: device index N unavailable"
     * stderr still surfaces on its own without needing this side
     * effect. */
    if (rc != 0) g_enabled = 0;
    return rc;
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

        /* Per FR-010: scale-degree + octave-offset mapping. Compute
         * once per event (1 modulo + 1 divide + table lookup + 2
         * saturating adds) so Note On / Note Off / Velocity-0 routes
         * all hit voice_pool_trigger_midi() / voice_pool_release_midi()
         * with the same absolute MIDI note - voice_pool_release_midi
         * matches via trigger_key = the note passed to trigger_midi,
         * so a single mapping keeps trigger and release consistent
         * without an inverse-map table. Channel filter above keeps
         * mapping-cost off the rejected-channel path. */
        uint8_t mapped_key = map_midi_note(ev.key);

        switch (ev.type) {
            case MIDI_EVENT_NOTE_ON:
                if (ev.value == 0) {
                    /* Per FR-011: Note On with velocity 0 = Note Off. */
                    voice_pool_release_midi(mapped_key, ev.channel);
                } else {
                    voice_pool_trigger_midi(mapped_key, ev.value, ev.channel);
                }
                break;
            case MIDI_EVENT_NOTE_OFF:
                voice_pool_release_midi(mapped_key, ev.channel);
                break;
            case MIDI_EVENT_CC: {
                /* US2 (spec 003 + tasks.md T030): CC dispatch via
                 * CC_MAP[ev.key] -> adjust_*(delta). Formula
                 * (data-model.md Entity 4 + H1 fix):
                 *   delta = (V - 64) * scale
                 * CC_TARGET_NONE entries are silently dropped --
                 * no fprintf, no callback overhead (Principle VII).
                 * Multiple CCs targeting the same parameter sum
                 * additively per FR-022 because the adjust_* calls
                 * compose over the previous value.
                 * Bounds guard: CC_MAP has exactly 128 entries and
                 * ev.key is uint8_t (0..255) filled by the backends -
                 * a malformed producer value > 127 must not index
                 * .rodata out of bounds. */
                if (ev.key > 127) break;
                const cc_map_entry_t *entry = &CC_MAP[ev.key];
                if (entry->target == CC_TARGET_NONE) break;
                int delta = ((int)ev.value - 64) * (int)entry->scale;
                switch (entry->target) {
                    case CC_TARGET_CUTOFF:            voice_adjust_cutoff(delta);            break;
                    case CC_TARGET_RESONANCE:         voice_adjust_resonance(delta);         break;
                    case CC_TARGET_REVERB_WET:        reverb_adjust_wet(delta);              break;
                    case CC_TARGET_DELAY_WET:         delay_adjust_wet(delta);               break;
                    case CC_TARGET_DELAY_FEEDBACK:    delay_adjust_feedback(delta);          break;
                    case CC_TARGET_FILTER_LFO_DEPTH:  voice_adjust_lfo_filter_depth(delta);  break;
                    case CC_TARGET_MUTATION_RATE:
                        /* gen_force_mutate() not yet implemented (Q-phase
                         * out of scope for v1); CC is silently absorbed
                         * so a producer that targets this slot does not
                         * error, but produces no audible effect. */
                        (void)delta;
                        break;
                    case CC_TARGET_COMPRESSOR_THRESH: compressor_adjust_threshold(delta);   break;
                    default: break;  /* future targets: harmless no-op */
                }
                break;
            }
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
