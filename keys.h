#ifndef KEYS_H
#define KEYS_H

#define KEY_IGNORED   0
#define KEY_CONSUMED  1
#define KEY_QUIT      2

/* Dispatch one keypress to the right subsystem. Handles help toggle
   (via ui_*), generative actions (gen_*), voice / filter (voice_*),
   and effects (effects.h adjust_* functions). Returns KEY_QUIT when
   the user pressed 'q' (caller cleans up audio + exits), otherwise
   KEY_CONSUMED or KEY_IGNORED. */
int keys_dispatch(char ch);

#endif
