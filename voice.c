#include "voice.h"
#include "arena.h"
#include "sin_table.h"
#include "env_table.h"
#include "note_table.h"
#include <stdint.h>

#define ENV_ATTACK_SAMPLES   220
#define ENV_DECAY_SAMPLES    8820
#define ENV_RELEASE_SAMPLES  26460
#define ENV_SUSTAIN_LEVEL    16384
#define ENV_PEAK             32767

/* Per-voice peak normalization. After PEAK_WINDOW_SAMPLES of every
   trigger, each voice's output is scaled so its observed peak hits
   PEAK_TARGET. Gain is clamped between 1.0x and PEAK_GAIN_MAX (8.8 fp,
   so 1024 = 4.0x). */
#define PEAK_WINDOW_SAMPLES  2205    /* ~50 ms; covers attack + early decay */
#define PEAK_TARGET          16000   /* per-voice peak after normalization;
                                        chosen so 3-4 simultaneous voices
                                        after the >>3 mix divide produce
                                        mid-range output level */
#define PEAK_GAIN_MAX        1024    /* 4.0x in 8.8 fixed point */
#define PEAK_GAIN_UNITY      256     /* 1.0x in 8.8 fixed point */

/* SVF (2-pole Chamberlin) parameters. f=200, q=100 with >>8 shift
   gives ~5.6 kHz cutoff and Q ~ 2.56 at 44.1 kHz sample rate. */
#define SVF_F  200
#define SVF_Q  100

static uint32_t prng_state = 0xCAFEBABEu;
static uint16_t fm_mod_depth = 1500;

void voice_set_mod_depth(uint16_t d) {
    if (d < 100) d = 100;
    if (d > 8000) d = 8000;
    fm_mod_depth = d;
}

uint16_t voice_get_mod_depth(void) {
    return fm_mod_depth;
}

static int16_t prng_noise(void) {
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return (int16_t)(x >> 16);
}

/* Per-role parameters: BASS, CHORD, MELODY */
static const uint16_t role_mod_depth[3]  = {  200, 1500, 1500 };
static const uint8_t  role_fm_ratio[3]   = {    1,    2,    2 };
static const uint16_t role_attack[3]     = { 2205,  882,  220 }; /* 50 / 20 / 5 ms */
static const uint16_t role_release[3]    = {44100,26460,26460 }; /* 1000 / 600 / 600 ms */

/* Per-voice-slot base pan position (0 = full left, 128 = center,
   255 = full right). Bass slots 0-1 sit at center; chord slots 2-4
   spread across the field; melody slots 5-7 lean to the outer zones
   for the widest stage. */
static const uint8_t slot_base_pan[N_VOICES] = {
    128, 128,           /* bass: center */
     72, 128, 184,      /* chord: L, C, R */
     56, 200,  96,      /* melody: alternating outer */
};

/* Per-voice-slot LFO increment for slow pan motion. Numbers are
   uint32 phase-increments at 44.1 kHz; freqs ~0.07-0.18 Hz so the
   modulation period is several seconds. */
static const uint32_t slot_lfo_inc[N_VOICES] = {
     6818,  9738,       /* bass: 0.07, 0.10 Hz */
    10711,  8276, 12166,/* chord: 0.11, 0.085, 0.125 Hz */
    14606, 11684, 17527,/* melody: 0.15, 0.12, 0.18 Hz */
};

/* Pan jitter range per role (centered): bass tight, chord medium,
   melody wide. */
static const uint8_t role_pan_jitter[3] = { 16, 32, 48 };

void voice_init(Voice *v) {
    v->type = VOICE_OFF;
    v->note = 0;
    v->env_phase = ENV_OFF;
    v->role = ROLE_MELODY;
    v->pan = 128;
    v->env_amp = 0;
    v->env_time = 0;
    v->lfo_phase = 0;
    v->lfo_inc = 0;
    v->peak_seen = 1;
    v->gain = PEAK_GAIN_UNITY;
    v->peak_window = 0;
    v->svf_lp = 0;
    v->svf_bp = 0;
}

void voice_trigger(Voice *v, uint8_t note, uint8_t type, uint8_t role) {
    v->type = type;
    v->note = note;
    v->role = role;
    v->env_phase = ENV_A;
    v->env_time = 0;
    v->env_amp = 0;
    v->svf_lp = 0;
    v->svf_bp = 0;
    /* Reset peak detection for this trigger. Starts at 1.0x gain;
       gain will decrease as the voice's peak grows during the window. */
    v->peak_seen = 1;
    v->gain = PEAK_GAIN_UNITY;
    v->peak_window = PEAK_WINDOW_SAMPLES;

    if (type == VOICE_KS) {
        v->u.ks.len = note_ks_len[note];
        v->u.ks.idx = 0;
        uint16_t len = v->u.ks.len;
        for (uint16_t i = 0; i < len; i++) {
            v->u.ks.buf[i] = (int16_t)(prng_noise() >> 1);
        }
    } else {
        v->u.fm.phase_c   = 0;
        v->u.fm.phase_m   = 0;
        v->u.fm.inc_c     = note_phase_inc[note];
        v->u.fm.inc_m     = note_phase_inc[note] * role_fm_ratio[role];
        v->u.fm.mod_depth = (role == ROLE_MELODY) ? fm_mod_depth : role_mod_depth[role];
    }
}

