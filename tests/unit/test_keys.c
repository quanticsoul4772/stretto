/* Unit tests for keys.c (live-mode key dispatcher).
 *
 * Tests verify each key invokes the expected subsystem setter by
 * sampling the relevant getter before and after dispatch. Stubs out
 * ui.c's terminal I/O so help-related keys do not touch a real TTY. */
#include "test.h"
#include "../../keys.h"
#include "../../ui.h"
#include "../../voice.h"
#include "../../gen.h"
#include "../../effects.h"
#include <stdint.h>

static int pool_ready = 0;
static void ensure_init(void) {
    if (!pool_ready) {
        voice_pool_init();
        effects_init();
        gen_seed(0);
        gen_init();
        ui_set_no_ui(1);                /* keep ui_show_help / clear_screen quiet */
        pool_ready = 1;
    }
}

TEST(keys_q_returns_quit) {
    ensure_init();
    ASSERT_EQ(keys_dispatch('q'), KEY_QUIT);
}

TEST(keys_unknown_returns_ignored) {
    ensure_init();
    ASSERT_EQ(keys_dispatch('z'), KEY_IGNORED);
    ASSERT_EQ(keys_dispatch('@'), KEY_IGNORED);
}

TEST(keys_help_toggles_visibility) {
    ensure_init();
    ui_set_help_visible(0);
    ASSERT_EQ(keys_dispatch('?'), KEY_CONSUMED);
    ASSERT_TRUE(ui_help_visible());
    ASSERT_EQ(keys_dispatch('?'), KEY_CONSUMED);
    ASSERT_FALSE(ui_help_visible());
}

TEST(keys_any_key_dismisses_help) {
    ensure_init();
    ui_set_help_visible(1);
    keys_dispatch('s');                 /* any non-? key */
    ASSERT_FALSE(ui_help_visible());
}

TEST(keys_tempo_changes_step_samples) {
    ensure_init();
    uint32_t before = gen_get_step_samples();
    keys_dispatch('+');
    ASSERT_TRUE(gen_get_step_samples() < before);
    keys_dispatch('-');
    keys_dispatch('-');
    ASSERT_TRUE(gen_get_step_samples() > before);
}

TEST(keys_bracket_adjusts_mod_depth) {
    ensure_init();
    voice_set_mod_depth(2000);
    keys_dispatch(']');
    ASSERT_EQ(voice_get_mod_depth(), 2200u);
    keys_dispatch('[');
    ASSERT_EQ(voice_get_mod_depth(), 2000u);
}

TEST(keys_scale_cycles) {
    ensure_init();
    uint8_t before = gen_get_scale();
    keys_dispatch('s');
    ASSERT_EQ(gen_get_scale(), (uint8_t)((before + 1) % 6));
}

TEST(keys_gate_g_down_G_up) {
    ensure_init();
    uint8_t before = gen_get_gate();
    keys_dispatch('g');
    uint8_t after_down = gen_get_gate();
    ASSERT_TRUE(after_down <= before);
    keys_dispatch('G');
    ASSERT_TRUE(gen_get_gate() >= after_down);
}

TEST(keys_delay_wet_d_down_D_up) {
    ensure_init();
    uint16_t before = delay_get_wet();
    keys_dispatch('D');
    ASSERT_TRUE(delay_get_wet() > before);
    keys_dispatch('d');
    ASSERT_TRUE(delay_get_wet() <= before + 16);
}

TEST(keys_delay_feedback_f_down_F_up) {
    ensure_init();
    uint16_t before = delay_get_feedback();
    keys_dispatch('F');
    ASSERT_TRUE(delay_get_feedback() > before);
    keys_dispatch('f');
    ASSERT_TRUE(delay_get_feedback() <= before + 16);
}

TEST(keys_reverb_wet_r_down_R_up) {
    ensure_init();
    uint16_t before = reverb_get_wet();
    keys_dispatch('R');
    ASSERT_TRUE(reverb_get_wet() > before);
    keys_dispatch('r');
    ASSERT_TRUE(reverb_get_wet() <= before + 16);
}

TEST(keys_filter_cutoff_c_down_C_up) {
    ensure_init();
    /* Cutoff initial value (200) is above the user clamp ceiling (180),
       so push into mid-range first so both directions land cleanly. */
    while (voice_get_cutoff() > 100) keys_dispatch('c');
    uint16_t mid = voice_get_cutoff();
    keys_dispatch('C');
    ASSERT_TRUE(voice_get_cutoff() > mid);
    keys_dispatch('c');
    ASSERT_TRUE(voice_get_cutoff() <= mid);
}

TEST(keys_filter_resonance_n_down_N_up) {
    ensure_init();
    while (voice_get_resonance() > 100) keys_dispatch('n');
    uint16_t mid = voice_get_resonance();
    keys_dispatch('N');
    ASSERT_TRUE(voice_get_resonance() > mid);
    keys_dispatch('n');
    ASSERT_TRUE(voice_get_resonance() <= mid);
}

TEST(keys_lfo_depth_m_down_M_up) {
    ensure_init();
    while (voice_get_lfo_filter_depth() > 100) keys_dispatch('m');
    uint16_t mid = voice_get_lfo_filter_depth();
    keys_dispatch('M');
    ASSERT_TRUE(voice_get_lfo_filter_depth() > mid);
    keys_dispatch('m');
    ASSERT_TRUE(voice_get_lfo_filter_depth() <= mid);
}

TEST(keys_t_cycles_filter_mode) {
    ensure_init();
    /* Bring mode to a known state. */
    while (voice_get_filter_mode() != 0) keys_dispatch('t');
    keys_dispatch('t');
    ASSERT_EQ(voice_get_filter_mode(), 1);
    keys_dispatch('t');
    ASSERT_EQ(voice_get_filter_mode(), 2);
    keys_dispatch('t');
    ASSERT_EQ(voice_get_filter_mode(), 3);
    keys_dispatch('t');
    ASSERT_EQ(voice_get_filter_mode(), 0);
}

int main(void) {
    return RUN_ALL();
}
