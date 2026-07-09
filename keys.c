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

#define APPEND(fmt, val) \
    do { if (p > 0 && (size_t)p < cap) \
        p += snprintf(buf + p, cap - (size_t)p, fmt, val); } while (0)

    if (ui_param_is_set(UI_PARAM_SCALE))
        APPEND(" --scale %s", ui_scale_name((int)gen_get_scale()));
    if (ui_param_is_set(UI_PARAM_BAR_MS))
        APPEND(" --bar-ms %u",
               (unsigned)((uint64_t)gen_get_step_samples() * 48000u / SAMPLE_RATE));
    if (ui_param_is_set(UI_PARAM_GATE))
        APPEND(" --gate %u", (unsigned)gen_get_gate());
    if (ui_param_is_set(UI_PARAM_MOD_DEPTH))
        APPEND(" --mod-depth %u", (unsigned)voice_get_mod_depth());
    if (ui_param_is_set(UI_PARAM_CUTOFF))
        APPEND(" --cutoff %u", (unsigned)voice_get_cutoff());
    if (ui_param_is_set(UI_PARAM_RESONANCE))
        APPEND(" --resonance %u", (unsigned)voice_get_resonance());
    if (ui_param_is_set(UI_PARAM_LFO_DEPTH))
        APPEND(" --lfo-depth %u", (unsigned)voice_get_lfo_filter_depth());
    if (ui_param_is_set(UI_PARAM_FILTER_MODE))
        APPEND(" --filter-mode %s", ui_filter_mode_name((int)voice_get_filter_mode()));
    if (ui_param_is_set(UI_PARAM_REVERB))
        APPEND(" --reverb %u", (unsigned)reverb_get_wet());
    if (ui_param_is_set(UI_PARAM_DELAY))
        APPEND(" --delay %u", (unsigned)delay_get_wet());
    if (ui_param_is_set(UI_PARAM_FEEDBACK))
        APPEND(" --feedback %u", (unsigned)delay_get_feedback());
    if (ui_param_is_set(UI_PARAM_COMP_THRESHOLD))
        APPEND(" --comp-threshold %u", (unsigned)compressor_get_threshold());
#undef APPEND

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
