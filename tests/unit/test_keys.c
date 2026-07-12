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
#include <string.h>

/* test_init_synth + ui_set_no_ui (keeps ui_show_help / clear_screen
   quiet during the test run). */
static inline void ensure_init(void) {
    static int done = 0;
    test_init_synth();
    if (!done) { ui_set_no_ui(1); done = 1; }
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

/* ---- NO_COLOR SGR filter (052) ---- */

TEST(strip_sgr_removes_color_keeps_text) {
    char buf[] = "\x1b[33mS:\x1b[97mD\x1b[0m done";
    int n = ui_strip_sgr(buf, (int)(sizeof buf - 1));
    buf[n] = '\0';
    ASSERT_EQ(n, 8);
    ASSERT_EQ(memcmp(buf, "S:D done", 8), 0);
}

TEST(strip_sgr_keeps_functional_escapes) {
    /* Cursor home, hide cursor, erase line end in H / l / K - all
       must survive; only the trailing SGR reset is removed. */
    char buf[] = "\x1b[H\x1b[?25l\x1b[2Kx\x1b[0m";
    int n = ui_strip_sgr(buf, (int)(sizeof buf - 1));
    buf[n] = '\0';
    ASSERT_EQ(n, 14);
    ASSERT_EQ(memcmp(buf, "\x1b[H\x1b[?25l\x1b[2Kx", 14), 0);
}

TEST(strip_sgr_plain_text_unchanged) {
    char buf[] = "no escapes at all";
    int n = ui_strip_sgr(buf, (int)(sizeof buf - 1));
    ASSERT_EQ(n, (int)(sizeof buf - 1));
    ASSERT_EQ(memcmp(buf, "no escapes at all", (size_t)n), 0);
}

TEST(strip_sgr_truncated_escape_at_end_kept) {
    /* A buffer ending mid-escape must not read past len or drop the
       partial bytes silently. */
    char buf[] = "x\x1b[3";
    int n = ui_strip_sgr(buf, (int)(sizeof buf - 1));
    ASSERT_EQ(n, (int)(sizeof buf - 1));
}

/* ---- status builders (074) ---- */

/* Assert every \r\n-terminated segment of buf[0..len) is at most
   max_cols VISIBLE columns: per segment, strip SGR (functional
   escapes survive - pinned above), then skip the leading \x1b[2K.
   Returns the number of lines checked. */
static int count_clamped_lines(char *buf, int len, int max_cols) {
    int nlines = 0, start = 0;
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            char seg[1024];
            int seglen = i - start;
            memcpy(seg, buf + start, (size_t)seglen);
            int n = ui_strip_sgr(seg, seglen);
            char *s = seg;
            if (n >= 4 && memcmp(s, "\x1b[2K", 4) == 0) { s += 4; n -= 4; }
            if (n > max_cols) return -1;
            nlines++;
            start = i + 2;
            i++;
        }
    }
    return nlines;
}

TEST(status_panel_clamps_at_tw40) {
    ensure_init();
    char buf[4096];
    int len = ui_build_status_panel(buf, 40);
    ASSERT_TRUE(len > 0);
    ASSERT_EQ(count_clamped_lines(buf, len, 39), 5);
}

TEST(status_row_clamps_at_tw40) {
    ensure_init();
    char buf[2048];
    int len = ui_build_status_row(buf, 40);
    ASSERT_TRUE(len > 0);
    ASSERT_EQ(count_clamped_lines(buf, len, 39), 1);
}

TEST(status_panel_tw80_keeps_full_words) {
    ensure_init();
    char buf[4096];
    int len = ui_build_status_panel(buf, 80);
    int n = ui_strip_sgr(buf, len);
    buf[n] = '\0';
    /* Over-truncation isn't caught by length checks alone: the
       80-col panel must still carry the promised full words. */
    ASSERT_TRUE(strstr(buf, "stretto") != NULL);
    ASSERT_TRUE(strstr(buf, "scale ") != NULL);
    ASSERT_TRUE(strstr(buf, "swing ") != NULL);
    ASSERT_TRUE(strstr(buf, "lowpass") != NULL || strstr(buf, "highpass") != NULL
             || strstr(buf, "bandpass") != NULL || strstr(buf, "notch") != NULL);
    ASSERT_TRUE(strstr(buf, "voices") != NULL);
}

TEST(status_panel_survives_10_digit_seed) {
    ensure_init();
    /* Live runs without --seed use a clock-derived seed (~10 digits).
       append_num's digit buffer must hold a full uint32 - the
       original t[6] smashed the stack on the panel's seed field
       (found as a live-mode SIGSEGV by test_smoke_live sub-check C). */
    gen_seed(4294967295u);
    char buf[4096];
    int len = ui_build_status_panel(buf, 200);
    ASSERT_TRUE(len > 0);
    int n = ui_strip_sgr(buf, len);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "4294967295") != NULL);
}

TEST(status_row_contains_swing_field) {
    ensure_init();
    /* Wide budget: Sw: sits last in the row (importance-ordered) and
       the full row is ~78 visible cols, so measure unclamped. */
    char buf[2048];
    int len = ui_build_status_row(buf, 200);
    int n = ui_strip_sgr(buf, len);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "Sw:") != NULL);
}

int main(void) {
    return RUN_ALL();
}
