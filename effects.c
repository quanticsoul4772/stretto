#include "effects.h"
#include "arena.h"
#include <stdint.h>
#include <string.h>

/* ---- int16 saturating clamp ---------------------------------- */

int16_t sat16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* ---- master-bus stereo delay --------------------------------- */
/* Two independent mono buffers, 250 ms long at 48 kHz. Standard
   feed-forward + feedback topology: out = dry + tap*wet; buffer-write
   = dry + tap*feedback. */
#define DELAY_SAMPLES  12000u

static int16_t *delay_l;
static int16_t *delay_r;
static uint32_t delay_idx      = 0;
static uint16_t delay_wet      = 100;  /* 0..256, mix amount */
static uint16_t delay_feedback = 140;  /* 0..200, capped to avoid runaway */

static int16_t *alloc_zero(uint32_t n_samples) {
    int16_t *p = arena_alloc(n_samples * sizeof(int16_t));
    memset(p, 0, n_samples * sizeof(int16_t));
    return p;
}

static void delay_init(void) {
    /* Idempotent: arena-allocate buffers on first call; subsequent
       calls just reset the index and clear the existing buffers. */
    if (!delay_l) {
        delay_l = alloc_zero(DELAY_SAMPLES);
        delay_r = alloc_zero(DELAY_SAMPLES);
    } else {
        memset(delay_l, 0, DELAY_SAMPLES * sizeof(int16_t));
        memset(delay_r, 0, DELAY_SAMPLES * sizeof(int16_t));
    }
    delay_idx = 0;
}

void delay_process(int16_t *buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int32_t dry_l = buf[2 * i];
        int32_t dry_r = buf[2 * i + 1];
        int32_t tap_l = delay_l[delay_idx];
        int32_t tap_r = delay_r[delay_idx];

        int32_t out_l = dry_l + ((tap_l * (int32_t)delay_wet) >> 8);
        int32_t out_r = dry_r + ((tap_r * (int32_t)delay_wet) >> 8);

        int32_t fb_l = dry_l + ((tap_l * (int32_t)delay_feedback) >> 8);
        int32_t fb_r = dry_r + ((tap_r * (int32_t)delay_feedback) >> 8);

        delay_l[delay_idx] = sat16(fb_l);
        delay_r[delay_idx] = sat16(fb_r);

        if (++delay_idx >= DELAY_SAMPLES) delay_idx = 0;

        buf[2 * i]     = sat16(out_l);
        buf[2 * i + 1] = sat16(out_r);
    }
}

void delay_adjust_wet(int delta) {
    int v = (int)delay_wet + delta;
    if (v < 0)   v = 0;
    if (v > 256) v = 256;
    delay_wet = (uint16_t)v;
}

void delay_adjust_feedback(int delta) {
    int v = (int)delay_feedback + delta;
    if (v < 0)   v = 0;
    if (v > 200) v = 200;
    delay_feedback = (uint16_t)v;
}

uint16_t delay_get_wet(void)      { return delay_wet; }
uint16_t delay_get_feedback(void) { return delay_feedback; }

/* ---- Schroeder reverb ---------------------------------------- */
/* 4 parallel comb filters per channel, outputs summed and passed
   through 2 series all-pass filters per channel. Prime-number
   delays (Schroeder 1962, rescaled by 48000/44100) avoid metallic
   resonance. Slightly different L vs R delays preserve stereo
   separation in the tail. */
#define REV_C1L 1693
#define REV_C2L 1759
#define REV_C3L 1621
#define REV_C4L 1549
#define REV_C1R 1721
#define REV_C2R 1747
#define REV_C3R 1613
#define REV_C4R 1571
#define REV_AP1L 241
#define REV_AP2L 607
#define REV_AP1R 251
#define REV_AP2R 613
#define COMB_G   180             /* ~0.70 in 8.8 fixed, RT60 ~1.5 s */
#define AP_G     180

static int16_t *rev_c1l, *rev_c2l, *rev_c3l, *rev_c4l;
static int16_t *rev_c1r, *rev_c2r, *rev_c3r, *rev_c4r;
static int16_t *rev_ap1l, *rev_ap2l;
static int16_t *rev_ap1r, *rev_ap2r;
static uint16_t i_c1l, i_c2l, i_c3l, i_c4l;
static uint16_t i_c1r, i_c2r, i_c3r, i_c4r;
static uint16_t i_ap1l, i_ap2l, i_ap1r, i_ap2r;

static uint16_t reverb_wet      = 60;   /* 0..256, mix amount */
static int8_t   reverb_wet_bias = 0;    /* section-driven additive bias */

