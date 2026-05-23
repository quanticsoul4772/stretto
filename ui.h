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

/* --- one-frame render --- */

/* Draw one oscilloscope frame plus the status row, using `buf` as
   the source signal (interleaved L,R,L,R int16 samples). Issues a
   single write() per frame so terminal I/O does not block the audio
   thread. */
void ui_draw_oscilloscope(int16_t *buf, uint32_t frames);

/* --- help overlay --- */

void ui_show_help(void);
void ui_clear_screen(void);
int  ui_help_visible(void);
void ui_set_help_visible(int visible);

/* --- no-ui flag (set from main() based on --no-ui argv) --- */

void ui_set_no_ui(int flag);
int  ui_get_no_ui(void);

#endif
