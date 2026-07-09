#include "audio.h"
#include "mixer.h"
#include "ui.h"
#include "keys.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <mmsystem.h>

/* Four cycling buffers of BUFFER_FRAMES each (~21 ms at 48 kHz).
   Total buffered latency ~85 ms. CALLBACK_EVENT means the main
   thread waits on a single event handle for buffer completion -
   no callback function needed. */
#define WAVE_BUFFERS 4
static HWAVEOUT hwo;
static WAVEHDR  wave_hdrs[WAVE_BUFFERS];
static int16_t *wave_bufs[WAVE_BUFFERS];
static HANDLE   wave_event;

static void win_cleanup(void) {
    for (int i = 0; i < WAVE_BUFFERS; i++) {
        if (wave_hdrs[i].lpData) {
            waveOutUnprepareHeader(hwo, &wave_hdrs[i], sizeof(WAVEHDR));
        }
    }
    if (hwo) {
        waveOutReset(hwo);
        waveOutClose(hwo);
        hwo = NULL;
    }
    ui_term_restore_mode();
}

void audio_play(void) {
    if (!ui_get_no_ui()) {
        ui_term_raw_mode();
    }
    atexit(win_cleanup);
    /* Initial resume-line snapshot (seed + any CLI-flag params). No
       signal handlers on Windows - Ctrl-C arrives as keystroke 0x03
       through the 'q' path, which prints the line below; a hard
       console-kill in --no-ui mode prints nothing (documented). */
    keys_build_resume_line();

    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 2;
    wf.nSamplesPerSec  = SAMPLE_RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    wave_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!wave_event) {
        fprintf(stderr, "stretto: CreateEvent failed\n");
        exit(1);
    }

    MMRESULT mr = waveOutOpen(&hwo, WAVE_MAPPER, &wf,
                              (DWORD_PTR)wave_event, 0, CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "stretto: waveOutOpen failed (code %u)\n", (unsigned)mr);
        exit(1);
    }

    size_t buf_bytes = BUFFER_FRAMES * 2u * sizeof(int16_t);
    for (int i = 0; i < WAVE_BUFFERS; i++) {
        wave_bufs[i] = arena_alloc(buf_bytes);
        memset(&wave_hdrs[i], 0, sizeof(WAVEHDR));
        wave_hdrs[i].lpData         = (LPSTR)wave_bufs[i];
        wave_hdrs[i].dwBufferLength = (DWORD)buf_bytes;
        waveOutPrepareHeader(hwo, &wave_hdrs[i], sizeof(WAVEHDR));
    }

    /* Prime all four buffers and submit them so playback starts
       immediately. */
    for (int i = 0; i < WAVE_BUFFERS; i++) {
        render_chunk(wave_bufs[i], BUFFER_FRAMES);
        waveOutWrite(hwo, &wave_hdrs[i], sizeof(WAVEHDR));
    }

    int next = 0;
    for (;;) {
        while (!(wave_hdrs[next].dwFlags & WHDR_DONE)) {
            WaitForSingleObject(wave_event, INFINITE);
        }
        render_chunk(wave_bufs[next], BUFFER_FRAMES);
        waveOutWrite(hwo, &wave_hdrs[next], sizeof(WAVEHDR));
        next = (next + 1) % WAVE_BUFFERS;

        if (!ui_get_no_ui() && !ui_help_visible()) {
            ui_draw_oscilloscope(wave_bufs[next], BUFFER_FRAMES);
        }

        char ch;
        while (ui_term_read_key(&ch)) {
            if (keys_dispatch(ch) == KEY_QUIT) {
                win_cleanup();
                /* Last-built snapshot = state at the user's last
                   action; covers interactive Ctrl-C too (arrives as
                   keystroke 0x03 through this path). */
                fputs(ui_get_resume_line(), stderr);
                exit(0);
            }
        }
    }
}
