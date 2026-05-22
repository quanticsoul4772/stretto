#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <pulse/pulseaudio.h>
#else
#include <windows.h>
#include <mmsystem.h>
#endif

#include "arena.h"
#include "voice.h"
#include "gen.h"

#define SAMPLE_RATE    48000
#define BUFFER_FRAMES  1024
/* 300 ms playback latency. WSLg's libasound -> pulse-plugin chain
   crackles at sustained 48 kHz output; using libpulse-simple
   directly (same path paplay uses) is clean. This latency value is
   passed to pulse as the target buffer length. */
#define LATENCY_US     300000

/* Master-bus stereo delay. Two independent mono buffers (one per
   channel), 250 ms long at 44.1 kHz. Standard feed-forward + feedback
   topology: output = dry + tap*wet; delay-buffer-write = dry + tap*fb. */
#define DELAY_SAMPLES  12000u   /* 250 ms at 48 kHz */
static int16_t *delay_l;
static int16_t *delay_r;
static uint32_t delay_idx = 0;
static uint16_t delay_wet      = 100;  /* 0..256, mix amount */
static uint16_t delay_feedback = 140;  /* 0..200, capped to avoid runaway */

/* Schroeder reverb: 4 parallel comb filters whose outputs are summed
   and then passed through 2 series all-pass filters per channel. The
   classic prime-number delay choices (Schroeder 1962) avoid metallic
   resonance. L and R channels use slightly different delays so the
   reverb tail keeps stereo separation. */
/* Schroeder primes rescaled by 48000/44100 to preserve original
   reverb delay times at the new sample rate. All values are primes
   so the four combs and two all-passes remain coprime and the
   reverb tail stays free of metallic resonance. */
#define REV_C1L 1693
#define REV_C2L 1759
#define REV_C3L 1621
#define REV_C4L 1549
#define REV_C1R 1721
#define REV_C2R 1747
#define REV_C3R 1613
#define REV_C4R 1571
#define REV_AP1L 241
#define REV_AP2L 607
#define REV_AP1R 251
#define REV_AP2R 613

static int16_t *rev_c1l, *rev_c2l, *rev_c3l, *rev_c4l;
static int16_t *rev_c1r, *rev_c2r, *rev_c3r, *rev_c4r;
static int16_t *rev_ap1l, *rev_ap2l;
static int16_t *rev_ap1r, *rev_ap2r;
static uint16_t i_c1l, i_c2l, i_c3l, i_c4l;
static uint16_t i_c1r, i_c2r, i_c3r, i_c4r;
static uint16_t i_ap1l, i_ap2l, i_ap1r, i_ap2r;

static uint16_t reverb_wet = 60;   /* 0..256, mix amount */
#define COMB_G 180                 /* ~0.70 in 8.8 fixed, RT60 ~1.5 s */
#define AP_G   180                 /* ~0.70 */

#ifndef _WIN32
#define TIOCGWINSZ     0x5413

static struct termios saved_termios;
static int termios_saved = 0;
#endif
static int help_visible = 0;
static int no_ui = 0;

#ifndef _WIN32
/* Terminal UI helpers (Unix-only - use write() directly). */
static const char HELP_TEXT[] =
    "\x1b[H\x1b[2J"
    "  stretto keys\r\n"
    "  ------------\r\n"
    "  SPACE  force mutation now\r\n"
    "  +  /  -    tempo  faster / slower\r\n"
    "  [  /  ]    FM mod_depth  down / up\r\n"
    "  s          cycle scale  D L P l H M\r\n"
    "  g  /  G    gate probability  down / up\r\n"
    "  d  /  D    delay wet mix  down / up\r\n"
    "  f  /  F    delay feedback  down / up\r\n"
    "  r  /  R    reverb wet mix  down / up\r\n"
    "  ?          toggle this help\r\n"
    "  q          quit\r\n"
    "\r\n"
    "  (any key dismisses)\r\n";

static void show_help(void) {
    (void)!write(1, HELP_TEXT, sizeof(HELP_TEXT) - 1);
}

static void clear_screen(void) {
    (void)!write(1, "\x1b[H\x1b[2J", 7);
}
#endif  /* terminal UI helpers */

static inline int16_t sat16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void delay_init(void) {
    delay_l = arena_alloc(DELAY_SAMPLES * sizeof(int16_t));
    delay_r = arena_alloc(DELAY_SAMPLES * sizeof(int16_t));
    /* arena_alloc does not zero; clear so the first echoes are silence. */
    memset(delay_l, 0, DELAY_SAMPLES * sizeof(int16_t));
    memset(delay_r, 0, DELAY_SAMPLES * sizeof(int16_t));
    delay_idx = 0;
}

