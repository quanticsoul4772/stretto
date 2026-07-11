/* audio_midi_linux.c - libasound ALSA sequencer backend (T022 + PR
 * bind wiring).
 *
 * Backend design:
 *   snd_seq_t *g_seq, int g_port, pthread_t g_thread, int
 *   g_current_device_index - module-static singleton state. Init
 *   creates a SND_SEQ_OPEN_INPUT handle to "default", names the
 *   client "Stretto", creates one generic writable application
 *   port "Stretto Input", auto-subscribes via snd_seq_connect_from
 *   to either a single decoded (client<<8)|port address (explicit
 *   `synth --midi N`) or to every enumerated MIDI_GENERIC /
 *   MIDI_KEYBOARD port (wildcard `synth --midi`, N == -1), spawns
 *   one worker pthread that polls snd_seq_poll_descriptors_count for
 *   100 ms, drains any pending events via snd_seq_event_input,
 *   and re-enqueues each NOTEON/NOTEOFF/CONTROLLER into the SPSC
 *   ring via audio_midi_enqueue().
 *
 *   Auto-subscribe (vs the Phase-1+2 "wait for `aconnect`" pattern)
 *   is the actual spec T022 contract: spec asks the implementer to
 *   "subscribe to all clients/ports that send MIDI Keyboard events
 *   (via snd_seq_connect_from ... iterating the enumerate output)"
 *   which this implementation now honors. Per-port connect failures
 *   in wildcard mode are tolerated (already-subscribed / permission
 *   errors are not fatal for the whole wildcard); explicit-mode
 *   connect failures return -1 because the operator hand-picked that
 *   source and a misroute is operator-visible (FR-002).
 *
 *   All non-subscribed event types (active sensing, clock, sysex,
 *   etc.) fall through silently - queue slots are precious (256
 *   entries), so MIDI_EVENT_NONE is NOT enqueued for them.
 *
 *   ALSA uses 0..15 for channels; MIDI_EVENT_NOTE_ON.channel lives
 *   in the MIDI 1.0 1..16 domain, so each parser branch adds +1.
 *
 *   Cooperative shutdown: audio_midi_linux_close clears the atomic
 *   g_run flag, joins the pthread. The worker's loop body polls for
 *   100 ms then exits; full shutdown latency is one poll window
 *   with no self-pipe / spare fd required.
 *
 *   Idempotent + re-bind: same device_index is a no-op (worker keeps
 *   running, g_current_device_index unchanged). Different device_index
 *   tears down via audio_midi_linux_close() then re-opens. callers
 *   that genuinely want to swap devices between renders should call
 *   audio_midi_close() explicitly between calls (cleaner Lua-style
 *   lifecycle).
 *
 * FR-034 + Q3 (no auto-reconnect): disconnect during operation
 * (PORT_EXIT / CLIENT_EXIT) is handled in the worker; the
 * close-and-reopen-loop happens only on explicit reopen, NOT on
 * disconnect. After disconnect the synth continues playing from
 * internal generative state and CC-modulated parameters retain
 * their last value.
 *
 * Compile guard: this .o is in Makefile's OBJS but not WIN_OBJS so
 * Windows builds never see these symbols. The .c file pulls in
 * alsa/asoundlib.h + pthread.h + poll.h + string.h but only on
 * Linux. ci.yml installs libasound2-dev on stock Ubuntu runners so
 * the pkg-config gate in the Makefile becomes active (linker pulls
 * -lasound there) while dev boxes without libasound2-dev still
 * build partial artifacts (compile fails on missing header).
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
/* Last device_index passed in to audio_midi_linux_init. -1 means
 * either "wildcard" or "never initialized". Same value on a
 * subsequent open is a no-op (idempotency); different value
 * triggers close-then-reopen. main.c is expected to call
 * audio_midi_close() between renders but the close-and-rebind
 * defensive path catches tests that don't. */
