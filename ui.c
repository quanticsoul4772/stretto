#include "ui.h"
#include "voice.h"
#include "gen.h"
#include "effects.h"
#include "config.h"   /* SAMPLE_RATE for the ms/bar readout */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>   /* sig_atomic_t for the resume-line double buffer */

#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#define TIOCGWINSZ_VAL 0x5413
static struct termios saved_termios;
static struct termios raw_termios;   /* the raw state we applied; replayed
                                        by the SIGTSTP handler's resume path */
static int saved_fcntl_flags = 0;
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

/* Version string shown in the status panel. "" (not NULL) until
   main() calls the setter: unit-test binaries link ui.o without
   main.o and must be able to build the panel without crashing.
   ui.c deliberately does NOT include version.h - the Makefile keeps
   version.h out of $(HEADERS) so only main.o rebuilds per version
   bump. */
static const char *version_str = "";

void ui_set_version(const char *v) { version_str = v ? v : ""; }

/* Preset-capture flag table (specs/004-preset-capture): one row per
   initial-state flag, indexed by its UI_PARAM_* id. Ranges mirror the
   engine setters' clamps exactly; out-of-range values are usage
   errors rather than silent clamps. `named`: 1 = scale names, 2 =
   filter-mode names (numeric always accepted too). Setters consume
   no PRNG draws, so output stays a pure function of (seed, flags).
   Lives HERE, not in main.c: unit-test binaries link ui.o without
   main.o, and the help overlay reads names/ranges from this table. */
static void set_mod_depth_i(int v) { voice_set_mod_depth((uint16_t)v); }

const ParamFlag PARAM_FLAGS[UI_PARAM_COUNT] = {
    /* [UI_PARAM_SCALE]          */ { "--scale",          0,    5,     gen_set_scale,              1 },
    /* [UI_PARAM_BAR_MS]         */ { "--bar-ms",         760,  7600,  gen_set_bar_ms,             0 },
    /* [UI_PARAM_GATE]           */ { "--gate",           32,   255,   gen_set_gate,               0 },
    /* [UI_PARAM_MOD_DEPTH]      */ { "--mod-depth",      100,  8000,  set_mod_depth_i,            0 },
    /* [UI_PARAM_CUTOFF]         */ { "--cutoff",         30,   180,   voice_set_cutoff,           0 },
    /* [UI_PARAM_RESONANCE]      */ { "--resonance",      0,    180,   voice_set_resonance,        0 },
    /* [UI_PARAM_LFO_DEPTH]      */ { "--lfo-depth",      0,    255,   voice_set_lfo_filter_depth, 0 },
    /* [UI_PARAM_FILTER_MODE]    */ { "--filter-mode",    0,    3,     voice_set_filter_mode,      2 },
    /* [UI_PARAM_REVERB]         */ { "--reverb",         0,    256,   reverb_set_wet,             0 },
    /* [UI_PARAM_DELAY]          */ { "--delay",          0,    256,   delay_set_wet,              0 },
    /* [UI_PARAM_FEEDBACK]       */ { "--feedback",       0,    200,   delay_set_feedback,         0 },
    /* [UI_PARAM_COMP_THRESHOLD] */ { "--comp-threshold", 8000, 30000, compressor_set_threshold,   0 },
    /* [UI_PARAM_SWING]          */ { "--swing",          0,    100,   gen_set_swing,              0 },
};

/* --- help overlay (v2, 074) + clear --- */

static int build_help_overlay(char *buf);   /* defined below */

void ui_show_help(void) {
    if (no_ui_flag) return;
    /* Built on demand into BSS (zero file bytes; the old static
       HELP_TEXT rodata card is gone). Written once per ? toggle, so
       values are live as of that keypress; the MIDI drain keeps
       running while help is up (only scope DRAWING pauses), so a
       CC-changed value goes stale until the next open - accepted. */
    static char hb[4096];
    int n = build_help_overlay(hb);
    (void)!write(1, hb, (size_t)n);
}

void ui_clear_screen(void) {
    if (no_ui_flag) return;
    (void)!write(1, "\x1b[H\x1b[2J", 7);
}

int  ui_help_visible(void)            { return help_visible; }
void ui_set_help_visible(int visible) { help_visible = visible; }

