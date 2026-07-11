/* audio_midi_winmm.c - Win32 midiInProc backend (US1 / T023).
 *
 * Phase 1+2 stub returned -1 from init; US1 replaces it with a real
 * midiIn callback installed via midiInOpen(CALLBACK_FUNCTION), per
 * spec/003-midi-input/research.md D2 (Win32 MIDI input is callback-
 * only - there is no blocking-read or polling API equivalent to
 * ALSA's sequencer). The callback runs on a system-managed thread
 * created by midiInStart(); we enqueue each parsed event into the
 * same SPSC ring the Linux pthread backend uses.
 *
 * Backend design:
 *   HMIDIIN g_hin, uint32_t g_run (atomic) - module-static
 *   singleton state. Init opens a midiIn handle via midiInOpen
 *   targeting device_index (or WAVE_MAPPER for default), installs
 *   the MidiInProc callback with CALLBACK_FUNCTION, then prepares
 *   and adds a 4-buffer pool of MIDIHDR sysex buffers with
 *   midiInPrepareHeader + midiInAddBuffer. Each MIM_DATA callback
 *   parses the short message (status | data1<<8 | data2<<16) into
 *   a midi_event_t and enqueues via audio_midi_enqueue(). Each
 *   MIM_LONGDATA callback receives a sysex payload (re-add the
 *   buffer to keep the pool alive for the next sysex). MIM_CLOSE
 *   signals FR-034 graceful disconnect.
 *
 *   Win32 MIDI uses 0..15 channels (low nibble of status byte);
 *   MIDI_EVENT.channel lives in the MIDI 1.0 1..16 domain, so
 *   each parser branch adds +1.
 *
 *   Cooperative shutdown: audio_midi_winmm_close flips the atomic
 *   g_run flag, then calls midiInStop -> midiInReset -> unprepare
 *   all 4 sysex hdrs -> midiInClose in that order. midiInClose
 *   blocks until the callback finishes processing the close per
 *   MSDN, so the audio thread's teardown does not race against an
 *   in-callback enqueue. There is no pthread_join because winmm
 *   owns the service thread - the atomic g_run is a defensive
 *   guard for any callback the close sequence cannot cancel (e.g.
 *   a MIM_LONGDATA that was in-flight when Reset was called; we
 *   skip re-add so the pool is drained back to idle).
 *
 * Compile guard: this .o is in Makefile's WIN_OBJS but not OBJS so
 * Linux builds never see any winmm symbols. The .c file pulls in
 * <windows.h> + <mmsystem.h> which CI's stock ubuntu runner cannot
 * satisfy on its own - WIN_OBJS is compiled via the cross-tool
 * x86_64-w64-mingw32-gcc (Makefile:55), and the link already pulls
 * -lwinmm because audio_winmm.c uses waveOut* (Makefile:63).
 */
#include "audio_midi.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <mmsystem.h>  /* midiInOpen / midiInProc / midiInGetDevCaps / MIDIHDR */
#include <stdint.h>
#include <string.h>    /* memset for MIDIHDR zero-init before PrepareHeader */

/* 4096 B is the canonical sysex buffer size - big enough for any
   standard vendor sysex (Yamaha, AKAI, Korg) without bloating the
   binary. Four buffers gives enough headroom to not lose incoming
   sysex under bursty producer load; the SPSC ring is 256 entries
   so we're not trying to ferry sysex through the ring anyway
   (sysex consumes too many queue slots for the v1 feature set -
   US3 is when we wire separate sysex_queue). For now MIM_LONGDATA
   is acknowledged (re-added) but the payload contents are
   discarded; same drop policy as audio_midi_linux.c's
   "default: continue" for non-NOTEON/NOTEOFF/CONTROLLER. */
#define WINMM_SYSEX_BUFFERS 4
#define WINMM_SYSEX_SIZE    4096

