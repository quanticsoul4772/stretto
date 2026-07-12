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

/* ---- 076: property fuzz over the status builders ----------------
 *
 * The 074 surface's one escaped defect (append_num's t[6] stack
 * smash on 10-digit clock seeds) was caught by the smoke suite, not
 * unit tests. These properties drive REAL state through
 * keys_dispatch with a fixed-seed xorshift32 (the test_midi.c fuzz
 * pattern: identical failures on every run) and check structural
 * invariants of ui_build_status_panel / ui_build_status_row at
 * random widths.
 *
 * ORDERING CONTRACT: the fuzz mutates gen/effects/voice state and
 * ui.c's set-only param dirty mask for the REST OF THIS BINARY.
 * Keep everything below this line LAST in the file, and never add
 * resume-line (ui_param_is_set absence) tests to this binary below
 * this point. */

static uint32_t ui_fuzz_state = 0x2E787331u;   /* fixed seed */
static uint32_t ui_fuzz(void) {
    uint32_t x = ui_fuzz_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ui_fuzz_state = x;
    return x;
}

/* Interior value for every param via the shared PARAM_FLAGS setters,
   so rail state left by earlier tests or fuzz rounds never makes an
   adjuster assert direction-blind. Cutoff/mod-depth POWER-ON
   defaults sit OUTSIDE the setter clamps ([30,180] etc.) - defaults
   are not restorable via setters; interior is the only sane reset. */
static void reset_params_interior(void) {
    static const int interior[UI_PARAM_COUNT] = {
        /* scale */ 2,    /* bar-ms */ 2000, /* gate */ 128,
        /* mod   */ 2000, /* cutoff */ 100,  /* reso */ 90,
        /* lfo   */ 120,  /* fmode  */ 1,    /* reverb */ 128,
        /* delay */ 128,  /* feedb  */ 100,  /* comp */ 15000,
        /* swing */ 50,
    };
    for (int k = 0; k < UI_PARAM_COUNT; k++)
        PARAM_FLAGS[k].set(interior[k]);
}

/* Split buf[0..len) into \r\n-terminated segments (CRLF excluded).
   Returns the count; fills starts/lens up to max. */
static int split_crlf(const char *buf, int len, int *starts, int *lens, int max) {
    int n = 0, start = 0;
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            if (n < max) { starts[n] = start; lens[n] = i - start; }
            n++;
            start = i + 2;
            i++;
        }
    }
    return n;
}

/* Strip one raw segment into out, verify + drop the pinned \x1b[2K
   prefix (part of the builder contract, so memcmp not skip-if).
   Returns the remaining stripped length, or -1 if the prefix is
   missing. */
static int strip_segment(const char *buf, int start, int len, char *out) {
    memcpy(out, buf + start, (size_t)len);
    int n = ui_strip_sgr(out, len);
    if (n < 4 || memcmp(out, "\x1b[2K", 4) != 0) return -1;
    memmove(out, out + 4, (size_t)(n - 4));
    return n - 4;
}

/* Properties P1-P5 for one builder at one width, against a tw=500
   reference built back-to-back (builders are pure getters, so the
   pair sees identical state). Diagnostics carry iter/tw/line - the
   fixed seed makes any report exactly reproducible. Returns nonzero
   on first violation so callers can stop the run. */
