/* Property tests for the resume line (071) - the preset-capture
 * contract's flag-bearing form, which no end-to-end path can exercise
 * on CI (render mode prints seed-only; the full line is live-mode
 * only, PA-gated).
 *
 * A DEDICATED binary, not part of test_keys.c: ui.c's param_set_mask
 * is set-only (no clear API), and test_keys' key-dispatch tests mark
 * most params - only a fresh binary has a clean mask, which is what
 * enables the absence assertions and the staged-marking property
 * below. Test order = source order (constructor registration), so
 * the clean-mask tests come FIRST in this file.
 *
 * Expected lines are built from the DRAWN values, never from the
 * getters: a getters-vs-getters comparison would be vacuous against
 * setter/getter bugs. Every setter is an exact identity for in-range
 * draws (clamps verified), so drawn-value expectations pin the whole
 * set -> get -> print chain.
 *
 * FLAG SPELLINGS in expected_line() are byte-identical to keys.c's
 * fragments AND to the 13-flag combination pair in tests/test_cli.sh:
 * this test pins keys.c's spellings, the CLI pair pins main.c's
 * parser - together they close the keys.c-vs-main.c name-drift loop.
 */
#include "test.h"
#include "../../ui.h"
#include "../../keys.h"
#include "../../gen.h"
#include "../../voice.h"
#include "../../effects.h"
#include "../../config.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Deterministic local PRNG (fixed seed; the in-repo idiom). */
static uint32_t rr_state = 0x0715A11Du;
static uint32_t rr(void) {
    uint32_t x = rr_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rr_state = x;
    return x;
}
static uint32_t rr_range(uint32_t lo, uint32_t hi) {  /* inclusive */
    return lo + rr() % (hi - lo + 1u);
}

/* Drawn values for the 13 params, indexed by UI_PARAM_*. */
typedef struct { uint32_t v[UI_PARAM_COUNT]; } draws_t;

static void draw_all(draws_t *d) {
    d->v[UI_PARAM_SCALE]          = rr_range(0, 5);
    d->v[UI_PARAM_BAR_MS]         = rr_range(760, 7600);
    d->v[UI_PARAM_GATE]           = rr_range(32, 255);
    d->v[UI_PARAM_MOD_DEPTH]      = rr_range(100, 8000);
    d->v[UI_PARAM_CUTOFF]         = rr_range(30, 180);
    d->v[UI_PARAM_RESONANCE]      = rr_range(0, 180);
    d->v[UI_PARAM_LFO_DEPTH]      = rr_range(0, 255);
    d->v[UI_PARAM_FILTER_MODE]    = rr_range(0, 3);
    d->v[UI_PARAM_REVERB]         = rr_range(0, 256);
    d->v[UI_PARAM_DELAY]          = rr_range(0, 256);
    d->v[UI_PARAM_FEEDBACK]       = rr_range(0, 200);
    d->v[UI_PARAM_COMP_THRESHOLD] = rr_range(8000, 30000);
    d->v[UI_PARAM_SWING]          = rr_range(0, 100);
}

static void apply_all(const draws_t *d) {
    gen_set_scale((int)d->v[UI_PARAM_SCALE]);
    gen_set_bar_ms((int)d->v[UI_PARAM_BAR_MS]);
    gen_set_gate((int)d->v[UI_PARAM_GATE]);
    voice_set_mod_depth((uint16_t)d->v[UI_PARAM_MOD_DEPTH]);
    voice_set_cutoff((int)d->v[UI_PARAM_CUTOFF]);
    voice_set_resonance((int)d->v[UI_PARAM_RESONANCE]);
    voice_set_lfo_filter_depth((int)d->v[UI_PARAM_LFO_DEPTH]);
    voice_set_filter_mode((int)d->v[UI_PARAM_FILTER_MODE]);
    reverb_set_wet((int)d->v[UI_PARAM_REVERB]);
    delay_set_wet((int)d->v[UI_PARAM_DELAY]);
    delay_set_feedback((int)d->v[UI_PARAM_FEEDBACK]);
    compressor_set_threshold((int)d->v[UI_PARAM_COMP_THRESHOLD]);
    gen_set_swing((int)d->v[UI_PARAM_SWING]);
}

/* Build the exact expected line for the params in MASK (bit i =
 * UI_PARAM i marked), from the drawn values. Fragments in enum order;
 * trailing \n to match ui_set_resume_line's stored form. The bar-ms
 * value mirrors keys.c's double conversion (identity at 48 kHz, but
 * kept explicit so this stays correct if SAMPLE_RATE changes). */
