#include "audio.h"
#include "mixer.h"
#include "ui.h"
#include "keys.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pulse/pulseaudio.h>

#define LATENCY_US     300000

/* Threaded-mainloop callbacks: each just signals the mainloop so
   the main thread can re-check stream/context state. */
static void pa_state_cb(pa_context *c, void *userdata) {
    (void)c;
    pa_threaded_mainloop_signal((pa_threaded_mainloop *)userdata, 0);
}
static void pa_stream_state_cb(pa_stream *s, void *userdata) {
    (void)s;
    pa_threaded_mainloop_signal((pa_threaded_mainloop *)userdata, 0);
}
static void pa_stream_write_cb(pa_stream *s, size_t length, void *userdata) {
    (void)s; (void)length;
    pa_threaded_mainloop_signal((pa_threaded_mainloop *)userdata, 0);
}

static void restore_terminal(void) {
    ui_term_restore_mode();
}

void audio_play(void) {
    /* --no-ui skips all terminal setup. Without the flag,
       ui_term_raw_mode itself degrades to headless when stdin or
       stdout is not a TTY (isatty check - parity with the Windows
       GetConsoleMode path), so redirected invocations run instead
       of dying on ENOTTY.

       atexit is registered BEFORE raw mode engages: restore_terminal
       no-ops until ui_term_raw_mode saves state, and registering
       first means the exit(1) paths inside raw-mode setup (fcntl
       failure after termios is already raw) still restore. The
       signal handlers cover the non-exit() deaths (Ctrl-C, SIGTERM,
       SIGQUIT, SIGHUP) that atexit can never see; they restore the
       terminal async-signal-safely and re-raise. Installed in
       --no-ui mode too: with no terminal state saved the handler is
       a plain re-raise, so behavior there is unchanged. */
    atexit(restore_terminal);
    if (!ui_get_no_ui()) {
        ui_term_raw_mode();
    }
    ui_install_signal_handlers();
    /* Initial resume-line snapshot (seed + any CLI-flag params) so a
       session killed before its first keypress - including headless
       --no-ui runs - is still recallable via the signal handler. */
    keys_build_resume_line();

    /* Full pa_stream API with a threaded mainloop, matching what
       paplay does. pa_simple's internal helper thread was getting
       under-scheduled by WSLg; running PA's event loop in our own
       pa_threaded_mainloop with INTERPOLATE_TIMING +
       AUTO_TIMING_UPDATE flags avoids that. */
    pa_threaded_mainloop *ml = pa_threaded_mainloop_new();
    if (!ml) { fprintf(stderr, "pa: mainloop alloc\n"); exit(1); }

    pa_context *ctx = pa_context_new(pa_threaded_mainloop_get_api(ml), "stretto");
    pa_context_set_state_callback(ctx, pa_state_cb, ml);

    if (pa_context_connect(ctx, NULL, 0, NULL) < 0) {
        fprintf(stderr, "pa: connect %s\n", pa_strerror(pa_context_errno(ctx)));
        exit(1);
    }
    if (pa_threaded_mainloop_start(ml) < 0) {
        fprintf(stderr, "pa: mainloop start\n");
        exit(1);
    }

    pa_threaded_mainloop_lock(ml);
    for (;;) {
        pa_context_state_t st = pa_context_get_state(ctx);
        if (st == PA_CONTEXT_READY) break;
        if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
            fprintf(stderr, "pa: context %s\n",
                    pa_strerror(pa_context_errno(ctx)));
            exit(1);
        }
        pa_threaded_mainloop_wait(ml);
    }

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = SAMPLE_RATE;
    ss.channels = 2;

    pa_stream *stream = pa_stream_new(ctx, "music", &ss, NULL);
    pa_stream_set_state_callback(stream, pa_stream_state_cb, ml);
    pa_stream_set_write_callback(stream, pa_stream_write_cb, ml);

    pa_buffer_attr ba;
    ba.maxlength = (uint32_t)-1;
    ba.tlength = (uint32_t)((uint64_t)SAMPLE_RATE * 4u * LATENCY_US / 1000000u);
    ba.prebuf = (uint32_t)-1;
    ba.minreq = (uint32_t)-1;
    ba.fragsize = (uint32_t)-1;

    if (pa_stream_connect_playback(stream, NULL, &ba,
            PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE,
            NULL, NULL) < 0) {
        fprintf(stderr, "pa: connect_playback %s\n",
                pa_strerror(pa_context_errno(ctx)));
        exit(1);
    }
    for (;;) {
        pa_stream_state_t sst = pa_stream_get_state(stream);
        if (sst == PA_STREAM_READY) break;
        if (sst == PA_STREAM_FAILED || sst == PA_STREAM_TERMINATED) {
            fprintf(stderr, "pa: stream %s\n",
                    pa_strerror(pa_context_errno(ctx)));
            exit(1);
        }
        pa_threaded_mainloop_wait(ml);
    }
    pa_threaded_mainloop_unlock(ml);

    int16_t *buf = arena_alloc(BUFFER_FRAMES * 2 * sizeof(int16_t));
    size_t buf_bytes = BUFFER_FRAMES * 2u * sizeof(int16_t);

    for (;;) {
        render_chunk(buf, BUFFER_FRAMES);

        pa_threaded_mainloop_lock(ml);
        while (pa_stream_writable_size(stream) < buf_bytes) {
            pa_threaded_mainloop_wait(ml);
        }
        if (pa_stream_write(stream, buf, buf_bytes, NULL, 0, PA_SEEK_RELATIVE) < 0) {
            pa_threaded_mainloop_unlock(ml);
            fprintf(stderr, "pa: write %s\n",
                    pa_strerror(pa_context_errno(ctx)));
            exit(1);
        }
        pa_threaded_mainloop_unlock(ml);

        if (!ui_get_no_ui() && !ui_help_visible()) {
            ui_draw_oscilloscope(buf, BUFFER_FRAMES);
        }

        char ch;
        while (ui_term_read_key(&ch)) {
            if (keys_dispatch(ch) == KEY_QUIT) {
                pa_threaded_mainloop_lock(ml);
                pa_operation *op = pa_stream_drain(stream, NULL, NULL);
                if (op) pa_operation_unref(op);
                pa_threaded_mainloop_unlock(ml);
                pa_threaded_mainloop_stop(ml);
                pa_stream_unref(stream);
                pa_context_disconnect(ctx);
                pa_context_unref(ctx);
                pa_threaded_mainloop_free(ml);
                /* Last-built snapshot = state at the user's last
                   action (no rebuild: post-keypress mutate() drift is
                   deliberately not captured). */
                fputs(ui_get_resume_line(), stderr);
                exit(0);   /* atexit runs restore_terminal */
            }
        }
    }
}