void ui_set_no_ui(int flag) { no_ui_flag = flag; }
int  ui_get_no_ui(void)     { return no_ui_flag; }

/* --- preset capture: dirty bits + resume line ------------------ */

/* Which parameters the USER explicitly set (CLI flag or live key).
   Internal mutate() drift is deliberately never marked: --seed alone
   reproduces all drift from bar 0, and printing drifted values would
   seed the recalled run with them as INITIAL values and diverge at
   the first mutation. */
static uint16_t param_set_mask = 0;

void ui_mark_param_set(int param) {
    if (param >= 0 && param < UI_PARAM_COUNT)
        param_set_mask |= (uint16_t)(1u << param);
}

int ui_param_is_set(int param) {
    if (param < 0 || param >= UI_PARAM_COUNT) return 0;
    return (param_set_mask >> param) & 1u;
}

static const char *SCALE_NAMES[6] = {
    "dorian", "lydian", "phrygian", "locrian", "harmminor", "mixolydian"
};
static const char *FILTER_MODE_NAMES[4] = { "lp", "hp", "bp", "notch" };

const char *ui_scale_name(int idx) {
    return SCALE_NAMES[(idx >= 0 && idx < 6) ? idx : 0];
}
const char *ui_filter_mode_name(int idx) {
    return FILTER_MODE_NAMES[idx & 3];
}

/* Double buffer: writers fill the inactive half, record its length
   (plain int - the line can exceed sig_atomic_t's guaranteed 127
   range), then flip the index LAST. The signal handler reads the
   index once and write()s that half, so a signal landing mid-rebuild
   still sees a complete previous snapshot. Buffers are BSS-zero, so
   ui_get_resume_line returns "" until the first set. Worst-case line
   is ~233 bytes (all 13 params + max-width values); 320 leaves
   margin. */
static char resume_buf[2][320];
static int  resume_len[2] = { 0, 0 };
static volatile sig_atomic_t resume_idx = 0;

void ui_set_resume_line(const char *line) {
    int next = 1 - (int)__atomic_load_n(&resume_idx, __ATOMIC_ACQUIRE);
    int n = 0;
    while (line[n] != '\0' && n < (int)sizeof(resume_buf[0]) - 2) {
        resume_buf[next][n] = line[n];
        n++;
    }
    resume_buf[next][n++] = '\n';
    resume_buf[next][n]   = '\0';
    resume_len[next] = n;
    /* Release-store the flip: `volatile sig_atomic_t` alone only
       covers a signal delivered to the SAME thread. Fatal signals can
       be delivered to any thread (e.g. pulse's mainloop thread), and
       without release/acquire that thread could observe the new index
       before the buffer bytes (same pattern as audio_midi.c's ring
       head). */
    __atomic_store_n(&resume_idx, (sig_atomic_t)next, __ATOMIC_RELEASE);
}

const char *ui_get_resume_line(void) {
    return resume_buf[__atomic_load_n(&resume_idx, __ATOMIC_ACQUIRE)];
}

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

/* One exit path for the four raw-mode syscall failures: each inline
   fprintf+exit pair costs ~30 B of call setup, and this file sits a
   few dozen bytes under a 4 KB text-segment page cliff (Principle I:
   crossing it costs the full page). */