static int16_t *alloc_zero(uint32_t n_samples) {
    int16_t *p = arena_alloc(n_samples * sizeof(int16_t));
    memset(p, 0, n_samples * sizeof(int16_t));
    return p;
}

static void reverb_init(void) {
    rev_c1l = alloc_zero(REV_C1L);  rev_c1r = alloc_zero(REV_C1R);
    rev_c2l = alloc_zero(REV_C2L);  rev_c2r = alloc_zero(REV_C2R);
    rev_c3l = alloc_zero(REV_C3L);  rev_c3r = alloc_zero(REV_C3R);
    rev_c4l = alloc_zero(REV_C4L);  rev_c4r = alloc_zero(REV_C4R);
    rev_ap1l = alloc_zero(REV_AP1L); rev_ap1r = alloc_zero(REV_AP1R);
    rev_ap2l = alloc_zero(REV_AP2L); rev_ap2r = alloc_zero(REV_AP2R);
    i_c1l = i_c2l = i_c3l = i_c4l = 0;
    i_c1r = i_c2r = i_c3r = i_c4r = 0;
    i_ap1l = i_ap2l = i_ap1r = i_ap2r = 0;
}

/* Comb filter step. y[n] = x[n-D] + g * y[n-D], using delay-line as
   recirculating buffer. Output is the tap (delayed input), buffer
   write is input + tap*g. */
static inline int16_t comb_step(int16_t *buf, uint16_t size, uint16_t *idx, int16_t in) {
    int32_t tap = buf[*idx];
    int32_t w = in + ((tap * COMB_G) >> 8);
    if (w >  32767) w =  32767;
    if (w < -32768) w = -32768;
    buf[*idx] = (int16_t)w;
    *idx = (uint16_t)((*idx + 1) % size);
    return (int16_t)tap;
}

/* All-pass filter step. Output passes the same magnitude as input at
   all frequencies but shifts phases; smooths the dense reflections
   from the parallel combs into a continuous tail. */
static inline int16_t ap_step(int16_t *buf, uint16_t size, uint16_t *idx, int16_t in) {
    int32_t tap = buf[*idx];
    int32_t y = tap - ((in * AP_G) >> 8);
    int32_t w = in + ((tap * AP_G) >> 8);
    if (y >  32767) y =  32767;
    if (y < -32768) y = -32768;
    if (w >  32767) w =  32767;
    if (w < -32768) w = -32768;
    buf[*idx] = (int16_t)w;
    *idx = (uint16_t)((*idx + 1) % size);
    return (int16_t)y;
}

static void reverb_process(int16_t *buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int16_t in_l = buf[2 * i];
        int16_t in_r = buf[2 * i + 1];

        /* 4 parallel combs, averaged. */
        int32_t sum_l = (int32_t)comb_step(rev_c1l, REV_C1L, &i_c1l, in_l)
                      + comb_step(rev_c2l, REV_C2L, &i_c2l, in_l)
                      + comb_step(rev_c3l, REV_C3L, &i_c3l, in_l)
                      + comb_step(rev_c4l, REV_C4L, &i_c4l, in_l);
        sum_l >>= 2;
        int32_t sum_r = (int32_t)comb_step(rev_c1r, REV_C1R, &i_c1r, in_r)
                      + comb_step(rev_c2r, REV_C2R, &i_c2r, in_r)
                      + comb_step(rev_c3r, REV_C3R, &i_c3r, in_r)
                      + comb_step(rev_c4r, REV_C4R, &i_c4r, in_r);
        sum_r >>= 2;

        /* 2 series all-passes per channel. */
        int16_t ap_l = ap_step(rev_ap1l, REV_AP1L, &i_ap1l, (int16_t)sum_l);
                ap_l = ap_step(rev_ap2l, REV_AP2L, &i_ap2l, ap_l);
        int16_t ap_r = ap_step(rev_ap1r, REV_AP1R, &i_ap1r, (int16_t)sum_r);
                ap_r = ap_step(rev_ap2r, REV_AP2R, &i_ap2r, ap_r);

        /* Mix wet onto dry. */
        int32_t out_l = in_l + ((ap_l * (int32_t)reverb_wet) >> 8);
        int32_t out_r = in_r + ((ap_r * (int32_t)reverb_wet) >> 8);
        buf[2 * i]     = sat16(out_l);
        buf[2 * i + 1] = sat16(out_r);
    }
}

