/* audio_midi_linux.c - libasound ALSA sequencer backend (US1 / T022).
 *
 * Phase 1+2 stub returned -1 from init; US1 replaces it with a real
 * pthread worker draining snd_seq_event_input() into the SPSC ring,
 * per spec/003-midi-input/research.md D1 (canonical libasound pattern;
 * snd_seq_create_thread() is NOT a stock libasound API).
 *
 * Backend design:
 *   snd_seq_t *g_seq, int g_port, pthread_t g_thread - module-static
 *   singleton state. Init creates a SND_SEQ_OPEN_INPUT handle to
 *   "default", names the client "Stretto", creates one generic
 *   writable application port "Stretto Input", spawns one worker
 *   pthread that polls snd_seq_poll_descriptors for 100 ms, drains
 *   any pending events via snd_seq_event_input, and re-enqueues each
 *   NOTEON / NOTEOFF / CONTROLLER into the SPSC ring via
 *   audio_midi_enqueue(). End-users route their hardware to Stretto
 *   via `aconnect <client> 129:0` (no auto-subscribe so a wrong
 *   subscription layout doesn't fight the user's aconnect choices).
 *
 *   All non-subscribed event types (active sensing, clock, sysex,
 *   etc) fall through silently - queue slots are precious (256
 *   entries), so MIDI_EVENT_NONE is NOT enqueued for them.
 *
 *   ALSA uses 0..15 for channels; MIDI_EVENT_NOTE_ON.channel lives in
 *   the MIDI 1.0 1..16 domain, so each parser branch adds +1.
 *
 *   Cooperative shutdown: audio_midi_linux_close clears the atomic
 *   g_run flag, joins the pthread. The worker's loop body polls for
 *   100 ms then exits; full shutdown latency is one poll window with
 *   no self-pipe / spare fd required.
 *
 * Compile guard: this .o is in Makefile's OBJS but not WIN_OBJS so
 * Windows builds never see these symbols. The .c file pulls in
 * alsa/asoundlib.h + pthread.h + poll.h but only on Linux. ci.yml
 * installs libasound2-dev on stock Ubuntu runners so the pkg-config
 * gate in the Makefile becomes active (linker pulls -lasound there)
 * while dev boxes without libasound2-dev still build partial
 * artifacts (compile fails on missing header).
 */
#include "audio_midi.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>

/* Reserve a fixed-size pollfd array so the worker is portable to
   both gcc and clang (no __builtin_alloca). A single SND_SEQ_OPEN_INPUT
   handle gives exactly 1 POLLIN fd; we leave a margin of 4 in case
   future expanded subscriptions add more. */
#define ALSA_MAX_PFDS   4
#define ALSA_POLL_MS  100

static snd_seq_t *g_seq   = NULL;
static int        g_port  = -1;
static pthread_t  g_thread;
/* atomic: cleared by audio_midi_linux_close to wake the worker; the
 * worker polls with __atomic_load_n / __ATOMIC_ACQUIRE; shutdown sets
 * with __atomic_store_n / __ATOMIC_RELEASE. Pairing honors the C11
 * acquire/release model in audio_midi.c's research.md D3. */
static uint32_t   g_run   = 0u;