static void reverb_init(void) {
    /* Idempotent: arena-allocate comb/all-pass buffers on first call;
       subsequent calls clear the existing buffers and reset cursors. */
    if (!rev_c1l) {
        rev_c1l = alloc_zero(REV_C1L);  rev_c1r = alloc_zero(REV_C1R);
        rev_c2l = alloc_zero(REV_C2L);  rev_c2r = alloc_zero(REV_C2R);
        rev_c3l = alloc_zero(REV_C3L);  rev_c3r = alloc_zero(REV_C3R);
        rev_c4l = alloc_zero(REV_C4L);  rev_c4r = alloc_zero(REV_C4R);
        rev_ap1l = alloc_zero(REV_AP1L); rev_ap1r = alloc_zero(REV_AP1R);
        rev_ap2l = alloc_zero(REV_AP2L); rev_ap2r = alloc_zero(REV_AP2R);
    } else {
        memset(rev_c1l, 0, REV_C1L * 2); memset(rev_c1r, 0, REV_C1R * 2);
        memset(rev_c2l, 0, REV_C2L * 2); memset(rev_c2r, 0, REV_C2R * 2);
        memset(rev_c3l, 0, REV_C3L * 2); memset(rev_c3r, 0, REV_C3R * 2);
        memset(rev_c4l, 0, REV_C4L * 2); memset(rev_c4r, 0, REV_C4R * 2);
        memset(rev_ap1l, 0, REV_AP1L * 2); memset(rev_ap1r, 0, REV_AP1R * 2);
        memset(rev_ap2l, 0, REV_AP2L * 2); memset(rev_ap2r, 0, REV_AP2R * 2);
    }
    i_c1l = i_c2l = i_c3l = i_c4l = 0;
    i_c1r = i_c2r = i_c3r = i_c4r = 0;
    i_ap1l = i_ap2l = i_ap1r = i_ap2r = 0;
}

/* y[n] = x[n-D] + g*y[n-D] using delay-line as recirculating buffer. */
static inline int16_t comb_step(int16_t *buf, uint16_t size, uint16_t *idx, int16_t in) {
    int32_t tap = buf[*idx];
    int32_t w = in + ((tap * COMB_G) >> 8);
    buf[*idx] = sat16(w);
    *idx = (uint16_t)((*idx + 1) % size);
    return (int16_t)tap;
}

/* All-pass: same magnitude at every freq, shifts phases. Smooths
   the dense comb output into a continuous tail. */
static inline int16_t ap_step(int16_t *buf, uint16_t size, uint16_t *idx, int16_t in) {
    int32_t tap = buf[*idx];
    int32_t y = tap - ((in * AP_G) >> 8);
    int32_t w = in + ((tap * AP_G) >> 8);
    buf[*idx] = sat16(w);
    *idx = (uint16_t)((*idx + 1) % size);
    return sat16(y);
}

void reverb_process(int16_t *buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int16_t in_l = buf[2 * i];
        int16_t in_r = buf[2 * i + 1];

        int32_t sum_l = (int32_t)comb_step(rev_c1l, REV_C1L, &i_c1l, in_l)
                      + comb_step(rev_c2l, REV_C2L, &i_c2l, in_l)
                      + comb_step(rev_c3l, REV_C3L, &i_c3l, in_l)
                      + comb_step(rev_c4l, REV_C4L, &i_c4l, in_l);
        sum_l >>= 2;
        int32_t sum_r = (int32_t)comb_step(rev_c1r, REV_C1R, &i_c1r, in_r)
                      + comb_step(rev_c2r, REV_C2R, &i_c2r, in_r)
                      + comb_step(rev_c3r, REV_C3R, &i_c3r, in_r)
                      + comb_step(rev_c4r, REV_C4R, &i_c4r, in_r);
        sum_r >>= 2;

        int16_t ap_l = ap_step(rev_ap1l, REV_AP1L, &i_ap1l, (int16_t)sum_l);
                ap_l = ap_step(rev_ap2l, REV_AP2L, &i_ap2l, ap_l);
        int16_t ap_r = ap_step(rev_ap1r, REV_AP1R, &i_ap1r, (int16_t)sum_r);
                ap_r = ap_step(rev_ap2r, REV_AP2R, &i_ap2r, ap_r);

        /* Apply section bias on top of user wet, clamped to [0, 256]. */
        int eff_wet = (int)reverb_wet + (int)reverb_wet_bias;
        if (eff_wet < 0)   eff_wet = 0;
        if (eff_wet > 256) eff_wet = 256;
        int32_t out_l = in_l + ((ap_l * eff_wet) >> 8);
        int32_t out_r = in_r + ((ap_r * eff_wet) >> 8);
        buf[2 * i]     = sat16(out_l);
        buf[2 * i + 1] = sat16(out_r);
    }
}