static void reverb_adjust_wet(int delta) {
    int v = (int)reverb_wet + delta;
    if (v < 0)   v = 0;
    if (v > 256) v = 256;
    reverb_wet = (uint16_t)v;
}

/* Soft cubic saturation: y = x - x^3 / 2^31. Linear for small x,
   compresses peaks smoothly. At full-scale int16 input (~32767) the
   output is ~50%; at typical signal levels (10-20% of full scale)
   the change is sub-1%. Adds gentle analog-tape-like warmth to
   peaks without affecting the quiet-signal character. */
static inline int16_t soft_sat(int16_t x) {
    int64_t x3 = (int64_t)x * x * x;
    int32_t cubic = (int32_t)(x3 >> 31);
    int32_t y = (int32_t)x - cubic;
    return sat16(y);
}

static void saturate_process(int16_t *buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        buf[2 * i]     = soft_sat(buf[2 * i]);
        buf[2 * i + 1] = soft_sat(buf[2 * i + 1]);
    }
}

/* In-place stereo delay processing on an interleaved L,R,L,R buffer. */
static void delay_process(int16_t *buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int32_t dry_l = buf[2 * i];
        int32_t dry_r = buf[2 * i + 1];
        int32_t tap_l = delay_l[delay_idx];
        int32_t tap_r = delay_r[delay_idx];

        int32_t out_l = dry_l + ((tap_l * (int32_t)delay_wet) >> 8);
        int32_t out_r = dry_r + ((tap_r * (int32_t)delay_wet) >> 8);

        int32_t fb_l = dry_l + ((tap_l * (int32_t)delay_feedback) >> 8);
        int32_t fb_r = dry_r + ((tap_r * (int32_t)delay_feedback) >> 8);

        delay_l[delay_idx] = sat16(fb_l);
        delay_r[delay_idx] = sat16(fb_r);

        if (++delay_idx >= DELAY_SAMPLES) delay_idx = 0;

        buf[2 * i]     = sat16(out_l);
        buf[2 * i + 1] = sat16(out_r);
    }
}

static void delay_adjust_wet(int delta) {
    int v = (int)delay_wet + delta;
    if (v < 0)   v = 0;
    if (v > 256) v = 256;
    delay_wet = (uint16_t)v;
}

static void delay_adjust_feedback(int delta) {
    int v = (int)delay_feedback + delta;
    if (v < 0)   v = 0;
    if (v > 200) v = 200;     /* cap to avoid feedback runaway */
    delay_feedback = (uint16_t)v;
}

/* Fills 2*frames int16 samples in interleaved L,R,L,R,... order, with
   the master-bus effects chain applied in place:
     voice mix -> reverb -> delay -> soft saturation -> output
   Saturation last so any peaks that reverb or delay add are smoothed
   before going to the device. */
static void render_chunk(int16_t *out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        gen_step();
        Stereo s = voice_pool_mix();
        out[2 * i]     = s.l;
        out[2 * i + 1] = s.r;
    }
    reverb_process(out, frames);
    delay_process(out, frames);
    saturate_process(out, frames);
}

#ifndef _WIN32
static void restore_terminal(void) {
    if (termios_saved) {
        tcsetattr(0, TCSANOW, &saved_termios);
        (void)!write(1, "\x1b[?25h\n", 8);
    }
}

/* Build the entire frame (status row + oscilloscope grid) into one
   buffer and write it with a single write() syscall. Previously we
   issued ~3 + 3*h syscalls per frame (~75 at h=24); under WSLg's
   RDP-based terminal each one can stall, accumulating enough delay
   to underrun the PulseAudio buffer. One write per frame keeps the
   audio loop unblocked. */
