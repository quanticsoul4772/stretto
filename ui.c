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
    out[p++] = 0x1b; out[p++] = '['; out[p++] = 'H';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '?'; out[p++] = '2'; out[p++] = '5'; out[p++] = 'l';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '2'; out[p++] = 'K';

    #define APPEND_NUM(val) do { \
        unsigned _v = (unsigned)(val); \
        char _t[6]; int _n = 0; \
        do { _t[_n++] = '0' + _v % 10; _v /= 10; } while (_v); \
        while (_n > 0) out[p++] = _t[--_n]; \
    } while (0)
    #define APPEND_STR(s) do { const char *_s = (s); while (*_s) out[p++] = *_s++; } while (0)
    #define COL_RESET  "\x1b[0m"
    #define COL_DIM    "\x1b[90m"
    #define COL_BLUE   "\x1b[34m"
    #define COL_CYAN   "\x1b[36m"
    #define COL_GREEN  "\x1b[32m"
    #define COL_YELLOW "\x1b[33m"
    #define COL_MAG    "\x1b[35m"
    #define COL_RED    "\x1b[31m"
    #define COL_WHITE  "\x1b[97m"

    uint32_t voice_mask = voice_pool_active_mask();

    APPEND_STR(COL_CYAN "M:" COL_WHITE);
    APPEND_NUM(voice_get_mod_depth());

    APPEND_STR(" " COL_YELLOW "S:" COL_WHITE);
    out[p++] = "DLPlHM"[gen_get_scale() % 6];

    APPEND_STR(" " COL_MAG "V:");
    for (int i = 0; i < N_VOICES; i++) {
        const char *role_col =
            (i < 2) ? COL_RED :
            (i < 5) ? COL_GREEN :
            (i < 8) ? COL_BLUE :
                      COL_YELLOW;
        APPEND_STR(role_col);
        out[p++] = (voice_mask & (1u << i)) ? '*' : '.';
    }

    APPEND_STR(" " COL_CYAN "G:" COL_WHITE);
    APPEND_NUM(gen_get_gate());

    APPEND_STR(" " COL_GREEN "R:" COL_WHITE);
    APPEND_NUM(reverb_get_wet());

    APPEND_STR(" " COL_YELLOW "D:" COL_WHITE);
    APPEND_NUM(delay_get_wet());
    out[p++] = '/';
    APPEND_NUM(delay_get_feedback());

    APPEND_STR(" " COL_MAG "deg:" COL_WHITE);
    APPEND_NUM(gen_get_degree());

    APPEND_STR(" " COL_RED "act:");
    {
        uint8_t am = gen_get_active_mask();
        for (int i = 0; i < 7; i++) {
            if (am & (1u << i)) { APPEND_STR(COL_RED); out[p++] = '#'; }
            else                { APPEND_STR(COL_DIM); out[p++] = '.'; }
        }
    }

    APPEND_STR(" " COL_WHITE "chord:");
    {
        static const char *names[6] = { "triad", "7th", "sus4", "sus2", "inv1", "inv2" };
        APPEND_STR(names[gen_get_chord_pattern() % 6]);
    }
    APPEND_STR(" " COL_CYAN "Cr:" COL_WHITE);
    APPEND_NUM(gen_get_chord_root());
    APPEND_STR(" " COL_CYAN "Sec:" COL_WHITE);
    APPEND_STR(gen_get_section_name());

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

    static const char *level_colors[7] = {
        COL_DIM, COL_BLUE, COL_CYAN, COL_GREEN, COL_YELLOW, COL_MAG, COL_RED,
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
        /* Worst case alternating colors ~800 bytes per row; bail
           before overrunning the buffer. */
        if (p > (int)sizeof(out) - 1024) break;
    }
    APPEND_STR(COL_RESET);
    (void)!write(1, out, (size_t)p);

    #undef APPEND_NUM
    #undef APPEND_STR
}