static int        g_current_device_index = -1;

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
    /* FR-001 + FR-002: --midi [N] opens a specific enumerated device,
     * --midi-default is an alias for --midi 0. main.c passes -1 for
     * the wildcard / no-explicit-N case ("synth --midi" alone) which
     * auto-subscribes to every readable MIDI_GENERIC / MIDI_KEYBOARD
     * port. The decode (client<<8)|port mirrors audio_midi_linux_list_devices
     * (PR #108) so the enumerate output is directly bindable. */
    
    /* Idempotency + re-bind: same-N is a no-op (worker keeps running).
     * Different-N tears down via audio_midi_linux_close(); main.c is
     * expected to call audio_midi_close() between renders but doing
     * the defensive close-and-rebind here keeps test env's T034
     * round-trip (audio_midi_open(0) then audio_midi_open(1)) green
     * without tying the contract to a specific caller discipline. */
    if (g_seq != NULL && g_current_device_index == device_index) return 0;
    if (g_seq != NULL) audio_midi_linux_close();

    snd_seq_t *seq = NULL;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) return -1;
    if (snd_seq_set_client_name(seq, "Stretto") < 0) {
        snd_seq_close(seq);
        return -1;
    }
    int synth_port = snd_seq_create_simple_port(
        seq, "Stretto Input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_NO_EXPORT,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (synth_port < 0) {
        snd_seq_close(seq);
        return -1;
    }

    int sub_count = 0;
    if (device_index >= 0) {
        /* Explicit --midi N: decode (client<<8)|port (matching the
         * encode in audio_midi_linux_list_devices; identical to ALSA
         * addr convention used by amidi / aconnect). aconnect to a
         * single source. If the source port is no longer present
         * (e.g., the operator's USB controller was unplugged between
         * --midi-list-devices and --midi N), snd_seq_connect_from
         * fails and we return -1. Per FR-034 + Q3 no auto-reconnect
         * on disconnect: we cannot substitute the wildcard path
         * here because the operator picked that specific N
         * deliberately and a silent fallback to wildcard would
         * obscure an operator-visible misroute. */
        if (device_index == 0) {
            /* --midi 0 / --midi-default: raw index 0 decodes to ALSA
             * client 0 port 0 = the kernel's System Timer port, which
             * is never a MIDI device (connecting would deliver timer
             * queue events, not notes). Per FR-002's intent ("open
             * the default device"), 0 means the FIRST device from
             * --midi-list-devices; remap before decoding. */
            midi_input_device_t devs[MIDI_LIST_DEVICES_CAP];
            int32_t n = 0;
            if (audio_midi_linux_list_devices(devs, &n) != 0 || n == 0) {
                snd_seq_close(seq);
                return -1;
            }
            device_index = devs[0].index;
        }
        int src_client = (device_index >> 8) & 0xFF;
        int src_port   = device_index & 0xFF;
        if (snd_seq_connect_from(seq, synth_port, src_client, src_port) < 0) {
            snd_seq_close(seq);
            return -1;
        }
        sub_count = 1;
    } else {
        /* Wildcard --midi (no N). Iterate the same enumerate filter
         * (CAP_READ + MIDI_GENERIC) and aconnect to each matching
         * source. MIDI_GENERIC is the broadest alsa-lib category
         * (= "anything that doesn't fit a more-specific
         * SYNTHESIZER type"), which historically envelopes keyboards
         * AND generic controllers - the spec's intent for "MIDIO
         * Keyboard events" (research.md D1). The prior
         * MIDI_KEYBOARD filter hit a non-existent upstream alsa-lib
         * macro (SND_SEQ_PORT_TYPE_MIDI_KEYBOARD is NOT defined in
         * /usr/include/alsa/seq.h on stock Ubuntu; the constant was
         * either invented or copied from an old fork); the
         * MIDI_GENERIC-only filter is the correct upstream libasound
         * vocabulary and matches what --gc-sections + libasound2-dev
         * builds expect.
         *
         * Per-port connect failures (already-subscribed / permission
         * denied / source just unplugged) are tolerated and silently
         * dropped because the wildcard contract is "best-effort
         * subscribe to all readable MIDI controllers", not "every
         * individual source succeeds". Cap at MIDI_LIST_DEVICES_CAP
         * (defined in audio_midi.h) so the rarest studio-rig
         * scenarios (>32 controllers) don't run away. */
        snd_seq_client_info_t *cinfo;
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_client_info_set_client(cinfo, -1);   /* rewind iterator */
        int my_client = snd_seq_client_id(seq);
        while (snd_seq_query_next_client(seq, cinfo) >= 0) {
            int client = snd_seq_client_info_get_client(cinfo);
            if (client == my_client) continue;   /* skip Stretto itself */
            snd_seq_port_info_t *pinfo;
            snd_seq_port_info_alloca(&pinfo);
            snd_seq_port_info_set_client(pinfo, client);
            snd_seq_port_info_set_port(pinfo, -1);  /* rewind to first port */
            while (snd_seq_query_next_port(seq, pinfo) >= 0) {
                unsigned int caps = snd_seq_port_info_get_capability(pinfo);
                unsigned int type = snd_seq_port_info_get_type(pinfo);
                if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
                /* MIDI_GENERIC only - see the surrounding comment
                 * block for the lift from the invented
                 * MIDI_KEYBOARD alias (which is NOT in upstream
                 * alsa-lib). MIDI_GENERIC is what libasound's
                 * snd_seq_port_info_get_type() returns for USB-MIDI
                 * keyboards + class-compliant controllers AND any
                 * port that does not declare a more specific
                 * SYNTHESIZER type. The dual-filter (CAP_READ +
                 * MIDI_GENERIC) matches what `aconnect` would
                 * connect to by default. */
                if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
                int src_port = snd_seq_port_info_get_port(pinfo);
                /* Per-port connect errors are tolerated in wildcard mode. */
                snd_seq_connect_from(seq, synth_port, client, src_port);
                sub_count++;
                if (sub_count >= MIDI_LIST_DEVICES_CAP) break;
            }
            if (sub_count >= MIDI_LIST_DEVICES_CAP) break;
        }
    }

    if (sub_count == 0) {
        /* FR-002: when no MIDI device is connected, the synth exits
         * with a non-zero code. audio_midi_open's -1 return drives
         * main.c's "MIDI: device index N unavailable" error path
         * (and for wildcard N=-1, the same error fires when no
         * hardware is connected at all). */
        snd_seq_close(seq);
        return -1;
    }

    g_seq  = seq;
    g_port = synth_port;
    g_current_device_index = device_index;

    __atomic_store_n(&g_run, 1, __ATOMIC_RELEASE);
    if (pthread_create(&g_thread, NULL, midi_worker, NULL) != 0) {
        snd_seq_close(seq);
        g_seq  = NULL;
        g_port = -1;
        g_current_device_index = -1;
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
    /* T034, finally real (the long-standing stub returned -1 and made
       --midi-list-devices print nothing on every platform). Walks the
       sequencer with the SAME dual filter the wildcard open path uses
       (CAP_READ + MIDI_GENERIC), so what this lists is exactly what
       `--midi` would subscribe to. index is the (client<<8)|port
       encoding audio_midi_linux_init decodes for explicit --midi N -
       list output is directly bindable. name is "client port",
       truncated to the struct's 64 bytes. */
    *count = 0;
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0)
        return -1;   /* no sequencer (no snd-seq module / container) */

    snd_seq_client_info_t *cinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    int my_client = snd_seq_client_id(seq);
    while (snd_seq_query_next_client(seq, cinfo) >= 0
           && *count < MIDI_LIST_DEVICES_CAP) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == my_client) continue;
        const char *cname = snd_seq_client_info_get_name(cinfo);
        snd_seq_port_info_t *pinfo;
        snd_seq_port_info_alloca(&pinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0
               && *count < MIDI_LIST_DEVICES_CAP) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            unsigned int type = snd_seq_port_info_get_type(pinfo);
            if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
            if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
            int port = snd_seq_port_info_get_port(pinfo);
            midi_input_device_t *d = &out[*count];
            d->index = (int32_t)(((client & 0xFF) << 8) | (port & 0xFF));
            snprintf(d->name, sizeof d->name, "%s %s",
                     cname ? cname : "?",
                     snd_seq_port_info_get_name(pinfo));
            (*count)++;
        }
    }
    snd_seq_close(seq);
    return 0;
}