static int16_t ks_step(Voice *v) {
    uint16_t idx  = v->u.ks.idx;
    uint16_t len  = v->u.ks.len;
    uint16_t next = idx + 1;
    if (next >= len) next = 0;
    int16_t a = v->u.ks.buf[idx];
    int16_t b = v->u.ks.buf[next];
    int16_t avg = (int16_t)((((int32_t)a + b) * 32440) >> 16);
    v->u.ks.buf[idx] = avg;
    v->u.ks.idx = next;
    return a;
}

static int16_t fm_step(Voice *v) {
    /* Re-use the per-voice pan LFO to also detune the carrier and
       modulator frequencies. Same scale on both so the FM ratio
       stays constant. Peak excursion: lfo (+/-24576) * inc / 2^23 ~=
       inc * 0.29%, about 5 cents at LFO peak - subtle chorus
       motion. Uses int64 for the multiply to avoid overflow on
       higher-pitched notes. */
    int16_t lfo = sin_table[v->lfo_phase >> 22];
    int32_t det_m = (int32_t)(((int64_t)v->u.fm.inc_m * lfo) >> 23);
    int32_t det_c = (int32_t)(((int64_t)v->u.fm.inc_c * lfo) >> 23);

    int16_t mod = sin_table[v->u.fm.phase_m >> 22];
    v->u.fm.phase_m += (uint32_t)((int32_t)v->u.fm.inc_m + det_m);
    uint32_t phase_with_mod = v->u.fm.phase_c + ((uint32_t)((int32_t)mod * v->u.fm.mod_depth) << 6);
    int16_t out = sin_table[phase_with_mod >> 22];
    v->u.fm.phase_c += (uint32_t)((int32_t)v->u.fm.inc_c + det_c);
    return out;
}

static uint16_t env_step(Voice *v) {
    uint32_t amp = v->env_amp;
    uint16_t attack_n  = role_attack[v->role];
    uint16_t release_n = role_release[v->role];

    switch (v->env_phase) {
        case ENV_A: {
            uint32_t idx = ((uint32_t)v->env_time * 255u) / attack_n;
            if (idx > 255u) idx = 255u;
            amp = ((uint32_t)env_table[idx] * ENV_PEAK) / 255u;
            v->env_time++;
            if (v->env_time >= attack_n) {
                v->env_phase = ENV_D;
                v->env_time = 0;
                amp = ENV_PEAK;
            }
            break;
        }
        case ENV_D: {
            uint32_t idx = ((uint32_t)v->env_time * 255u) / ENV_DECAY_SAMPLES;
            if (idx > 255u) idx = 255u;
            uint32_t curve = 255u - env_table[idx];
            amp = ENV_SUSTAIN_LEVEL + ((uint32_t)(ENV_PEAK - ENV_SUSTAIN_LEVEL) * curve) / 255u;
            v->env_time++;
            if (v->env_time >= ENV_DECAY_SAMPLES) {
                v->env_phase = ENV_R;
                v->env_time = 0;
                amp = ENV_SUSTAIN_LEVEL;
            }
            break;
        }
        case ENV_R: {
            uint32_t idx = ((uint32_t)v->env_time * 255u) / release_n;
            if (idx > 255u) idx = 255u;
            uint32_t curve = 255u - env_table[idx];
            amp = ((uint32_t)ENV_SUSTAIN_LEVEL * curve) / 255u;
            v->env_time++;
            if (v->env_time >= release_n) {
                v->env_phase = ENV_OFF;
                v->type = VOICE_OFF;
                amp = 0;
            }
            break;
        }
        default:
            amp = 0;
    }

    v->env_amp = (uint16_t)amp;
    return (uint16_t)amp;
}