static void *midi_worker(void *arg) {
    (void)arg;
    int npfd = snd_seq_poll_descriptors_count(g_seq, POLLIN);
    if (npfd <= 0 || npfd > ALSA_MAX_PFDS) return NULL;
    struct pollfd pfd[ALSA_MAX_PFDS];
    snd_seq_poll_descriptors(g_seq, pfd, npfd, POLLIN);

    /* Outer loop is the cooperative-shutdown gate (cleared by
       audio_midi_linux_close via the atomic g_run flag). Inner
       loop drains any events that the single poll wake exposed,
       guarded by snd_seq_event_input_pending() so snd_seq_event_input
       does not block mid-drain (research.md D1: blocking semantics,
       but per-event drain is non-blocking once poll() has fired). */
    while (__atomic_load_n(&g_run, __ATOMIC_ACQUIRE)) {
        if (poll(pfd, (nfds_t)npfd, ALSA_POLL_MS) <= 0) continue;
        while (snd_seq_event_input_pending(g_seq, 1) > 0) {
            snd_seq_event_t *ev = NULL;
            if (snd_seq_event_input(g_seq, &ev) < 0 || ev == NULL) break;

            midi_event_t qev = {
                .type    = MIDI_EVENT_NONE,
                .channel = 0,
                .key     = 0,
                .value   = 0
            };
            switch (ev->type) {
                case SND_SEQ_EVENT_NOTEON:
                    qev.type    = MIDI_EVENT_NOTE_ON;
                    qev.channel = (uint8_t)(ev->data.note.channel + 1);
                    qev.key     = (uint8_t)ev->data.note.note;
                    qev.value   = (uint8_t)ev->data.note.velocity;
                    break;
                case SND_SEQ_EVENT_NOTEOFF:
                    qev.type    = MIDI_EVENT_NOTE_OFF;
                    qev.channel = (uint8_t)(ev->data.note.channel + 1);
                    qev.key     = (uint8_t)ev->data.note.note;
                    qev.value   = (uint8_t)ev->data.note.velocity;
                    break;
                case SND_SEQ_EVENT_CONTROLLER:
                    qev.type    = MIDI_EVENT_CC;
                    qev.channel = (uint8_t)(ev->data.control.channel + 1);
                    qev.key     = (uint8_t)ev->data.control.param;
                    qev.value   = (uint8_t)ev->data.control.value;
                    break;
                case SND_SEQ_EVENT_PORT_EXIT:
                    /* fallthrough */
                case SND_SEQ_EVENT_CLIENT_EXIT:
                    /* FR-034 graceful disconnect: a connected ALSA
                       source port (PORT_EXIT) or whole client
                       (CLIENT_EXIT) just went away. Do NOT call
                       audio_midi_close() here -- that path calls
                       audio_midi_linux_close() which pthread_joins
                       this very thread, and pthread_join-on-self is
                       undefined behavior. Instead, flip the
                       cooperative-shutdown flag so the outer loop
                       exits; app shutdown then completes the teardown
                       cleanly via audio_midi_close(). */
                    __atomic_store_n(&g_run, 0u, __ATOMIC_RELEASE);
                    return NULL;
                default:
                    /* Sysex / clock / active-sensing / pitch-bend /
                       program change fall through; do NOT enqueue
                       MIDI_EVENT_NONE (queue slots are precious). */
                    continue;
            }
            audio_midi_enqueue(&qev);
        }
    }
    return NULL;
}

int audio_midi_linux_init(int device_index) {
    (void)device_index;   /* phase 1: end-user routes via aconnect */
    if (g_seq != NULL) return 0;   /* idempotent */

    snd_seq_t *seq = NULL;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) return -1;
    if (snd_seq_set_client_name(seq, "Stretto") < 0) {
        snd_seq_close(seq);
        return -1;
    }
    int port = snd_seq_create_simple_port(
        seq, "Stretto Input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_NO_EXPORT,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) {
        snd_seq_close(seq);
        return -1;
    }

    g_seq  = seq;
    g_port = port;

    __atomic_store_n(&g_run, 1, __ATOMIC_RELEASE);
    if (pthread_create(&g_thread, NULL, midi_worker, NULL) != 0) {
        snd_seq_close(g_seq);
        g_seq  = NULL;
        g_port = -1;
        return -1;
    }
    return 0;
}

void audio_midi_linux_close(void) {
    if (g_seq == NULL) return;
    /* Per research.md D3: producer / shutdown flag is paired atomic.
       The worker's outer-loop acquire-load sees 0 within ~100 ms
       (one poll window); pthread_join then waits for the worker to
       actually exit. No race with snd_seq_close since the worker is
       already gone by the time we close the seq handle. */
    __atomic_store_n(&g_run, 0, __ATOMIC_RELEASE);
    pthread_join(g_thread, NULL);
    snd_seq_close(g_seq);
    g_seq  = NULL;
    g_port = -1;
}

int audio_midi_linux_list_devices(midi_input_device_t *out, int32_t *count) {
    (void)out; (void)count;
    /* Implemented in T034 (snd_seq_query_next_client +
       snd_seq_query_get_port_info); T022 keeps the stub so re-builds
       without libasound2-dev still link cleanly via --gc-sections. */
    return -1;
}