static int check_builder(int (*build)(char *, unsigned), int nlines,
                         unsigned tw, int iter, int *failed) {
    static char a[8192], b[8192], f[8192];
    static char sa[2048], sf[2048];
    int la = build(a, tw);
    int lb = build(b, tw);
    int lf = build(f, 500);

    /* P5: determinism - consecutive builds byte-identical. */
    if (la != lb || memcmp(a, b, (size_t)la) != 0) {
        fprintf(stderr, "    [iter=%d tw=%u] P5 determinism\n", iter, tw);
        (*failed)++;
        return 1;
    }
    /* P1: every line's visible length <= tw-1 (count_clamped_lines
       strips, skips the 2K, measures). */
    if (count_clamped_lines(a, la, tw > 1 ? (int)tw - 1 : 0) != nlines) {
        fprintf(stderr, "    [iter=%d tw=%u] P1 clamp/line-count\n", iter, tw);
        (*failed)++;
        return 1;
    }
    int st_a[8], ln_a[8], st_f[8], ln_f[8];
    if (split_crlf(a, la, st_a, ln_a, 8) != nlines ||
        split_crlf(f, lf, st_f, ln_f, 8) != nlines) {
        fprintf(stderr, "    [iter=%d tw=%u] segment count != %d\n", iter, tw, nlines);
        (*failed)++;
        return 1;
    }
    for (int i = 0; i < nlines; i++) {
        int na = strip_segment(a, st_a[i], ln_a[i], sa);
        int nf = strip_segment(f, st_f[i], ln_f[i], sf);
        if (na < 0 || nf < 0) {
            fprintf(stderr, "    [iter=%d tw=%u line=%d] missing 2K prefix\n", iter, tw, i);
            (*failed)++;
            return 1;
        }
        /* P2: no escape byte survives past the 2K prefix - catches
           both split escapes and functional-escape leakage. */
        if (memchr(sa, 0x1b, (size_t)na) != NULL) {
            fprintf(stderr, "    [iter=%d tw=%u line=%d] P2 escape leak\n", iter, tw, i);
            (*failed)++;
            return 1;
        }
        /* P3: the clamped line is a byte-prefix of the unclamped
           one (clamp_line cuts at a space or a visible byte, never
           inside an escape, so stripping is prefix-preserving). */
        if (na > nf || memcmp(sa, sf, (size_t)na) != 0) {
            fprintf(stderr, "    [iter=%d tw=%u line=%d] P3 prefix\n", iter, tw, i);
            (*failed)++;
            return 1;
        }
        /* P4: a truncated line was cut AT a space (dropped), or no
           space existed to cut at (mid-word overflow fallback). */
        if (na < nf && sf[na] != ' ' && memchr(sa, ' ', (size_t)na) != NULL) {
            fprintf(stderr, "    [iter=%d tw=%u line=%d] P4 boundary\n", iter, tw, i);
            (*failed)++;
            return 1;
        }
    }
    return 0;
}

TEST(status_builders_fuzz_properties) {
    ensure_init();
    reset_params_interior();
    /* Explicit alphabet (not derived from PARAM_KEYS - those carry
       '/' filler). ' ' = gen_force_mutate for cheap state variety;
       '?' is safe under ensure_init's ui_set_no_ui(1). */
    static const char ALPHA[] = "sgGdDfFrRcCnNmMt[]+-lL ?";
    for (int iter = 0; iter < 250; iter++) {
        int nk = 1 + (int)(ui_fuzz() % 8u);
        for (int k = 0; k < nk; k++)
            keys_dispatch(ALPHA[ui_fuzz() % (unsigned)(sizeof ALPHA - 1)]);
        /* Reseed sometimes - xorshift32 routinely yields 10-digit
           seeds, keeping regression pressure on append_num's t[10]
           (the 074 stack smash). */
        if ((ui_fuzz() & 3u) == 0) gen_seed(ui_fuzz());
        unsigned tw = 1u + ui_fuzz() % 200u;
        if (check_builder(ui_build_status_panel, 5, tw, iter, failed)) return;
        if (check_builder(ui_build_status_row,   1, tw, iter, failed)) return;
    }
}

