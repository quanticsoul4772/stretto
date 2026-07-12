#ifndef UI_H
#define UI_H

#include <stdint.h>

/* Cross-platform terminal helpers, oscilloscope, status row, and
   help overlay for live-mode stretto. All terminal output goes
   through ANSI escapes; raw mode enables those on Windows via
   ENABLE_VIRTUAL_TERMINAL_PROCESSING. */

/* --- terminal primitives --- */

int  ui_term_get_size(unsigned int *w, unsigned int *h);
int  ui_term_read_key(char *out);
void ui_term_raw_mode(void);
void ui_term_restore_mode(void);

#ifndef _WIN32
/* Install SIGINT / SIGTERM / SIGHUP / SIGQUIT handlers that restore
   the terminal (async-signal-safe restore) and re-raise so the
   process still dies by the signal. Linux live mode only; Windows
   delivers Ctrl-C as keystroke 0x03 through the 'q' path instead. */
void ui_install_signal_handlers(void);
#endif

/* --- one-frame render --- */

/* Draw one oscilloscope frame plus the status row, using `buf` as
   the source signal (interleaved L,R,L,R int16 samples). Issues a
   single write() per frame so terminal I/O does not block the audio
   thread. Honors NO_COLOR (no-color.org): a present, non-empty
   NO_COLOR strips the SGR color sequences (glyph heat-map ramp and
   functional cursor escapes are kept). */
void ui_draw_oscilloscope(int16_t *buf, uint32_t frames);

/* Strip SGR (ESC [ ... m) sequences from buf in place; returns the
   new length. Functional escapes are kept. Public for unit tests. */
int ui_strip_sgr(char *buf, int len);

/* Status builders (074). Each writes erase-line-prefixed, CRLF-
   terminated line(s) into buf, clamped to tw-1 VISIBLE columns
   (SGR bytes don't count), and returns the byte length. The 5-line
   full-word panel renders when the terminal has >= 20 rows; the
   compact single row is the small-terminal fallback. Public for
   PTY-free unit tests (the ui_strip_sgr precedent). */
int ui_build_status_panel(char *buf, unsigned tw);
int ui_build_status_row(char *buf, unsigned tw);

/* --- help overlay --- */

void ui_show_help(void);
void ui_clear_screen(void);
int  ui_help_visible(void);
void ui_set_help_visible(int visible);

/* --- no-ui flag (set from main() based on --no-ui argv) --- */

void ui_set_no_ui(int flag);
int  ui_get_no_ui(void);

/* --- preset capture: parameter identity + resume line --- */

/* One id per user-tunable parameter, shared by main.c (CLI flags)
   and keys.c (live keys) for dirty-bit marking and by the resume-
   line builder. */
enum {
    UI_PARAM_SCALE = 0,
    UI_PARAM_BAR_MS,
    UI_PARAM_GATE,
    UI_PARAM_MOD_DEPTH,
    UI_PARAM_CUTOFF,
    UI_PARAM_RESONANCE,
    UI_PARAM_LFO_DEPTH,
    UI_PARAM_FILTER_MODE,
    UI_PARAM_REVERB,
    UI_PARAM_DELAY,
    UI_PARAM_FEEDBACK,
    UI_PARAM_COMP_THRESHOLD,
    UI_PARAM_SWING,          /* 069: first flag-only param (no live key) */
    UI_PARAM_COUNT
};

/* Preset-capture flag table (specs/004-preset-capture): one row per
   initial-state flag, indexed by its UI_PARAM_* id. Ranges mirror the
   engine setters' clamps exactly. `named`: 1 = scale names, 2 =
   filter-mode names (numeric always accepted too). Defined in ui.c
   (NOT main.c): every unit-test binary links ui.o without main.o, and
   the help overlay reads names/ranges from this table. */
typedef struct {
    const char *name;
    int min, max;
    void (*set)(int);
    int named;
} ParamFlag;

extern const ParamFlag PARAM_FLAGS[UI_PARAM_COUNT];

/* Version string for the status panel. main() passes STRETTO_VERSION
   once at startup; ui.c never includes version.h so only main.o
   rebuilds when the version changes (version.h is deliberately out of
   the Makefile's $(HEADERS)). Defaults to "" until set. */
void ui_set_version(const char *v);

/* Dirty bits track parameters the USER explicitly set (CLI flag or
   live key) - never internal mutate() drift, which --seed already
   reproduces from bar 0. Marked at the user-action call sites, not
   inside the shared adjust_* functions (voice_mutate_filter calls
   those). */
void ui_mark_param_set(int param);
int  ui_param_is_set(int param);

/* Canonical flag-value names, single source for main.c's parser and
   keys.c's resume-line builder. */
const char *ui_scale_name(int idx);        /* 0..5 */
const char *ui_filter_mode_name(int idx);  /* 0..3 */

/* Resume line ("resume with: --seed N ..."). Double-buffered so the
   POSIX signal handler can write() a consistent snapshot even if a
   signal lands mid-rebuild. Stored with a trailing newline;
   ui_get_resume_line returns "" until the first set. */
void        ui_set_resume_line(const char *line);
const char *ui_get_resume_line(void);

#endif