static void expected_line(char *out, size_t cap, uint32_t seed,
                          const draws_t *d, uint16_t mask) {
    int p = snprintf(out, cap, "resume with: --seed %u", seed);
#define FRAG(param, fmt, val) \
    do { if ((mask >> (param)) & 1u) \
        p += snprintf(out + p, cap - (size_t)p, fmt, val); } while (0)
    FRAG(UI_PARAM_SCALE,       " --scale %s",
         ui_scale_name((int)d->v[UI_PARAM_SCALE]));
    FRAG(UI_PARAM_BAR_MS,      " --bar-ms %u",
         (unsigned)((uint64_t)((uint64_t)d->v[UI_PARAM_BAR_MS]
                    * SAMPLE_RATE / 48000) * 48000u / SAMPLE_RATE));
    FRAG(UI_PARAM_GATE,        " --gate %u",       d->v[UI_PARAM_GATE]);
    FRAG(UI_PARAM_MOD_DEPTH,   " --mod-depth %u",  d->v[UI_PARAM_MOD_DEPTH]);
    FRAG(UI_PARAM_CUTOFF,      " --cutoff %u",     d->v[UI_PARAM_CUTOFF]);
    FRAG(UI_PARAM_RESONANCE,   " --resonance %u",  d->v[UI_PARAM_RESONANCE]);
    FRAG(UI_PARAM_LFO_DEPTH,   " --lfo-depth %u",  d->v[UI_PARAM_LFO_DEPTH]);
    FRAG(UI_PARAM_FILTER_MODE, " --filter-mode %s",
         ui_filter_mode_name((int)d->v[UI_PARAM_FILTER_MODE]));
    FRAG(UI_PARAM_REVERB,      " --reverb %u",     d->v[UI_PARAM_REVERB]);
    FRAG(UI_PARAM_DELAY,       " --delay %u",      d->v[UI_PARAM_DELAY]);
    FRAG(UI_PARAM_FEEDBACK,    " --feedback %u",   d->v[UI_PARAM_FEEDBACK]);
    FRAG(UI_PARAM_COMP_THRESHOLD, " --comp-threshold %u",
         d->v[UI_PARAM_COMP_THRESHOLD]);
    FRAG(UI_PARAM_SWING,       " --swing %u",      d->v[UI_PARAM_SWING]);
#undef FRAG
    snprintf(out + p, cap - (size_t)p, "\n");
}

/* strcmp with a readable diff on failure (ASSERT_EQ's "0 vs 1" is
 * useless for strings). */
static int lines_equal(const char *got, const char *want) {
    if (strcmp(got, want) == 0) return 1;
    fprintf(stderr, "    got:  %s    want: %s", got, want);
    return 0;
}

/* ---- (i) clean-mask absence contract: MUST RUN FIRST ---- */

TEST(resume_line_empty_before_first_build) {
    /* BSS contract (ui.h): "" until the first ui_set_resume_line. */
    ASSERT_EQ(strcmp(ui_get_resume_line(), ""), 0);
}

TEST(resume_line_seed_only_with_clean_mask) {
    test_init_synth();          /* gen_seed(0) -> seed input 0 */
    ui_set_no_ui(1);
    keys_build_resume_line();
    ASSERT_TRUE(lines_equal(ui_get_resume_line(),
                            "resume with: --seed 0\n"));
}

/* ---- (ii) staged random-order marking ---- */

TEST(resume_line_staged_marking_matches_mask) {
    test_init_synth();
    draws_t d;
    draw_all(&d);
    apply_all(&d);
    /* Fisher-Yates shuffle of the 13 param indices. */
    int order[UI_PARAM_COUNT];
    for (int i = 0; i < UI_PARAM_COUNT; i++) order[i] = i;
    for (int i = UI_PARAM_COUNT - 1; i > 0; i--) {
        int j = (int)(rr() % (uint32_t)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }
    uint16_t mask = 0;
    char want[320];
    for (int i = 0; i < UI_PARAM_COUNT; i++) {
        ui_mark_param_set(order[i]);
        mask |= (uint16_t)(1u << order[i]);
        keys_build_resume_line();
        expected_line(want, sizeof want, 0, &d, mask);
        ASSERT_TRUE(lines_equal(ui_get_resume_line(), want));
    }
}

/* ---- (iii) 200-iteration all-set property ---- */

TEST(resume_line_random_values_roundtrip) {
    test_init_synth();
    /* All 13 bits are set after the staged test (set-only mask). */
    for (int p = 0; p < UI_PARAM_COUNT; p++) ui_mark_param_set(p);
    char want[320];
    for (int it = 0; it < 200; it++) {
        draws_t d;
        draw_all(&d);
        apply_all(&d);
        keys_build_resume_line();
        expected_line(want, sizeof want, 0, &d, 0x1FFF);
        ASSERT_TRUE(lines_equal(ui_get_resume_line(), want));
    }
}

/* ---- (iv) exact max-width line: 232 chars incl. newline ---- */

TEST(resume_line_max_width_exact) {
    test_init_synth();
    gen_seed(4294967295u);      /* 10-digit seed */
    draws_t d;
    d.v[UI_PARAM_SCALE]          = 5;     /* mixolydian (widest name) */
    d.v[UI_PARAM_BAR_MS]         = 7600;
    d.v[UI_PARAM_GATE]           = 255;
    d.v[UI_PARAM_MOD_DEPTH]      = 8000;
    d.v[UI_PARAM_CUTOFF]         = 180;
    d.v[UI_PARAM_RESONANCE]      = 180;
    d.v[UI_PARAM_LFO_DEPTH]      = 255;
    d.v[UI_PARAM_FILTER_MODE]    = 3;     /* notch (widest name) */
    d.v[UI_PARAM_REVERB]         = 256;
    d.v[UI_PARAM_DELAY]          = 256;
    d.v[UI_PARAM_FEEDBACK]       = 200;
    d.v[UI_PARAM_COMP_THRESHOLD] = 30000;
    d.v[UI_PARAM_SWING]          = 100;
    apply_all(&d);
    for (int p = 0; p < UI_PARAM_COUNT; p++) ui_mark_param_set(p);
    keys_build_resume_line();
    char want[320];
    expected_line(want, sizeof want, 4294967295u, &d, 0x1FFF);
    ASSERT_TRUE(lines_equal(ui_get_resume_line(), want));
    /* The worst-case line the 320-byte buffers must hold: 231 chars
       + newline = 232 (233 with NUL) - the ui.c comment's "~233 B"
       is exact. Truncation anywhere breaks the exact match above;
       this pins the number itself. */
    ASSERT_EQ(strlen(ui_get_resume_line()), 232);
    gen_seed(0);                /* hygiene for any later test */
}

int main(void) {
    return RUN_ALL();
}