static HMIDIIN g_hin = NULL;
/* atomic: cleared by audio_midi_winmm_close AND by the MIM_CLOSE
 * code path; callbacks acquire-load via __atomic_load_n /
 * __ATOMIC_ACQUIRE; shutdown writes via __atomic_store_n /
 * __ATOMIC_RELEASE. Pairing honors the C11 acquire/release model
 * in audio_midi.c's research.md D3. Same pattern as
 * audio_midi_linux.c's g_run, no `volatile` (the atomics already
 * supply the memory model; `volatile` is not a synchronization
 * primitive per C99 §6.7.3 and would only mislead future readers
 * into thinking plain volatile suffices for cross-thread access). */
static uint32_t g_run = 0u;
/* Last device_index passed in to audio_midi_winmm_init. -1 means
 * "WAVE_MAPPER" (default) or "never initialized". Same value on a
 * subsequent open is a no-op (idempotency); different value triggers
 * close-then-reopen so a fresh g_hin is opened against the
 * newly-selected device. */
static int      g_current_device_index = -1;

static MIDIHDR   g_sysex_hdrs [WINMM_SYSEX_BUFFERS];
static uint8_t   g_sysex_bufs [WINMM_SYSEX_BUFFERS][WINMM_SYSEX_SIZE];
static int       g_sysex_in_use = 0;  /* flips to 1 once midInPrepareHeader succeeded */

static void midiInProc(HMIDIIN hMidiIn, UINT wMsg,
                       DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    (void)hMidiIn;
    (void)dwInstance;
    (void)dwParam2;
    if (!__atomic_load_n(&g_run, __ATOMIC_ACQUIRE)) return;

    switch (wMsg) {
        case MIM_DATA: {
            /* dwParam1 = short message:
                 bits  0..7  = status
                 bits  8..15 = data1 (key / CC#)
                 bits 16..23 = data2 (velocity / value)
               dwParam2 = timestamp in ms (ignored - we use the
               audio thread's audio_midi_drain timing instead). */
            uint32_t raw = (uint32_t)dwParam1;
            uint8_t  status  = (uint8_t)(raw & 0xFFu);
            /* Data bytes are 7-bit by the MIDI spec; mask 0x7F (not
               0xFF) so a misbehaving driver can never hand the drain
               a CC number >= 128 (CC_MAP has exactly 128 entries). */
            uint8_t  data1   = (uint8_t)((raw >> 8)  & 0x7Fu);
            uint8_t  data2   = (uint8_t)((raw >> 16) & 0x7Fu);

            /* System realtime / common (0xF0..0xFF) are ignored -
               they arrive via MIM_LONGDATA for sysex, never in
               MIM_DATA; clock + start/stop/continue are not
               handled in v1. */
            if (status >= 0xF0u) return;

            midi_event_t qev = {
                .type    = MIDI_EVENT_NONE,
                .channel = 0,
                .key     = 0,
                .value   = 0
            };
            switch (status & 0xF0u) {
                case 0x80u:  /* Note Off */
                    qev.type    = MIDI_EVENT_NOTE_OFF;
                    qev.channel = (uint8_t)((status & 0x0Fu) + 1u);
                    qev.key     = data1;
                    qev.value   = data2;
                    break;
                case 0x90u:  /* Note On - V=0 -> NoteOff conversion happens
                                * in audio_midi.c's drain (FR-011), so
                                * we pass the raw velocity here. Same as
                                * audio_midi_linux.c's SND_SEQ_EVENT_NOTEON
                                * branch (no conversion at enqueue). */
                    qev.type    = MIDI_EVENT_NOTE_ON;
                    qev.channel = (uint8_t)((status & 0x0Fu) + 1u);
                    qev.key     = data1;
                    qev.value   = data2;
                    break;
                case 0xB0u:  /* Control Change */
                    qev.type    = MIDI_EVENT_CC;
                    qev.channel = (uint8_t)((status & 0x0Fu) + 1u);
                    qev.key     = data1;
                    qev.value   = data2;
                    break;
                default:
                    /* Poly AT (0xA0) / Program Change (0xC0) /
                       Channel Pressure (0xD0) / Pitch Bend (0xE0)
                       fall through; do NOT enqueue MIDI_EVENT_NONE
                       (queue slots are precious). */
                    return;
            }
            audio_midi_enqueue(&qev);
            break;
        }

        case MIM_LONGDATA: {
            /* dwParam1 = LPMIDIHDR pointer. The MHDR_DONE flag is
               set on the header - we clear it + re-prepare + re-add
               so the pool keeps delivering sysex. Skip re-add if
               we're tearing down so Close can finish cleanly
               without recursing into Prepare across a closed
               handle. (Re-adding during a close sequence would
               invoke midiInAddBuffer with an unsafe handle and
               could deadlock inside winmm.) */
            LPMIDIHDR hdr = (LPMIDIHDR)dwParam1;
            if (hdr == NULL) return;
            if (!__atomic_load_n(&g_run, __ATOMIC_ACQUIRE)) return;

            midiInUnprepareHeader(g_hin, hdr, sizeof(MIDIHDR));
            midiInPrepareHeader(g_hin, hdr, sizeof(MIDIHDR));
            midiInAddBuffer(g_hin, hdr, sizeof(MIDIHDR));
            break;
        }

        case MIM_CLOSE:
            /* FR-034 graceful disconnect: the device or app that
               owned the MIDI input handle signaled close (e.g. user
               ejected the USB MIDI controller, app shutdown).
               Flip g_run so any in-flight callback wakes into the
               return path; audio_midi_winmm_close then completes
               the teardown via midiInClose. Do NOT call
               audio_midi_close from inside the callback - that's
               the same recursion deadlock as the ALSA backend
               (research.md D1 notes pthread_join-on-self UB; this
               is the Win32 equivalent). */
            __atomic_store_n(&g_run, 0u, __ATOMIC_RELEASE);
            break;

        case MIM_ERROR:
        case MIM_LONGERROR:
            /* Input overrun / sysex overrun. The buffers are full;
               the device will drop further events until drain. No
               programmatic recovery needed - the soft-rate overflow
               in audio_midi_enqueue already counts ring drops, and
               winmm drops the input events itself. */
            break;

        default:
            break;
    }
}

