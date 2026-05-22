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
#include <conio.h>
#include <io.h>
#define write _write
#endif

/* Platform-abstracted terminal primitives. All UI / keyboard code
   below uses these, so the oscilloscope + status row + key handler
   work the same on Linux and Windows. */
static int  term_get_size(unsigned int *w, unsigned int *h);
static int  term_read_key(char *out);
static void term_raw_mode(void);
static void term_restore_mode(void);

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
#define TIOCGWINSZ_VAL 0x5413
static struct termios saved_termios;
static int termios_saved = 0;
#else
static DWORD g_old_in_mode  = 0;
static DWORD g_old_out_mode = 0;
static HANDLE g_hin  = NULL;
static HANDLE g_hout = NULL;
static int win_term_saved = 0;
#endif
static int help_visible = 0;
static int no_ui = 0;

/* Portable terminal UI helpers - work on both Linux and modern
   Windows console (after VT mode is enabled by term_raw_mode). */
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
    "  c  /  C    filter cutoff  down / up\r\n"
    "  n  /  N    filter resonance  down / up\r\n"
    "  m  /  M    filter LFO depth  down / up\r\n"
    "  t          cycle filter mode  LP / HP / BP / notch\r\n"
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

/* Platform implementations of the term_* primitives declared above. */

#ifndef _WIN32

static int term_get_size(unsigned int *w, unsigned int *h) {
    unsigned short ws[4] = { 24, 80, 0, 0 };
    if (ioctl(1, TIOCGWINSZ_VAL, ws) < 0) return 0;
    if (ws[0]) *h = ws[0];
    if (ws[1]) *w = ws[1];
    return 1;
}

static int term_read_key(char *out) {
    return (read(0, out, 1) == 1) ? 1 : 0;
}

static void term_raw_mode(void) {
    if (tcgetattr(0, &saved_termios) < 0) {
        fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
        exit(1);
    }
    termios_saved = 1;
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
}

static void term_restore_mode(void) {
    if (termios_saved) {
        tcsetattr(0, TCSANOW, &saved_termios);
        (void)!write(1, "\x1b[?25h\n", 8);
        termios_saved = 0;
    }
}

#else  /* _WIN32 */

static int term_get_size(unsigned int *w, unsigned int *h) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_hout ? g_hout : GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return 0;
    *w = (unsigned int)(csbi.srWindow.Right  - csbi.srWindow.Left + 1);
    *h = (unsigned int)(csbi.srWindow.Bottom - csbi.srWindow.Top  + 1);
    if (*w == 0) *w = 80;
    if (*h == 0) *h = 24;
    return 1;
}

static int term_read_key(char *out) {
    if (_kbhit()) {
        int ch = _getch();
        if (ch == 3) {       /* Ctrl-C arrives as 0x03 in raw mode */
            *out = 'q';
            return 1;
        }
        *out = (char)ch;
        return 1;
    }
    return 0;
}

static void term_raw_mode(void) {
    g_hin  = GetStdHandle(STD_INPUT_HANDLE);
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(g_hin, &g_old_in_mode) ||
        !GetConsoleMode(g_hout, &g_old_out_mode)) {
        /* probably redirected; UI off */
        no_ui = 1;
        return;
    }
    win_term_saved = 1;
    DWORD newin  = g_old_in_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT
                                   | ENABLE_PROCESSED_INPUT);
    DWORD newout = g_old_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(g_hin, newin);
    SetConsoleMode(g_hout, newout);
}

static void term_restore_mode(void) {
    if (win_term_saved) {
        (void)!write(1, "\x1b[?25h\n", 8);
        SetConsoleMode(g_hin, g_old_in_mode);
        SetConsoleMode(g_hout, g_old_out_mode);
        win_term_saved = 0;
    }
}

#endif

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

static void restore_terminal(void) {
    term_restore_mode();
}

/* Build the entire frame (status row + oscilloscope grid) into one
   buffer and write it with a single write() syscall. Saves ~75
   syscalls per frame; portable across Linux and Windows. */