static void draw_oscilloscope(int16_t *buf, uint32_t frames) {
    unsigned short ws[4];
    ws[0] = 24; ws[1] = 80;
    ioctl(1, TIOCGWINSZ, ws);
    if (ws[1] == 0) ws[1] = 80;
    if (ws[0] == 0) ws[0] = 24;

    uint32_t w = ws[1] > 120 ? 120 : ws[1];
    uint32_t h = ws[0] > 2 ? ws[0] - 2 : 22;
    if (w > frames) w = frames;

    static char out[4096];
    int p = 0;
    /* Cursor home + hide + clear line */
    out[p++] = 0x1b; out[p++] = '['; out[p++] = 'H';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '?'; out[p++] = '2'; out[p++] = '5'; out[p++] = 'l';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '2'; out[p++] = 'K';

    /* Status row: M:<mod> S:<scale> V:<voices> */
    uint32_t mask = voice_pool_active_mask();
    out[p++] = 'M'; out[p++] = ':';
    unsigned v = voice_get_mod_depth();
    char t[6]; int n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n > 0) out[p++] = t[--n];
    out[p++] = ' '; out[p++] = 'S'; out[p++] = ':';
    /* D=Dorian L=Lydian P=Phrygian l=Locrian H=Harmonic-minor M=Mixolydian */
    out[p++] = "DLPlHM"[gen_get_scale() % 6];
    out[p++] = ' '; out[p++] = 'V'; out[p++] = ':';
    for (int i = 0; i < N_VOICES; i++) out[p++] = (mask & (1u << i)) ? '*' : '.';
    out[p++] = '\r'; out[p++] = '\n';

    /* Oscilloscope grid: L channel, ASCII intensity per cell. */
    for (uint32_t r = 0; r < h; r++) {
        out[p++] = 0x1b; out[p++] = '['; out[p++] = '2'; out[p++] = 'K';
        int32_t thresh = 8000 - (int32_t)((r * 16000u) / h);
        for (uint32_t c = 0; c < w; c++) {
            int16_t samp = buf[2 * ((c * frames) / w)];
            int32_t a = samp < 0 ? -samp : samp;
            out[p++] = (a > thresh) ? (a > 7000 ? '@' : a > 5000 ? '#' : a > 3500 ? '*' : a > 2500 ? '+' : a > 1500 ? '-' : '.') : ' ';
        }
        if (r < h - 1) { out[p++] = '\r'; out[p++] = '\n'; }
        if (p > (int)sizeof(out) - 256) break;  /* defensive */
    }
    (void)!write(1, out, (size_t)p);
}
#endif  /* restore_terminal + draw_oscilloscope */

static void write_wav_header(FILE *f, uint32_t num_samples) {
    /* num_samples is per-channel frame count; stereo writes 4 bytes
       per frame (2 channels * 2 bytes). */
    uint32_t data_size   = num_samples * 4u;
    uint32_t file_size   = data_size + 36u;
    uint32_t fmt_size    = 16u;
    uint16_t audio_fmt   = 1u;
    uint16_t n_chan      = 2u;
    uint32_t sample_rate = SAMPLE_RATE;
    uint32_t byte_rate   = SAMPLE_RATE * 4u;
    uint16_t block_align = 4u;
    uint16_t bits        = 16u;

    fwrite("RIFF",     1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size,  4, 1, f);
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&n_chan,    2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits,      2, 1, f);
    fwrite("data",     1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

static void render_wav(int seconds, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "open %s\n", path);
        exit(1);
    }
    uint32_t total = (uint32_t)SAMPLE_RATE * (uint32_t)seconds;
    write_wav_header(f, total);
    int16_t *buf = arena_alloc(BUFFER_FRAMES * 2 * sizeof(int16_t));
    uint32_t remaining = total;
    while (remaining > 0) {
        uint32_t n = remaining > BUFFER_FRAMES ? BUFFER_FRAMES : remaining;
        render_chunk(buf, n);
        fwrite(buf, 2, n * 2, f);
        remaining -= n;
    }
    fclose(f);
}

#ifndef _WIN32
/* Threaded-mainloop pa_stream callbacks: each just wakes the
   condition variable so the main thread can re-check stream state
   or writable-size in its wait loops. */
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

