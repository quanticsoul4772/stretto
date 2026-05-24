#include "ui.h"
#include "voice.h"
#include "gen.h"
#include "effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#define TIOCGWINSZ_VAL 0x5413
static struct termios saved_termios;
static int termios_saved = 0;
#else
#include <windows.h>
#include <conio.h>
#include <io.h>
#define write _write
static DWORD g_old_in_mode  = 0;
static DWORD g_old_out_mode = 0;
static HANDLE g_hin  = NULL;
static HANDLE g_hout = NULL;
static int win_term_saved = 0;
#endif

static int help_visible = 0;
static int no_ui_flag   = 0;

/* --- help text + show / clear --- */

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

void ui_show_help(void) {
    if (no_ui_flag) return;
    (void)!write(1, HELP_TEXT, sizeof(HELP_TEXT) - 1);
}

void ui_clear_screen(void) {
    if (no_ui_flag) return;
    (void)!write(1, "\x1b[H\x1b[2J", 7);
}

int  ui_help_visible(void)            { return help_visible; }
void ui_set_help_visible(int visible) { help_visible = visible; }

void ui_set_no_ui(int flag) { no_ui_flag = flag; }
int  ui_get_no_ui(void)     { return no_ui_flag; }

/* --- platform-specific terminal primitives --- */

#ifndef _WIN32

int ui_term_get_size(unsigned int *w, unsigned int *h) {
    unsigned short ws[4] = { 24, 80, 0, 0 };
    if (ioctl(1, TIOCGWINSZ_VAL, ws) < 0) return 0;
    if (ws[0]) *h = ws[0];
    if (ws[1]) *w = ws[1];
    return 1;
}

int ui_term_read_key(char *out) {
    return (read(0, out, 1) == 1) ? 1 : 0;
}

void ui_term_raw_mode(void) {
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

void ui_term_restore_mode(void) {
    if (termios_saved) {
        tcsetattr(0, TCSANOW, &saved_termios);
        (void)!write(1, "\x1b[?25h\n", 8);
        termios_saved = 0;
    }
}

#else  /* _WIN32 */

int ui_term_get_size(unsigned int *w, unsigned int *h) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_hout ? g_hout : GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return 0;
    *w = (unsigned int)(csbi.srWindow.Right  - csbi.srWindow.Left + 1);
    *h = (unsigned int)(csbi.srWindow.Bottom - csbi.srWindow.Top  + 1);
    if (*w == 0) *w = 80;
    if (*h == 0) *h = 24;
    return 1;
}

int ui_term_read_key(char *out) {
    if (_kbhit()) {
        int ch = _getch();
        if (ch == 3) {                  /* Ctrl-C arrives as 0x03 in raw mode */
            *out = 'q';
            return 1;
        }
        *out = (char)ch;
        return 1;
    }
    return 0;
}

void ui_term_raw_mode(void) {
    g_hin  = GetStdHandle(STD_INPUT_HANDLE);
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(g_hin, &g_old_in_mode) ||
        !GetConsoleMode(g_hout, &g_old_out_mode)) {
        /* probably redirected; UI off */
        no_ui_flag = 1;
        return;
    }
    win_term_saved = 1;
    DWORD newin  = g_old_in_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT
                                   | ENABLE_PROCESSED_INPUT);
    DWORD newout = g_old_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(g_hin, newin);
    SetConsoleMode(g_hout, newout);
}

void ui_term_restore_mode(void) {
    if (win_term_saved) {
        (void)!write(1, "\x1b[?25h\n", 8);
        SetConsoleMode(g_hin, g_old_in_mode);
        SetConsoleMode(g_hout, g_old_out_mode);
        win_term_saved = 0;
    }
}

#endif

/* --- oscilloscope + status row --- */

/* ANSI 16-color escapes; cheap 5 bytes each. Shared between the
   status row builder and the oscilloscope grid renderer. */
#define COL_RESET  "\x1b[0m"
#define COL_DIM    "\x1b[90m"
#define COL_BLUE   "\x1b[34m"
#define COL_CYAN   "\x1b[36m"
#define COL_GREEN  "\x1b[32m"
#define COL_YELLOW "\x1b[33m"
#define COL_MAG    "\x1b[35m"
#define COL_RED    "\x1b[31m"
#define COL_WHITE  "\x1b[97m"