void reverb_adjust_wet(int delta) {
    int v = (int)reverb_wet + delta;
    if (v < 0)   v = 0;
    if (v > 256) v = 256;
    reverb_wet = (uint16_t)v;
}

uint16_t reverb_get_wet(void)               { return reverb_wet; }
void     reverb_set_wet_bias(int8_t bias)   { reverb_wet_bias = bias; }

/* ---- soft saturation ----------------------------------------- */
/* Cubic soft-clip: y = x - x^3 / 2^31. Linear for small x, smoothly
   compresses peaks. At full-scale int16 input (~32767) the output
   is ~50%; at typical levels (10-20% of full scale) the change is
   sub-1%. Adds gentle analog-tape warmth without affecting quiet
   signal character. */
static inline int16_t soft_sat(int16_t x) {
    int64_t x3 = (int64_t)x * x * x;
    int32_t cubic = (int32_t)(x3 >> 31);
    int32_t y = (int32_t)x - cubic;
    return sat16(y);
}

void saturate_process(int16_t *buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        buf[2 * i]     = soft_sat(buf[2 * i]);
        buf[2 * i + 1] = soft_sat(buf[2 * i + 1]);
    }
}

/* ---- master compressor + brickwall limiter ------------------- */
/* Feed-forward, stereo-linked. Envelope tracks max(|L|,|R|) so the
   gain reduction is identical on both channels and stereo imaging
   is preserved. 4:1 ratio above threshold, attack ~5 ms and release
   ~200 ms at 48 kHz, +1 dB makeup gain, brickwall ceiling just
   below int16 max so sat16 still has margin. */
#define COMP_THRESHOLD_DEFAULT  20000        /* int16 amplitude */
#define COMP_THRESHOLD_MIN       8000
#define COMP_THRESHOLD_MAX      30000
#define COMP_RATIO                  4        /* 4:1 above threshold */
#define COMP_ATTACK_COEF          268        /* (1-exp(-1/(0.005*48000)))*65536 */
#define COMP_RELEASE_COEF           7        /* (1-exp(-1/(0.200*48000)))*65536 */
#define COMP_MAKEUP_GAIN          288        /* ~+1 dB in 8.8 fixed */
#define LIMIT_CEILING           32000        /* brickwall, below int16 max */

static int32_t comp_env       = 0;
static int32_t comp_threshold = COMP_THRESHOLD_DEFAULT;

void compressor_process(int16_t *buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int32_t l = buf[2 * i];
        int32_t r = buf[2 * i + 1];
        int32_t al = l < 0 ? -l : l;
        int32_t ar = r < 0 ? -r : r;
        int32_t a  = al > ar ? al : ar;

        /* Asymmetric envelope follower: fast attack, slow release. */
        if (a > comp_env) {
            comp_env += ((a - comp_env) * COMP_ATTACK_COEF) >> 16;
        } else {
            comp_env -= ((comp_env - a) * COMP_RELEASE_COEF) >> 16;
        }

        /* Gain reduction: 1.0 below threshold, compressed above.
           gain is 8.8 fixed; target_env = threshold + over/ratio. */
        int32_t gain = 256;
        if (comp_env > comp_threshold) {
            int32_t over   = comp_env - comp_threshold;
            int32_t target = comp_threshold + over / COMP_RATIO;
            gain = (target * 256) / comp_env;
        }

        /* Apply gain + makeup, then brickwall. */
        int32_t yl = (l * gain) >> 8;
        int32_t yr = (r * gain) >> 8;
        yl = (yl * COMP_MAKEUP_GAIN) >> 8;
        yr = (yr * COMP_MAKEUP_GAIN) >> 8;
        if (yl >  LIMIT_CEILING) yl =  LIMIT_CEILING;
        if (yl < -LIMIT_CEILING) yl = -LIMIT_CEILING;
        if (yr >  LIMIT_CEILING) yr =  LIMIT_CEILING;
        if (yr < -LIMIT_CEILING) yr = -LIMIT_CEILING;

        buf[2 * i]     = sat16(yl);
        buf[2 * i + 1] = sat16(yr);
    }
}

void compressor_adjust_threshold(int delta) {
    int v = (int)comp_threshold + delta;
    if (v < COMP_THRESHOLD_MIN) v = COMP_THRESHOLD_MIN;
    if (v > COMP_THRESHOLD_MAX) v = COMP_THRESHOLD_MAX;
    comp_threshold = v;
}

uint16_t compressor_get_threshold(void) {
    return (uint16_t)comp_threshold;
}

/* ---- one-shot init ------------------------------------------- */

void effects_init(void) {
    delay_init();
    reverb_init();
    comp_env       = 0;
    comp_threshold = COMP_THRESHOLD_DEFAULT;
}
