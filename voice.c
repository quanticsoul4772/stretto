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

void voice_init(Voice *v) {
    v->type = VOICE_OFF;
    v->note = 0;
    v->env_phase = ENV_OFF;
    v->role = ROLE_MELODY;
    v->env_amp = 0;
    v->env_time = 0;
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
    int16_t mod = sin_table[v->u.fm.phase_m >> 22];
    v->u.fm.phase_m += v->u.fm.inc_m;
    uint32_t phase_with_mod = v->u.fm.phase_c + ((uint32_t)((int32_t)mod * v->u.fm.mod_depth) << 6);
    int16_t out = sin_table[phase_with_mod >> 22];
    v->u.fm.phase_c += v->u.fm.inc_c;
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
    return (int16_t)lp;
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

void voice_pool_trigger(uint8_t note, uint8_t type) {
    voice_trigger(&pool[pick_slot_range(0, N_VOICES)], note, type, ROLE_MELODY);
}

void voice_pool_trigger_role(uint8_t note, uint8_t type, uint8_t role) {
    int slot = pick_slot_range(role_slot_start[role], role_slot_end[role]);
    voice_trigger(&pool[slot], note, type, role);
}

int16_t voice_pool_mix(void) {
    int32_t sum = 0;
    for (int i = 0; i < N_VOICES; i++) sum += voice_step(&pool[i]);
    return (int16_t)(sum >> 3);
}

uint32_t voice_pool_active_mask(void) {
    uint32_t mask = 0;
    for (int i = 0; i < N_VOICES; i++) {
        if (pool[i].env_phase != ENV_OFF) mask |= (1u << i);
    }
    return mask;
}