/* Append an unsigned decimal value to buf, advancing *p. */
static inline void append_num(char *buf, int *p, unsigned v) {
    char t[6]; int n = 0;
    do { t[n++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (n > 0) buf[(*p)++] = t[--n];
}

/* Append a NUL-terminated string to buf, advancing *p. */
static inline void append_str(char *buf, int *p, const char *s) {
    while (*s) buf[(*p)++] = *s++;
}

/* Build one status row of synth state into buf, advancing *p.
   Field order is stable; consumers (README) reference field labels
   by name (M, S, V, G, R, D, deg, act, chord, Cr, Sec, Td, F, N, L, T). */
static void build_status_row(char *buf, int *p) {
    uint32_t voice_mask = voice_pool_active_mask();

    append_str(buf, p, COL_CYAN "M:" COL_WHITE);
    append_num(buf, p, voice_get_mod_depth());

    append_str(buf, p, " " COL_YELLOW "S:" COL_WHITE);
    buf[(*p)++] = "DLPlHM"[gen_get_scale() % 6];

    append_str(buf, p, " " COL_MAG "V:");
    for (int i = 0; i < N_VOICES; i++) {
        const char *role_col =
            (i < 2) ? COL_RED :
            (i < 5) ? COL_GREEN :
            (i < 8) ? COL_BLUE :
                      COL_YELLOW;
        append_str(buf, p, role_col);
        buf[(*p)++] = (voice_mask & (1u << i)) ? '*' : '.';
    }

    append_str(buf, p, " " COL_CYAN "G:" COL_WHITE);
    append_num(buf, p, gen_get_gate());

    append_str(buf, p, " " COL_GREEN "R:" COL_WHITE);
    append_num(buf, p, reverb_get_wet());

    append_str(buf, p, " " COL_YELLOW "D:" COL_WHITE);
    append_num(buf, p, delay_get_wet());
    buf[(*p)++] = '/';
    append_num(buf, p, delay_get_feedback());

    append_str(buf, p, " " COL_MAG "deg:" COL_WHITE);
    append_num(buf, p, gen_get_degree());

    append_str(buf, p, " " COL_RED "act:");
    {
        uint8_t am = gen_get_active_mask();
        for (int i = 0; i < 7; i++) {
            if (am & (1u << i)) { append_str(buf, p, COL_RED); buf[(*p)++] = '#'; }
            else                { append_str(buf, p, COL_DIM); buf[(*p)++] = '.'; }
        }
    }

    append_str(buf, p, " " COL_WHITE "chord:");
    {
        static const char *names[6] = { "triad", "7th", "sus4", "sus2", "inv1", "inv2" };
        append_str(buf, p, names[gen_get_chord_pattern() % 6]);
    }
    append_str(buf, p, " " COL_CYAN "Cr:" COL_WHITE);
    append_num(buf, p, gen_get_chord_root());
    append_str(buf, p, " " COL_CYAN "Sec:" COL_WHITE);
    append_str(buf, p, gen_get_section_name());
    append_str(buf, p, " " COL_YELLOW "Td:" COL_WHITE);
    append_num(buf, p, gen_get_tension());
    append_str(buf, p, " " COL_MAG "Mo:" COL_WHITE);
    buf[(*p)++] = gen_motif_replaying() ? 'r' : 'c';

    append_str(buf, p, " " COL_CYAN "F:" COL_WHITE);
    append_num(buf, p, voice_get_cutoff());
    append_str(buf, p, " " COL_YELLOW "N:" COL_WHITE);
    append_num(buf, p, voice_get_resonance());
    append_str(buf, p, " " COL_GREEN "L:" COL_WHITE);
    append_num(buf, p, voice_get_lfo_filter_depth());
    append_str(buf, p, " " COL_MAG "T:" COL_WHITE);
    {
        static const char *modes[4] = { "LP", "HP", "BP", "NO" };
        append_str(buf, p, modes[voice_get_filter_mode() & 3u]);
    }
    append_str(buf, p, " " COL_GREEN "Lm:" COL_WHITE);
    append_num(buf, p, compressor_get_threshold());

    append_str(buf, p, COL_RESET);
    buf[(*p)++] = '\r'; buf[(*p)++] = '\n';
}

/* Render the oscilloscope grid into buf using `frames` of stereo
   samples from `signal`. `w` columns x `h` rows; per-cell intensity
   mapped to a 7-level heat-map palette, RLE-emitted so the color
   escape only appears when the cell intensity changes. Bails before
   overrunning `cap` bytes of buf. */
static void build_oscilloscope_grid(char *buf, int *p, int cap,
                                    int16_t *signal, uint32_t frames,
                                    uint32_t w, uint32_t h) {
    static const char *level_colors[7] = {
        COL_DIM, COL_BLUE, COL_CYAN, COL_GREEN, COL_YELLOW, COL_MAG, COL_RED,
    };
    static const char level_chars[7] = { ' ', '.', '-', '+', '*', '#', '@' };

    int last_lvl = -1;
    for (uint32_t r = 0; r < h; r++) {
        buf[(*p)++] = 0x1b; buf[(*p)++] = '['; buf[(*p)++] = '2'; buf[(*p)++] = 'K';
        int32_t thresh = 8000 - (int32_t)((r * 16000u) / h);
        for (uint32_t c = 0; c < w; c++) {
            int16_t samp = signal[2 * ((c * frames) / w)];
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
                append_str(buf, p, level_colors[lvl]);
                last_lvl = lvl;
            }
            buf[(*p)++] = level_chars[lvl];
        }
        if (r < h - 1) { buf[(*p)++] = '\r'; buf[(*p)++] = '\n'; }
        /* Worst case alternating colors ~800 bytes per row; bail
           before overrunning the buffer. */
        if (*p > cap - 1024) break;
    }
    append_str(buf, p, COL_RESET);
}

void ui_draw_oscilloscope(int16_t *buf, uint32_t frames) {
    unsigned int tw = 80, th = 24;
    ui_term_get_size(&tw, &th);

    uint32_t w = tw > 120u ? 120u : tw;
    uint32_t h = th > 2u   ? (uint32_t)(th - 2u) : 22u;
    if (w > frames) w = frames;

    /* 24 KB comfortable headroom for worst-case colored output:
       24 rows * ~720 bytes/row + status row + escapes ~= 18 KB. */
    static char out[24576];
    int p = 0;
    /* Cursor home, hide cursor, clear status row. */
    out[p++] = 0x1b; out[p++] = '['; out[p++] = 'H';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '?'; out[p++] = '2'; out[p++] = '5'; out[p++] = 'l';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '2'; out[p++] = 'K';

    build_status_row(out, &p);
    build_oscilloscope_grid(out, &p, (int)sizeof(out), buf, frames, w, h);

    (void)!write(1, out, (size_t)p);
}