TEST(status_builders_hold_across_section_burst) {
    ensure_init();
    reset_params_interior();
    /* One burst, not per-iteration: gen_step is per-SAMPLE (~36k
       calls/bar at the 760 ms floor, ~875k per section change).
       Pin tempo to the floor, cross one section boundary (~27 bars),
       then re-check the properties on the evolved state (section
       name, motif replay, chord pattern, voice activity all moved). */
    PARAM_FLAGS[UI_PARAM_BAR_MS].set(760);
    for (int i = 0; i < 1000000; i++) gen_step();
    for (int iter = 0; iter < 20; iter++) {
        unsigned tw = 1u + ui_fuzz() % 200u;
        if (check_builder(ui_build_status_panel, 5, tw, 1000000 + iter, failed)) return;
        if (check_builder(ui_build_status_row,   1, tw, 1000000 + iter, failed)) return;
    }
}

/* ---- 076: help overlay content pinning ---- */

/* Copy the \r\n-bounded line containing needle into out (NUL-
   terminated); returns 1 if found. hay must be NUL-terminated. */
static int overlay_row(const char *hay, const char *needle, char *out, int cap) {
    const char *p = strstr(hay, needle);
    if (!p) return 0;
    const char *s = p;
    while (s > hay && s[-1] != '\n') s--;
    const char *e = strstr(p, "\r\n");
    if (!e) e = hay + strlen(hay);
    int n = (int)(e - s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
    return 1;
}

TEST(help_overlay_pins_structure) {
    ensure_init();
    reset_params_interior();
    static char hb[4096];
    int n = ui_build_help_overlay(hb);
    /* 2x headroom against ui_show_help's static hb[4096]; the floor
       catches a short-circuited build. Worst case ~1.2 KB with all
       values at their digit maxima. */
    ASSERT_BETWEEN(n, 401, 2047);
    /* Colorless contract: stripping SGR changes nothing (the only
       escapes are the leading \x1b[H\x1b[2J, which strip keeps). */
    static char copy[4096];
    memcpy(copy, hb, (size_t)n);
    ASSERT_EQ(ui_strip_sgr(copy, n), n);
    hb[n] = '\0';
    /* Every flag name (sans "--") appears - single-sourced from the
       same table the CLI parser uses. */
    for (int k = 0; k < UI_PARAM_COUNT; k++)
        ASSERT_TRUE(strstr(hb, PARAM_FLAGS[k].name + 2) != NULL);
    /* Exactly one flag-only param (swing). */
    const char *fo = strstr(hb, "(flag only)");
    ASSERT_TRUE(fo != NULL);
    ASSERT_TRUE(fo == NULL || strstr(fo + 1, "(flag only)") == NULL);
    ASSERT_TRUE(strstr(hb, "force mutation now") != NULL);
    ASSERT_TRUE(strstr(hb, "toggle this help") != NULL);
    ASSERT_TRUE(strstr(hb, "(any key dismisses)") != NULL);
    ASSERT_TRUE(strstr(hb, "quit") != NULL);
}

TEST(help_overlay_shows_live_values) {
    ensure_init();
    reset_params_interior();
    /* Row-anchored asserts: a global strstr("77") is unsound with
       fuzzed neighbor values in this binary; bound the check to the
       \r\n line that carries the param name. 77 is inside gate's
       [32,255]; "notch" is unique buffer-wide. */
    gen_set_gate(77);
    voice_set_filter_mode(3);
    static char hb[4096];
    char row[256];
    int n = ui_build_help_overlay(hb);
    hb[n] = '\0';
    ASSERT_TRUE(overlay_row(hb, "gate", row, (int)sizeof row));
    ASSERT_TRUE(strstr(row, "77") != NULL);
    ASSERT_TRUE(overlay_row(hb, "filter-mode", row, (int)sizeof row));
    ASSERT_TRUE(strstr(row, "notch") != NULL);
    /* Live, not cached: change and rebuild. */
    gen_set_gate(213);
    n = ui_build_help_overlay(hb);
    hb[n] = '\0';
    ASSERT_TRUE(overlay_row(hb, "gate", row, (int)sizeof row));
    ASSERT_TRUE(strstr(row, "213") != NULL);
}

int main(void) {
    return RUN_ALL();
}