int16_t voice_step(Voice *v) {
    if (v->env_phase == ENV_OFF) return 0;
    int16_t raw = (v->type == VOICE_KS) ? ks_step(v) : fm_step(v);
    uint16_t env = env_step(v);
    int16_t shaped = (int16_t)(((int32_t)raw * env) >> 15);

    int32_t hp = shaped - v->svf_lp - ((v->svf_bp * SVF_Q) >> 8);
    int32_t bp = v->svf_bp + ((hp * SVF_F) >> 8);
    int32_t lp = v->svf_lp + ((bp * SVF_F) >> 8);
    v->svf_bp = bp;
    v->svf_lp = lp;

    if (lp > 32767) lp = 32767;
    else if (lp < -32768) lp = -32768;

    /* Peak-normalize. During the measurement window, observe |lp| and
       recompute gain whenever a new peak is found. Gain can be below
       1.0x (loud voices like chord get attenuated) or above 1.0x (quiet
       voices like bass get boosted, up to PEAK_GAIN_MAX). The peak grows
       monotonically and gain decreases with it, so the gain ramp is
       smooth (no clicks). After the window, gain is fixed for the rest
       of the voice's life. */
    int32_t abs_lp = lp < 0 ? -lp : lp;
    if (v->peak_window > 0) {
        if ((uint16_t)abs_lp > v->peak_seen) {
            v->peak_seen = (uint16_t)(abs_lp > 0xFFFF ? 0xFFFF : abs_lp);
            uint32_t g = ((uint32_t)PEAK_TARGET * PEAK_GAIN_UNITY) / v->peak_seen;
            if (g > PEAK_GAIN_MAX) g = PEAK_GAIN_MAX;
            v->gain = (uint16_t)g;
        }
        v->peak_window--;
    }
    int32_t scaled = ((int32_t)lp * v->gain) >> 8;
    if (scaled > 32767) scaled = 32767;
    else if (scaled < -32768) scaled = -32768;
    return (int16_t)scaled;
}

static Voice *pool;

void voice_pool_init(void) {
    pool = arena_alloc(N_VOICES * sizeof(Voice));
    for (int i = 0; i < N_VOICES; i++) voice_init(&pool[i]);
}

/* Voice slot ranges per role: bass = 0..1, chord = 2..4, melody = 5..7. */
static const uint8_t role_slot_start[3] = { 0, 2, 5 };
static const uint8_t role_slot_end[3]   = { 2, 5, 8 };

static int pick_slot_range(uint8_t lo, uint8_t hi) {
    for (int i = lo; i < hi; i++) {
        if (pool[i].env_phase == ENV_OFF) return i;
    }
    int chosen = lo;
    uint16_t min_amp = 0xFFFFu;
    for (int i = lo; i < hi; i++) {
        if (pool[i].env_phase == ENV_R && pool[i].env_amp < min_amp) {
            min_amp = pool[i].env_amp;
            chosen = i;
        }
    }
    return chosen;
}

void voice_pool_trigger_role(uint8_t note, uint8_t type, uint8_t role) {
    int slot = pick_slot_range(role_slot_start[role], role_slot_end[role]);
    voice_trigger(&pool[slot], note, type, role);

    /* Place this voice on the stage: base pan from its slot, plus a
       random jitter within the role's range. PRNG state advances here
       so determinism is preserved across runs of the same binary. */
    int base = slot_base_pan[slot];
    int jitter_range = role_pan_jitter[role];
    int jitter = (int)(prng_noise() >> 8);    /* int16 -> roughly -128..127 */
    int j = (jitter * jitter_range) / 128;     /* scale to +/- range */
    int p = base + j;
    if (p < 0) p = 0;
    else if (p > 255) p = 255;
    pool[slot].pan = (uint8_t)p;
    pool[slot].lfo_phase = 0;
    pool[slot].lfo_inc = slot_lfo_inc[slot];
}

Stereo voice_pool_mix(void) {
    int32_t sum_l = 0;
    int32_t sum_r = 0;
    for (int i = 0; i < N_VOICES; i++) {
        Voice *v = &pool[i];
        int16_t s = voice_step(v);
        if (s == 0 && v->env_phase == ENV_OFF) continue;

        /* Slow LFO modulates pan around its base. sin_table entry is
           +/-24576; >> 9 gives roughly +/-48 pan units of drift. */
        v->lfo_phase += v->lfo_inc;
        int lfo = sin_table[v->lfo_phase >> 22];
        int p = (int)v->pan + (lfo >> 9);
        if (p < 0) p = 0;
        else if (p > 255) p = 255;

        /* Linear pan: L gain = (255 - p), R gain = p, divide by 255
           via >> 8 (treats 255 as 256; -6 dB center is acceptable). */
        sum_l += ((int32_t)s * (255 - p)) >> 8;
        sum_r += ((int32_t)s * p) >> 8;
    }
    Stereo out;
    out.l = (int16_t)(sum_l >> 3);
    out.r = (int16_t)(sum_r >> 3);
    return out;
}

uint32_t voice_pool_active_mask(void) {
    uint32_t mask = 0;
    for (int i = 0; i < N_VOICES; i++) {
        if (pool[i].env_phase != ENV_OFF) mask |= (1u << i);
    }
    return mask;
}