int audio_midi_winmm_init(int device_index) {
    /* Idempotent + re-bind: same device_index is a no-op (g_hin stays
     * open, g_current_device_index unchanged). Different device_index
     * tears down via audio_midi_winmm_close() before re-opening.
     * main.c is expected to call audio_midi_close() between renders
     * but the defensive close-and-rebind here keeps the contract
     * usable for tests that don't (e.g. T034 round-trip env-flex
     * calls audio_midi_open(0) then audio_midi_open(1)). */
    if (g_hin != NULL && g_current_device_index == device_index) return 0;
    if (g_hin != NULL) audio_midi_winmm_close();

    /* device_index >= 0 is the raw device id (dense ordinal, same as
     * --midi-list-devices output). device_index < 0 (wildcard --midi)
     * opens the FIRST device: Win32 has no MIDI *input* mapper -
     * WAVE_MAPPER/MIDI_MAPPER exist for output only, and
     * midiInOpen((UINT)-1) fails with MMSYSERR_BADDEVICEID even with
     * hardware attached (the old code did exactly that). One handle =
     * one device on this backend, so Windows wildcard is
     * "first available device", not Linux's subscribe-to-all;
     * documented in --help / the man page. */
    UINT devid;
    if (device_index < 0) {
        if (midiInGetNumDevs() == 0) return -1;
        devid = 0;
    } else {
        UINT n = midiInGetNumDevs();
        if ((UINT)device_index >= n) return -1;
        devid = (UINT)device_index;
    }

    MMRESULT mr = midiInOpen(&g_hin, devid,
                             (DWORD_PTR)midiInProc,
                             (DWORD_PTR)NULL,
                             CALLBACK_FUNCTION);
    if (mr != MMSYSERR_NOERROR) return -1;

    /* Prepare + add the 4 sysex buffers. Each callback replenishes
       its buffer in the MIM_LONGDATA branch (re-prepare + re-add
       after MHDR_DONE). */
    for (int i = 0; i < WINMM_SYSEX_BUFFERS; i++) {
        memset(&g_sysex_hdrs[i], 0, sizeof(MIDIHDR));
        g_sysex_hdrs[i].lpData         = (LPSTR)g_sysex_bufs[i];
        g_sysex_hdrs[i].dwBufferLength = WINMM_SYSEX_SIZE;
        if (midiInPrepareHeader(g_hin, &g_sysex_hdrs[i], sizeof(MIDIHDR))
                != MMSYSERR_NOERROR) {
            /* Roll back what we've prepared so far + close. */
            for (int j = 0; j < i; j++) {
                midiInUnprepareHeader(g_hin, &g_sysex_hdrs[j], sizeof(MIDIHDR));
            }
            midiInClose(g_hin);
            g_hin = NULL;
            return -1;
        }
        if (midiInAddBuffer(g_hin, &g_sysex_hdrs[i], sizeof(MIDIHDR))
                != MMSYSERR_NOERROR) {
            midiInUnprepareHeader(g_hin, &g_sysex_hdrs[i], sizeof(MIDIHDR));
            for (int j = 0; j < i; j++) {
                midiInUnprepareHeader(g_hin, &g_sysex_hdrs[j], sizeof(MIDIHDR));
            }
            midiInClose(g_hin);
            g_hin = NULL;
            return -1;
        }
    }
    g_sysex_in_use = 1;

    __atomic_store_n(&g_run, 1u, __ATOMIC_RELEASE);
    if (midiInStart(g_hin) != MMSYSERR_NOERROR) {
        __atomic_store_n(&g_run, 0u, __ATOMIC_RELEASE);
        for (int i = 0; i < WINMM_SYSEX_BUFFERS; i++) {
            midiInUnprepareHeader(g_hin, &g_sysex_hdrs[i], sizeof(MIDIHDR));
        }
        midiInClose(g_hin);
        g_hin = NULL;
        g_sysex_in_use = 0;
        return -1;
    }
    g_current_device_index = device_index;
    return 0;
}