static void die_sys(const char *what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

void ui_term_raw_mode(void) {
    /* Not a TTY on either end (stdin redirected: no keys to read;
       stdout redirected: the oscilloscope escapes would be garbage in
       a file) -> degrade to headless --no-ui mode instead of dying.
       Exact parity with the Windows path below, where GetConsoleMode
       failure flips no_ui_flag. The old code exit(1)'d on the ENOTTY
       tcgetattr, so `./synth < /dev/null` could not run at all. */
    if (!isatty(0) || !isatty(1)) {
        fprintf(stderr,
            "stretto: stdin/stdout is not a terminal; running headless (--no-ui)\n");
        no_ui_flag = 1;
        return;
    }
    if (tcgetattr(0, &saved_termios) < 0) die_sys("tcgetattr");
    int flags = fcntl(0, F_GETFL, 0);
    if (flags < 0) die_sys("fcntl");
    /* Record BOTH saved states before flipping termios_saved: the
       signal handler restores termios AND fcntl flags whenever
       termios_saved is set, so setting it before saved_fcntl_flags
       was recorded let a signal in that window stomp stdin's flags
       with the BSS zero. Only then mutate the terminal. */
    saved_fcntl_flags = flags;
    termios_saved = 1;
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    raw_termios = raw;   /* keep a copy for the SIGTSTP resume replay */
    if (tcsetattr(0, TCSANOW, &raw) < 0) die_sys("tcsetattr");
    if (fcntl(0, F_SETFL, flags | O_NONBLOCK) < 0) die_sys("fcntl");
}

static void sig_term_restore(void);

void ui_term_restore_mode(void) {
    /* O_NONBLOCK lives on the open file description shared with the
       parent shell; without the F_SETFL inside sig_term_restore every
       live session handed the shell back a nonblocking stdin. */
    sig_term_restore();
    termios_saved = 0;
}

/* Restore the terminal from signal context, then die by the signal.
   atexit handlers do NOT run on signal death, so this handler is the
   only restore path for Ctrl-C / SIGTERM / SIGQUIT / SIGHUP during
   live mode. Everything called here is on the POSIX async-signal-safe
   list (tcsetattr, fcntl, write, raise). SA_RESETHAND restored the
   default disposition before this handler ran, so the re-raise
   terminates the process with an honest killed-by-signal wait status
   (130 / 143 / ...), and a second Ctrl-C hard-kills even if the
   restore itself were to wedge. */
/* Shared by the fatal-signal and SIGTSTP handlers; async-signal-safe
   (tcsetattr, fcntl, write). */
static void sig_term_restore(void) {
    if (termios_saved) {
        tcsetattr(0, TCSANOW, &saved_termios);
        fcntl(0, F_SETFL, saved_fcntl_flags);
        (void)!write(1, "\x1b[?25h\n", 7);
    }
}

static void sig_restore_and_reraise(int sig) {
    sig_term_restore();
    /* Resume line: UNCONDITIONAL (outside the termios_saved guard) -
       headless --no-ui sessions never enter raw mode but are a
       primary capture target. Single acquire-load of the index; the
       double buffer guarantees a complete snapshot; write() is
       async-signal-safe. */
    {
        int idx = (int)__atomic_load_n(&resume_idx, __ATOMIC_ACQUIRE);
        if (resume_len[idx] > 0)
            (void)!write(2, resume_buf[idx], (size_t)resume_len[idx]);
    }
    raise(sig);
}

/* SIGTSTP (Ctrl-Z): restore the terminal, honestly stop, and re-enter
   raw mode when resumed. The stop happens INSIDE the handler: the
   default disposition is restored, SIGTSTP is unblocked (it is
   blocked during its own delivery), and raise() stops the process on
   the spot - so when `fg` sends SIGCONT, execution resumes right
   after raise() and the lines below re-arm the handler and replay the
   saved raw termios + O_NONBLOCK. Everything used here is on the
   POSIX.1-2008 async-signal-safe list (tcsetattr, fcntl, write,
   signal, sigprocmask, raise; sigemptyset/sigaddset are pure local
   bit ops). signal() rather than sigaction keeps the handler and
   install code small - Principle I pays per byte here (this handler
   sat exactly on a 4 KB text-segment page cliff).

   `bg` caveat (standard TUI behavior, same as less/vim): resuming in
   the background makes the re-entry tcsetattr raise SIGTTOU, which
   stops the process again until it is foregrounded. */
static void sig_suspend_and_resume(int sig) {
    (void)sig;
    sig_term_restore();
    signal(SIGTSTP, SIG_DFL);
    sigset_t tstp;
    sigemptyset(&tstp);
    sigaddset(&tstp, SIGTSTP);
    sigprocmask(SIG_UNBLOCK, &tstp, NULL);
    raise(SIGTSTP);
    /* ---- stopped here; SIGCONT (fg) resumes below ---- */
    signal(SIGTSTP, sig_suspend_and_resume);
    if (termios_saved) {
        tcsetattr(0, TCSANOW, &raw_termios);
        fcntl(0, F_SETFL, saved_fcntl_flags | O_NONBLOCK);
    }
    /* No redraw needed: the UI loop repaints every frame. The kernel
       re-blocks SIGTSTP on handler return per the entry mask. */
}

void ui_install_signal_handlers(void) {
    static const int fatal[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_restore_and_reraise;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
        sigaction(fatal[i], &sa, NULL);
    signal(SIGTSTP, sig_suspend_and_resume);
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
        /* Redirected stdin/stdout -> headless, same as the POSIX
           isatty degrade above. */
        fprintf(stderr,
            "stretto: stdin/stdout is not a terminal; running headless (--no-ui)\n");
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
        (void)!write(1, "\x1b[?25h\n", 7);
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

/* Append an unsigned decimal value to buf, advancing *p. t must
   hold a full uint32 (10 digits): the panel prints the raw seed,
   which is clock-derived (~10 digits) in unseeded live runs - the
   old t[6] was a stack smash waiting for the first big value. */
static inline void append_num(char *buf, int *p, unsigned v) {
    char t[10]; int n = 0;
    do { t[n++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (n > 0) buf[(*p)++] = t[--n];
}

/* Append a NUL-terminated string to buf, advancing *p. */
static inline void append_str(char *buf, int *p, const char *s) {
    while (*s) buf[(*p)++] = *s++;
}

/* Clamp one status line (bytes [start, *p)) to `budget` VISIBLE
   columns: escape sequences count zero columns (each fused color
   literal carries ~5 invisible bytes - a byte-counted budget would
   truncate an 80-col line at visible column ~35), and on overflow
   the line is cut at the last space boundary, never mid-word or
   mid-escape. One post-pass instead of save/rollback bookkeeping at
   ~40 field call sites (each inline group cost ~25 B of -Os text;
   the sum busted the size budget). Only sees our own output, so any
   CSI run can be skipped uncounted (the sole non-SGR escape is the
   line-leading erase \x1b[2K). */
static void clamp_line(char *buf, int start, int *p, int budget) {
    int cols = 0, cut = -1;
    int i = start;
    while (i < *p) {
        if (buf[i] == '\x1b' && i + 1 < *p && buf[i + 1] == '[') {
            int e = i + 2;
            while (e < *p && !(buf[e] >= 0x40 && buf[e] <= 0x7e)) e++;
            i = e + 1;
            continue;
        }
        if (cols == budget) {
            *p = (cut >= 0) ? cut : i;
            return;
        }
        if (buf[i] == ' ') cut = i;
        cols++;
        i++;
    }
}

/* Shared name tables (row + panel + help overlay). */
static const char *CHORD_NAMES[6] = { "triad", "7th", "sus4", "sus2", "inv1", "inv2" };
static const char *FILTER_MODES_SHORT[4] = { "LP", "HP", "BP", "NO" };
static const char *FULL_FILTER_NAMES[4] = { "lowpass", "highpass", "bandpass", "notch" };

/* Activity glyphs for voices [lo,hi) in role-band colors. Shared by
   the fallback row (full 0..N_VOICES span) and panel L5's per-role
   bands. */
static void append_voice_dots(char *buf, int *p, uint32_t mask,
                              int lo, int hi) {
    for (int i = lo; i < hi; i++) {
        const char *role_col =
            (i < 2) ? COL_RED :
            (i < 5) ? COL_GREEN :
            (i < 8) ? COL_BLUE :
                      COL_YELLOW;
        append_str(buf, p, role_col);
        buf[(*p)++] = (mask & (1u << i)) ? '*' : '.';
    }
}

/* Build one status row of synth state (small-terminal fallback).
   Field order is stable; consumers (README) reference field labels
   by name (M, S, V, G, R, D, deg, act, chord, Cr, Sec, Td, Mo, F, N,
   L, T, Lm, Sw). Emitted unclamped; the caller's clamp_line pass
   cuts to the terminal width - pre-074 the row wrapped below ~72
   columns, scrolling the scope every frame. */
static void build_status_row(char *buf, int *p) {
    uint32_t voice_mask = voice_pool_active_mask();

    append_str(buf, p, COL_CYAN "M:" COL_WHITE);
    append_num(buf, p, voice_get_mod_depth());

    append_str(buf, p, " " COL_YELLOW "S:" COL_WHITE);
    buf[(*p)++] = "DLPlHM"[gen_get_scale() % 6];

    append_str(buf, p, " " COL_MAG "V:");
    append_voice_dots(buf, p, voice_mask, 0, N_VOICES);

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
    append_str(buf, p, CHORD_NAMES[gen_get_chord_pattern() % 6]);
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
    append_str(buf, p, FILTER_MODES_SHORT[voice_get_filter_mode() & 3u]);
    append_str(buf, p, " " COL_GREEN "Lm:" COL_WHITE);
    append_num(buf, p, compressor_get_threshold());
    append_str(buf, p, " " COL_CYAN "Sw:" COL_WHITE);
    append_num(buf, p, gen_get_swing());
}

int ui_build_status_row(char *buf, unsigned tw) {
    int p = 0;
    append_str(buf, &p, "\x1b[2K");
    build_status_row(buf, &p);
    clamp_line(buf, 0, &p, (int)tw - 1);
    append_str(buf, &p, COL_RESET);
    buf[p++] = '\r'; buf[p++] = '\n';
    return p;
}

/* --- rich status panel (074) ------------------------------------
   Five full-word lines on terminals with PANEL_MIN_ROWS or more
   rows; the single row above is the small-terminal fallback. Rich
   layout: 5 lines + (th-6)-row scope; fallback: 1 line + (th-2)-row
   scope - both total th-1, so screen usage is unchanged. */
#define PANEL_MIN_ROWS 20

static void panel_line1(char *buf, int *p) {
    append_str(buf, p, COL_CYAN "stretto" COL_WHITE);
    if (version_str[0]) {
        buf[(*p)++] = ' ';
        append_str(buf, p, version_str);
    }
    append_str(buf, p, " | " COL_CYAN "seed " COL_WHITE);
    append_num(buf, p, (unsigned)gen_get_seed_input());
    append_str(buf, p, " | " COL_CYAN "bar " COL_WHITE);
    append_num(buf, p, (unsigned)gen_get_bar());
    append_str(buf, p, " [");
    append_str(buf, p, gen_get_section_name());
    buf[(*p)++] = ']';
    append_str(buf, p, " | " COL_WHITE);
    /* Canonical bar-length readout; same expression as keys.c's
       resume-line --bar-ms value. */
    append_num(buf, p,
        (unsigned)((uint64_t)gen_get_step_samples() * 48000u / SAMPLE_RATE));
    append_str(buf, p, " ms/bar");
    append_str(buf, p, " | " COL_CYAN "swing " COL_WHITE);
    append_num(buf, p, gen_get_swing());
}

static void panel_line2(char *buf, int *p) {
    append_str(buf, p, COL_YELLOW "scale " COL_WHITE);
    append_str(buf, p, SCALE_NAMES[gen_get_scale() % 6]);
    append_str(buf, p, " | " COL_WHITE "chord ");
    append_str(buf, p, CHORD_NAMES[gen_get_chord_pattern() % 6]);
    append_str(buf, p, " on degree ");
    append_num(buf, p, gen_get_chord_root());
    append_str(buf, p, " | " COL_YELLOW "tension " COL_WHITE);
    append_num(buf, p, gen_get_tension());
    append_str(buf, p, " | " COL_MAG "motif " COL_WHITE);
    append_str(buf, p, gen_motif_replaying() ? "replaying" : "capturing");
}

static void panel_line3(char *buf, int *p) {
    append_str(buf, p, COL_CYAN "filter " COL_WHITE);
    append_str(buf, p, FULL_FILTER_NAMES[voice_get_filter_mode() & 3u]);
    append_str(buf, p, " | " COL_CYAN "cutoff " COL_WHITE);
    append_num(buf, p, voice_get_cutoff());
    append_str(buf, p, " | " COL_YELLOW "resonance " COL_WHITE);
    append_num(buf, p, voice_get_resonance());
    append_str(buf, p, " | " COL_GREEN "lfo " COL_WHITE);
    append_num(buf, p, voice_get_lfo_filter_depth());
}

static void panel_line4(char *buf, int *p) {
    append_str(buf, p, COL_GREEN "reverb " COL_WHITE);
    append_num(buf, p, reverb_get_wet());
    append_str(buf, p, " | " COL_YELLOW "delay " COL_WHITE);
    append_num(buf, p, delay_get_wet());
    buf[(*p)++] = '/';
    append_num(buf, p, delay_get_feedback());
    append_str(buf, p, " | " COL_GREEN "compressor " COL_WHITE);
    append_num(buf, p, compressor_get_threshold());
    append_str(buf, p, " | " COL_MAG "mod depth " COL_WHITE);
    append_num(buf, p, voice_get_mod_depth());
}

static void panel_line5(char *buf, int *p) {
    uint32_t mask = voice_pool_active_mask();
    append_str(buf, p, COL_WHITE "voices");
    append_str(buf, p, " " COL_RED "bass ");
    append_voice_dots(buf, p, mask, 0, 2);
    append_str(buf, p, " " COL_GREEN "chord ");
    append_voice_dots(buf, p, mask, 2, 5);
    append_str(buf, p, " " COL_BLUE "melody ");
    append_voice_dots(buf, p, mask, 5, 8);
    append_str(buf, p, " " COL_YELLOW "drums ");
    append_voice_dots(buf, p, mask, 8, N_VOICES);
}

int ui_build_status_panel(char *buf, unsigned tw) {
    static void (*const line_fns[5])(char *, int *) = {
        panel_line1, panel_line2, panel_line3, panel_line4, panel_line5
    };
    int p = 0;
    for (int i = 0; i < 5; i++) {
        int start = p;
        append_str(buf, &p, "\x1b[2K");
        line_fns[i](buf, &p);
        clamp_line(buf, start, &p, (int)tw - 1);
        append_str(buf, &p, COL_RESET);
        buf[p++] = '\r'; buf[p++] = '\n';
    }
    return p;
}

/* --- help overlay v2 (074): live parameter readout --------------
   One row per PARAM_FLAGS entry: live key, flag name, CURRENT value,
   range. Names and ranges come from PARAM_FLAGS (single source with
   the CLI parser); values from the same getters the resume line
   uses. Colorless by design - no second NO_COLOR strip site. */

/* Live-key labels per UI_PARAM_* id (ENUM order - gate is index 2,
   mod-depth 3, matching ui.h, NOT the old help card's ordering).
   "" = flag-only parameter (no live key). */
static const char *PARAM_KEYS[UI_PARAM_COUNT] = {
    "s", "+/-", "g/G", "[/]", "c/C", "n/N", "m/M",
    "t", "r/R", "d/D", "f/F", "l/L", ""
};

/* Live value for PARAM_FLAGS[k]; named params return the index (the
   overlay maps it to a name). bar-ms uses the canonical expression
   from keys.c's resume-line builder. */
static unsigned param_current(int k) {
    switch (k) {
        case UI_PARAM_SCALE:          return gen_get_scale();
        case UI_PARAM_BAR_MS:
            return (unsigned)((uint64_t)gen_get_step_samples() * 48000u / SAMPLE_RATE);
        case UI_PARAM_GATE:           return gen_get_gate();
        case UI_PARAM_MOD_DEPTH:      return voice_get_mod_depth();
        case UI_PARAM_CUTOFF:         return voice_get_cutoff();
        case UI_PARAM_RESONANCE:      return voice_get_resonance();
        case UI_PARAM_LFO_DEPTH:      return voice_get_lfo_filter_depth();
        case UI_PARAM_FILTER_MODE:    return voice_get_filter_mode();
        case UI_PARAM_REVERB:         return reverb_get_wet();
        case UI_PARAM_DELAY:          return delay_get_wet();
        case UI_PARAM_FEEDBACK:       return delay_get_feedback();
        case UI_PARAM_COMP_THRESHOLD: return compressor_get_threshold();
        case UI_PARAM_SWING:          return gen_get_swing();
        default:                      return 0;
    }
}

/* Space-pad the current line (started at byte line_start) out to
   column col. */
static void pad_to(char *buf, int *p, int line_start, int col) {
    while (*p - line_start < col) buf[(*p)++] = ' ';
}

static int build_help_overlay(char *buf) {
    int p = 0;
    append_str(buf, &p,
        "\x1b[H\x1b[2J"
        "  stretto keys (live values)\r\n"
        "  ------------\r\n");
    for (int k = 0; k < UI_PARAM_COUNT; k++) {
        int ls = p;
        append_str(buf, &p, "  ");
        append_str(buf, &p, PARAM_KEYS[k][0] ? PARAM_KEYS[k] : "--");
        pad_to(buf, &p, ls, 9);
        append_str(buf, &p, PARAM_FLAGS[k].name + 2);   /* drop "--" */
        pad_to(buf, &p, ls, 26);
        if (PARAM_FLAGS[k].named == 1)
            append_str(buf, &p, ui_scale_name((int)param_current(k)));
        else if (PARAM_FLAGS[k].named == 2)
            append_str(buf, &p, ui_filter_mode_name((int)param_current(k)));
        else
            append_num(buf, &p, param_current(k));
        pad_to(buf, &p, ls, 38);
        buf[p++] = '[';
        append_num(buf, &p, (unsigned)PARAM_FLAGS[k].min);
        buf[p++] = '-';
        append_num(buf, &p, (unsigned)PARAM_FLAGS[k].max);
        buf[p++] = ']';
        if (!PARAM_KEYS[k][0])
            append_str(buf, &p, "  (flag only)");
        append_str(buf, &p, "\r\n");
    }
    append_str(buf, &p,
        "\r\n"
        "  SPACE    force mutation now\r\n"
        "  ?        toggle this help\r\n"
        "  q        quit\r\n"
        "\r\n"
        "  (any key dismisses)\r\n");
    return p;
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

/* no-color.org: a NO_COLOR environment variable that is present AND
   non-empty disables default color output. Cached once - the spec is
   about launch environment, not live toggling. */
static int no_color_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("NO_COLOR");
        cached = (v != NULL && v[0] != '\0');
    }
    return cached;
}

/* Strip SGR (color) escape sequences - ESC [ ... m - from buf in
   place, returning the new length. Functional escapes (cursor home /
   hide, erase-line, clear) end in other final bytes and are kept.
   Filtering at the single write site beats gating ~25 call sites
   whose color escapes are fused into compile-time string literals
   (e.g. " " COL_YELLOW "S:" COL_WHITE). Exposed (not static) so the
   unit suite can exercise it without a PTY. */
int ui_strip_sgr(char *buf, int len) {
    int w = 0, r = 0;
    while (r < len) {
        if (buf[r] == 0x1b && r + 1 < len && buf[r + 1] == '[') {
            int e = r + 2;
            while (e < len && !(buf[e] >= 0x40 && buf[e] <= 0x7e)) e++;
            if (e < len && buf[e] == 'm') {   /* SGR: drop it */
                r = e + 1;
                continue;
            }
            while (r <= e && r < len) buf[w++] = buf[r++];
            continue;
        }
        buf[w++] = buf[r++];
    }
    return w;
}

void ui_draw_oscilloscope(int16_t *buf, uint32_t frames) {
    unsigned int tw = 80, th = 24;
    ui_term_get_size(&tw, &th);

    uint32_t w = tw > 120u ? 120u : tw;
    /* Rich panel on tall terminals: 5 lines + (th-6)-row scope;
       fallback: 1 row + (th-2)-row scope. Both leave one spare row. */
    int rich = th >= PANEL_MIN_ROWS;
    uint32_t h = rich ? (uint32_t)(th - 6u)
                      : (th > 2u ? (uint32_t)(th - 2u) : 22u);
    if (w > frames) w = frames;

    /* 24 KB comfortable headroom for worst-case colored output:
       24 rows * ~720 bytes/row + status panel + escapes ~= 18 KB. */
    static char out[24576];
    int p = 0;
    /* Cursor home, hide cursor. (Erase-line leads every status line
       inside the builders.) */
    out[p++] = 0x1b; out[p++] = '['; out[p++] = 'H';
    out[p++] = 0x1b; out[p++] = '['; out[p++] = '?'; out[p++] = '2'; out[p++] = '5'; out[p++] = 'l';

    p += rich ? ui_build_status_panel(out + p, tw)
              : ui_build_status_row(out + p, tw);
    build_oscilloscope_grid(out, &p, (int)sizeof(out), buf, frames, w, h);

    if (no_color_enabled()) p = ui_strip_sgr(out, p);
    (void)!write(1, out, (size_t)p);
}