static void play_pulse(void) {
    if (tcgetattr(0, &saved_termios) < 0) {
        fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
        exit(1);
    }
    termios_saved = 1;
    atexit(restore_terminal);

    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) < 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        exit(1);
    }

    int flags = fcntl(0, F_GETFL, 0);
    if (flags < 0 || fcntl(0, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "fcntl: %s\n", strerror(errno));
        exit(1);
    }

    /* Full pa_stream API with a threaded mainloop, matching what
       paplay does. The internal helper thread used by pa_simple was
       getting under-scheduled by WSLg; running the PA event loop in
       our own pa_threaded_mainloop with INTERPOLATE_TIMING +
       AUTO_TIMING_UPDATE flags avoids that and matches paplay's
       behaviour exactly. */
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

        if (!no_ui && !help_visible) draw_oscilloscope(buf, BUFFER_FRAMES);

        char ch;
        while (read(0, &ch, 1) > 0) {
            if (ch == '?') {
                help_visible = !help_visible;
                if (help_visible) show_help();
                else clear_screen();
                continue;
            }
            if (help_visible) {
                help_visible = 0;
                clear_screen();
            }
            if (ch == ' ') gen_force_mutate();
            else if (ch == '+') gen_set_tempo(-10);
            else if (ch == '-') gen_set_tempo(+10);
            else if (ch == '[') {
                uint16_t d = voice_get_mod_depth();
                voice_set_mod_depth(d > 200 ? d - 200 : 100);
            }
            else if (ch == ']') {
                voice_set_mod_depth(voice_get_mod_depth() + 200);
            }
            else if (ch == 's') gen_cycle_scale();
            else if (ch == 'g') gen_adjust_gate(-16);
            else if (ch == 'G') gen_adjust_gate(+16);
            else if (ch == 'd') delay_adjust_wet(-16);
            else if (ch == 'D') delay_adjust_wet(+16);
            else if (ch == 'f') delay_adjust_feedback(-16);
            else if (ch == 'F') delay_adjust_feedback(+16);
            else if (ch == 'r') reverb_adjust_wet(-16);
            else if (ch == 'R') reverb_adjust_wet(+16);
            else if (ch == 'q') {
                pa_threaded_mainloop_lock(ml);
                pa_operation *op = pa_stream_drain(stream, NULL, NULL);
                if (op) pa_operation_unref(op);
                pa_threaded_mainloop_unlock(ml);
                pa_threaded_mainloop_stop(ml);
                pa_stream_unref(stream);
                pa_context_disconnect(ctx);
                pa_context_unref(ctx);
                pa_threaded_mainloop_free(ml);
                restore_terminal();
                exit(0);
            }
        }
    }
}
#else  /* _WIN32: Windows live audio via Win32 waveOut. */

/* Four cycling buffers of BUFFER_FRAMES each (~21 ms at 48 kHz).
   Total buffered latency ~85 ms. Uses CALLBACK_EVENT so the main
   thread can wait on a single event handle for buffer completion -
   no callback function needed. */
#define WAVE_BUFFERS 4
static HWAVEOUT hwo;
static WAVEHDR  wave_hdrs[WAVE_BUFFERS];
static int16_t *wave_bufs[WAVE_BUFFERS];
static HANDLE   wave_event;

static void play_pulse(void) {
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag     = WAVE_FORMAT_PCM;
    wf.nChannels      = 2;
    wf.nSamplesPerSec = SAMPLE_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign    = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
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

    /* Prime all four buffers with rendered audio and submit them. */
    for (int i = 0; i < WAVE_BUFFERS; i++) {
        render_chunk(wave_bufs[i], BUFFER_FRAMES);
        waveOutWrite(hwo, &wave_hdrs[i], sizeof(WAVEHDR));
    }

    int next = 0;
    for (;;) {
        /* Wait until this slot's buffer has finished playing. */
        while (!(wave_hdrs[next].dwFlags & WHDR_DONE)) {
            WaitForSingleObject(wave_event, INFINITE);
        }
        /* Render new audio into the freed buffer and resubmit. */
        render_chunk(wave_bufs[next], BUFFER_FRAMES);
        waveOutWrite(hwo, &wave_hdrs[next], sizeof(WAVEHDR));
        next = (next + 1) % WAVE_BUFFERS;
    }
}
#endif  /* _WIN32 */

int main(int argc, char **argv) {
    voice_pool_init();
    gen_init();
    reverb_init();
    delay_init();

    if (argc >= 2 && strcmp(argv[1], "--render") == 0) {
        if (argc != 4) {
            fprintf(stderr, "usage: %s --render <seconds> <output.wav>\n", argv[0]);
            exit(1);
        }
        char *end;
        long seconds = strtol(argv[2], &end, 10);
        if (*argv[2] == '\0' || *end != '\0') {
            fprintf(stderr, "render: seconds must be an integer, got \"%s\"\n", argv[2]);
            exit(1);
        }
        if (seconds < 1 || seconds > 3600) {
            fprintf(stderr, "render: seconds must be in 1..3600, got %ld\n", seconds);
            exit(1);
        }
        render_wav((int)seconds, argv[3]);
        fprintf(stderr, "arena: %zu/%d bytes used\n", arena_used(), HEAP_BYTES);
    } else if (argc == 1 || (argc == 2 && strcmp(argv[1], "--no-ui") == 0)) {
        if (argc == 2) no_ui = 1;
        play_pulse();
    } else {
        fprintf(stderr, "usage: %s [--render <seconds> <output.wav>] [--no-ui]\n", argv[0]);
        exit(1);
    }
    return 0;
}