void audio_midi_winmm_close(void) {
    if (g_hin == NULL) return;
    /* Order matters per MSDN + audio_midi_linux.c's symmetric
       comment: flip g_run first (defensive against any callback
       still firing during the close sequence), then midiInStop to
       halt input flow, then midiInReset to receive the MIM_LONGDATA
       callbacks with MHDR_DONE that we ignore (g_run == 0 short-
       circuits the re-add), then unprepare the static BSS buffers
       so they can be re-prepared on the next init cycle, then
       midiInClose which blocks until the service thread has
       returned from any in-flight callback. */
    __atomic_store_n(&g_run, 0u, __ATOMIC_RELEASE);
    midiInStop(g_hin);
    midiInReset(g_hin);
    if (g_sysex_in_use) {
        for (int i = 0; i < WINMM_SYSEX_BUFFERS; i++) {
            midiInUnprepareHeader(g_hin, &g_sysex_hdrs[i], sizeof(MIDIHDR));
        }
        g_sysex_in_use = 0;
    }
    midiInClose(g_hin);
    g_hin = NULL;
}

int audio_midi_winmm_list_devices(midi_input_device_t *out, int32_t *count) {
    (void)out; (void)count;
    /* Implemented in T035 (midiInGetNumDevs +
       midiInGetDevCaps + name truncation to 63 chars); T023 keeps
       the stub so re-builds without the cross-tool still link
       cleanly via --gc-sections. */
    return -1;
}

#endif