static void draw_oscilloscope(int16_t *buf, uint32_t frames) {
    unsigned int tw = 80, th = 24;
    term_get_size(&tw, &th);

    uint32_t w = tw > 120u ? 120u : tw;
    uint32_t h = th > 2u   ? (uint32_t)(th - 2u) : 22u;
    if (w > frames) w = frames;

    /* 24 KB comfortable headroom for worst-case colored output:
       24 rows * ~720 bytes/row + status row + escapes ~= 18 KB. */
    static char out[24576];
    int p = 0;
    /* Cursor home + hide + clear line */
    out[p++] = 0x1b; out[p++] = '['; out[p++] = 'H';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '?'; out[p++] = '2'; out[p++] = '5'; out[p++] = 'l';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '2'; out[p++] = 'K';

    /* Helpers for status row / colorized oscilloscope. */
    #define APPEND_NUM(val) do { \
        unsigned _v = (unsigned)(val); \
        char _t[6]; int _n = 0; \
        do { _t[_n++] = '0' + _v % 10; _v /= 10; } while (_v); \
        while (_n > 0) out[p++] = _t[--_n]; \
    } while (0)
    #define APPEND_STR(s) do { const char *_s = (s); while (*_s) out[p++] = *_s++; } while (0)
    /* ANSI 16-color escapes (cheap: 5 bytes each). */
    #define COL_RESET  "\x1b[0m"
    #define COL_DIM    "\x1b[90m"   /* bright black = dim gray */
    #define COL_BLUE   "\x1b[34m"
    #define COL_CYAN   "\x1b[36m"
    #define COL_GREEN  "\x1b[32m"
    #define COL_YELLOW "\x1b[33m"
    #define COL_MAG    "\x1b[35m"
    #define COL_RED    "\x1b[31m"
    #define COL_WHITE  "\x1b[97m"   /* bright white */

    /* Status row:
       M:<mod> S:<scale> V:<voices>   G:<gate>  R:<reverb_wet>
       D:<delay_wet>/<delay_fb>  deg:<n>  act:<7-char bitmask>
       chord:<voicing>  */
    uint32_t voice_mask = voice_pool_active_mask();

    /* M: mod_depth (cyan label, white value) */
    APPEND_STR(COL_CYAN "M:" COL_WHITE);
    APPEND_NUM(voice_get_mod_depth());

    /* S: scale letter (yellow) */
    APPEND_STR(" " COL_YELLOW "S:" COL_WHITE);
    out[p++] = "DLPlHM"[gen_get_scale() % 6];

    /* V: voices, color per role - bass red, chord green, melody blue,
       drums yellow (kick/snare/hihat = slots 8/9/10). */
    APPEND_STR(" " COL_MAG "V:");
    for (int i = 0; i < N_VOICES; i++) {
        const char *role_col =
            (i < 2) ? COL_RED :         /* bass slots 0..1 */
            (i < 5) ? COL_GREEN :       /* chord slots 2..4 */
            (i < 8) ? COL_BLUE :        /* melody slots 5..7 */
                      COL_YELLOW;       /* drum slots 8..10 */
        APPEND_STR(role_col);
        out[p++] = (voice_mask & (1u << i)) ? '*' : '.';
    }

    /* G: gate (cyan) */
    APPEND_STR(" " COL_CYAN "G:" COL_WHITE);
    APPEND_NUM(gen_get_gate());

    /* R: reverb wet (green) */
    APPEND_STR(" " COL_GREEN "R:" COL_WHITE);
    APPEND_NUM(reverb_wet);

    /* D: delay wet/feedback (yellow) */
    APPEND_STR(" " COL_YELLOW "D:" COL_WHITE);
    APPEND_NUM(delay_wet);
    out[p++] = '/';
    APPEND_NUM(delay_feedback);

    /* deg: current Markov walk position (magenta) */
    APPEND_STR(" " COL_MAG "deg:" COL_WHITE);
    APPEND_NUM(gen_get_degree());

    /* act: active scale-degree mask, colored # / dim . */
    APPEND_STR(" " COL_RED "act:");
    {
        uint8_t am = gen_get_active_mask();
        for (int i = 0; i < 7; i++) {
            if (am & (1u << i)) { APPEND_STR(COL_RED); out[p++] = '#'; }
            else                { APPEND_STR(COL_DIM); out[p++] = '.'; }
        }
    }

    /* chord: voicing name (white) */
    APPEND_STR(" " COL_WHITE "chord:");
    {
        static const char *names[6] = { "triad", "7th", "sus4", "sus2", "inv1", "inv2" };
        APPEND_STR(names[gen_get_chord_pattern() % 6]);
    }

    /* F: filter cutoff (cyan), N: resonance (yellow), L: LFO filter depth
       (green), T: filter mode (magenta). */
    APPEND_STR(" " COL_CYAN "F:" COL_WHITE);
    APPEND_NUM(voice_get_cutoff());
    APPEND_STR(" " COL_YELLOW "N:" COL_WHITE);
    APPEND_NUM(voice_get_resonance());
    APPEND_STR(" " COL_GREEN "L:" COL_WHITE);
    APPEND_NUM(voice_get_lfo_filter_depth());
    APPEND_STR(" " COL_MAG "T:" COL_WHITE);
    {
        static const char *modes[4] = { "LP", "HP", "BP", "NO" };
        APPEND_STR(modes[voice_get_filter_mode() & 3u]);
    }

    APPEND_STR(COL_RESET);
    out[p++] = '\r'; out[p++] = '\n';

    #undef APPEND_NUM

    /* Oscilloscope grid: per-cell color from intensity, RLE-emitted
       (color escape only when the char changes) so the per-frame
       byte cost stays modest. Heat-map palette: violet (very quiet) ->
       blue -> cyan -> green -> yellow -> magenta -> red (peak). */
    static const char *level_colors[7] = {
        COL_DIM,    /* ' ' silence */
        COL_BLUE,   /* '.' */
        COL_CYAN,   /* '-' */
        COL_GREEN,  /* '+' */
        COL_YELLOW, /* '*' */
        COL_MAG,    /* '#' */
        COL_RED,    /* '@' */
    };
    static const char level_chars[7] = { ' ', '.', '-', '+', '*', '#', '@' };

    int last_lvl = -1;
    for (uint32_t r = 0; r < h; r++) {
        out[p++] = 0x1b; out[p++] = '['; out[p++] = '2'; out[p++] = 'K';
        int32_t thresh = 8000 - (int32_t)((r * 16000u) / h);
        for (uint32_t c = 0; c < w; c++) {
            int16_t samp = buf[2 * ((c * frames) / w)];
            int32_t a = samp < 0 ? -samp : samp;
            int lvl;
            if (!(a > thresh))      lvl = 0;
            else if (a > 7000)      lvl = 6;
            else if (a > 5000)      lvl = 5;
            else if (a > 3500)      lvl = 4;
            else if (a > 2500)      lvl = 3;
            else if (a > 1500)      lvl = 2;
            else                    lvl = 1;
            if (lvl != last_lvl) {
                APPEND_STR(level_colors[lvl]);
                last_lvl = lvl;
            }
            out[p++] = level_chars[lvl];
        }
        if (r < h - 1) { out[p++] = '\r'; out[p++] = '\n'; }
        /* Worst case (alternating colors) needs ~800 bytes per row,
           so bail before a row that could overrun the buffer. */
        if (p > (int)sizeof(out) - 1024) break;
    }
    APPEND_STR(COL_RESET);
    (void)!write(1, out, (size_t)p);

    #undef APPEND_NUM
    #undef APPEND_STR
}

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
    /* --no-ui skips all terminal setup so the smoke test (which has
       no TTY on stdin) can run cleanly. Without a TTY, tcgetattr
       fails and the old code would exit(1). */
    if (!no_ui) {
        term_raw_mode();
        atexit(restore_terminal);
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
        while (term_read_key(&ch)) {
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
            else if (ch == 'c') voice_adjust_cutoff(-10);
            else if (ch == 'C') voice_adjust_cutoff(+10);
            else if (ch == 'n') voice_adjust_resonance(-10);
            else if (ch == 'N') voice_adjust_resonance(+10);
            else if (ch == 'm') voice_adjust_lfo_filter_depth(-8);
            else if (ch == 'M') voice_adjust_lfo_filter_depth(+8);
            else if (ch == 't') voice_cycle_filter_mode();
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
    term_restore_mode();
}

static void play_pulse(void) {
    if (!no_ui) {
        term_raw_mode();
    }
    atexit(win_cleanup);

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
        /* Wait until next slot's buffer has finished playing. */
        while (!(wave_hdrs[next].dwFlags & WHDR_DONE)) {
            WaitForSingleObject(wave_event, INFINITE);
        }
        render_chunk(wave_bufs[next], BUFFER_FRAMES);
        waveOutWrite(hwo, &wave_hdrs[next], sizeof(WAVEHDR));
        next = (next + 1) % WAVE_BUFFERS;

        /* Same UI + keyboard handling as the Linux build. */
        if (!no_ui && !help_visible) draw_oscilloscope(wave_bufs[next], BUFFER_FRAMES);

        char ch;
        while (term_read_key(&ch)) {
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
            else if (ch == 'c') voice_adjust_cutoff(-10);
            else if (ch == 'C') voice_adjust_cutoff(+10);
            else if (ch == 'n') voice_adjust_resonance(-10);
            else if (ch == 'N') voice_adjust_resonance(+10);
            else if (ch == 'm') voice_adjust_lfo_filter_depth(-8);
            else if (ch == 'M') voice_adjust_lfo_filter_depth(+8);
            else if (ch == 't') voice_cycle_filter_mode();
            else if (ch == 'q') {
                win_cleanup();
                exit(0);
            }
        }
    }
}
#endif  /* _WIN32 */

int main(int argc, char **argv) {
    voice_pool_init();

    /* Pre-scan argv for --seed N. If found, fix the PRNG to that
       value (used by the regression test and for reproducing a
       specific run you like). If absent, gen_init seeds from the
       system clock so every launch sounds different. */
    int positional_argc = 1;
    char *positional[8] = { argv[0] };
    for (int i = 1; i < argc && positional_argc < 8; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            char *end;
            unsigned long s = strtoul(argv[++i], &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "seed: must be unsigned integer, got \"%s\"\n", argv[i]);
                exit(1);
            }
            gen_seed((uint32_t)s);
        } else {
            positional[positional_argc++] = argv[i];
        }
    }
    argc = positional_argc;
    argv = positional;

    gen_init();
    reverb_init();
    delay_init();

    if (argc >= 2 && strcmp(argv[1], "--render") == 0) {
        if (argc != 4) {
            fprintf(stderr,
                "usage: %s --render <seconds> <output.wav> [--seed N]\n",
                argv[0]);
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
        fprintf(stderr,
            "usage: %s [--render <seconds> <output.wav>] [--no-ui] [--seed N]\n",
            argv[0]);
        exit(1);
    }
    return 0;
}
