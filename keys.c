#include "keys.h"
#include "ui.h"
#include "gen.h"
#include "voice.h"
#include "effects.h"
#include <stdint.h>

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
    return KEY_CONSUMED;
}
