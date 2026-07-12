#include "keys.h"
#include "ui.h"
#include "gen.h"
#include "voice.h"
#include "effects.h"
#include "config.h"
#include <stdint.h>
#include <stdio.h>

/* Rebuild the resume line ("resume with: --seed N ...") from current
   state. Only parameters the user explicitly set (ui_param_is_set)
   are included - internal mutate() drift is excluded because --seed
   alone reproduces it from bar 0. Called at every consumed keypress
   (the snapshot means "state at your last action") and once at
   audio_play() start so headless / untouched sessions still capture
   the seed and any CLI-flag values. */
void keys_build_resume_line(void) {
    char buf[320];
    size_t cap = sizeof buf;
    int p = snprintf(buf, cap, "resume with: --seed %u",
                     (unsigned)gen_get_seed_input());

    /* One loop over the shared PARAM_FLAGS table + ui_param_current
       getters instead of 13 hand-written APPEND blocks (077 size
       reclaim: the old form was ~1 KB of snprintf call sites).
       Output is byte-identical to the old per-param code -
       tests/unit/test_resume.c pins the exact strings, including
       the 232-char all-params max-width case - and the flag names
       can no longer drift from main.c's parser. */
    for (int k = 0; k < UI_PARAM_COUNT; k++) {
        if (!ui_param_is_set(k)) continue;
        if (p <= 0 || (size_t)p >= cap) break;
        unsigned v = ui_param_current(k);
        if (PARAM_FLAGS[k].named == 1)
            p += snprintf(buf + p, cap - (size_t)p, " %s %s",
                          PARAM_FLAGS[k].name, ui_scale_name((int)v));
        else if (PARAM_FLAGS[k].named == 2)
            p += snprintf(buf + p, cap - (size_t)p, " %s %s",
                          PARAM_FLAGS[k].name, ui_filter_mode_name((int)v));
        else
            p += snprintf(buf + p, cap - (size_t)p, " %s %u",
                          PARAM_FLAGS[k].name, v);
    }

    ui_set_resume_line(buf);
}

int keys_dispatch(char ch) {
    if (ch == '?') {
        int v = !ui_help_visible();
        ui_set_help_visible(v);
        if (v) ui_show_help();
        else   ui_clear_screen();
        return KEY_CONSUMED;
    }
    if (ui_help_visible()) {
        ui_set_help_visible(0);
        ui_clear_screen();
    }
    if      (ch == ' ')  gen_force_mutate();
    else if (ch == '+')  gen_set_tempo(-10);
    else if (ch == '-')  gen_set_tempo(+10);
    else if (ch == '[') {
        uint16_t d = voice_get_mod_depth();
        voice_set_mod_depth(d > 200 ? d - 200 : 100);
    }
    else if (ch == ']')  voice_set_mod_depth(voice_get_mod_depth() + 200);
    else if (ch == 's')  gen_cycle_scale();
    else if (ch == 'g')  gen_adjust_gate(-16);
    else if (ch == 'G')  gen_adjust_gate(+16);
    else if (ch == 'd')  delay_adjust_wet(-16);
    else if (ch == 'D')  delay_adjust_wet(+16);
    else if (ch == 'f')  delay_adjust_feedback(-16);
    else if (ch == 'F')  delay_adjust_feedback(+16);
    else if (ch == 'r')  reverb_adjust_wet(-16);
    else if (ch == 'R')  reverb_adjust_wet(+16);
    else if (ch == 'c')  voice_adjust_cutoff(-10);
    else if (ch == 'C')  voice_adjust_cutoff(+10);
    else if (ch == 'n')  voice_adjust_resonance(-10);
    else if (ch == 'N')  voice_adjust_resonance(+10);
    else if (ch == 'm')  voice_adjust_lfo_filter_depth(-8);
    else if (ch == 'M')  voice_adjust_lfo_filter_depth(+8);
    else if (ch == 't')  voice_cycle_filter_mode();
    else if (ch == 'l')  compressor_adjust_threshold(-1000);
    else if (ch == 'L')  compressor_adjust_threshold(+1000);
    else if (ch == 'q')  return KEY_QUIT;
    else                 return KEY_IGNORED;

    /* Consumed parameter keys converge here: mark the parameter as
       user-set and re-snapshot the resume line. SPACE (mutate) maps
       to no parameter - it perturbs generative state, which --seed
       reproduces; capturing it makes no sense. */
    {
        int param = -1;
        switch (ch) {
            case '+': case '-': param = UI_PARAM_BAR_MS;         break;
            case '[': case ']': param = UI_PARAM_MOD_DEPTH;      break;
            case 's':           param = UI_PARAM_SCALE;          break;
            case 'g': case 'G': param = UI_PARAM_GATE;           break;
            case 'd': case 'D': param = UI_PARAM_DELAY;          break;
            case 'f': case 'F': param = UI_PARAM_FEEDBACK;       break;
            case 'r': case 'R': param = UI_PARAM_REVERB;         break;
            case 'c': case 'C': param = UI_PARAM_CUTOFF;         break;
            case 'n': case 'N': param = UI_PARAM_RESONANCE;      break;
            case 'm': case 'M': param = UI_PARAM_LFO_DEPTH;      break;
            case 't':           param = UI_PARAM_FILTER_MODE;    break;
            case 'l': case 'L': param = UI_PARAM_COMP_THRESHOLD; break;
            default: break;
        }
        if (param >= 0) ui_mark_param_set(param);
        keys_build_resume_line();
    }
    return KEY_CONSUMED;
}
