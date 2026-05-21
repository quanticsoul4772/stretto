#include "voice.h"
#include "sin_table.h"
#include "env_table.h"
#include "note_table.h"
#include <stdint.h>

#define ENV_ATTACK_SAMPLES   220     /* ~5 ms  */
#define ENV_DECAY_SAMPLES    8820    /* ~200 ms */
#define ENV_RELEASE_SAMPLES  26460   /* ~600 ms */
#define ENV_SUSTAIN_LEVEL    16384   /* 50% of peak */
#define ENV_PEAK             32767

static uint32_t prng_state = 0xCAFEBABEu;

static int16_t prng_noise(void) {
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return (int16_t)(x >> 16);
}

void voice_init(Voice *v) {
    v->type = VOICE_OFF;
    v->note = 0;
    v->env_phase = ENV_OFF;
    v->env_amp = 0;
    v->env_time = 0;
}

void voice_trigger(Voice *v, uint8_t note, uint8_t type) {
    v->type = type;
    v->note = note;
    v->env_phase = ENV_A;
    v->env_time = 0;
    v->env_amp = 0;

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
        v->u.fm.inc_m     = note_phase_inc[note] * 2;
        v->u.fm.mod_depth = 6000;
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

    switch (v->env_phase) {
        case ENV_A: {
            uint32_t idx = ((uint32_t)v->env_time * 255u) / ENV_ATTACK_SAMPLES;
            if (idx > 255u) idx = 255u;
            amp = ((uint32_t)env_table[idx] * ENV_PEAK) / 255u;
            v->env_time++;
            if (v->env_time >= ENV_ATTACK_SAMPLES) {
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
            uint32_t idx = ((uint32_t)v->env_time * 255u) / ENV_RELEASE_SAMPLES;
            if (idx > 255u) idx = 255u;
            uint32_t curve = 255u - env_table[idx];
            amp = ((uint32_t)ENV_SUSTAIN_LEVEL * curve) / 255u;
            v->env_time++;
            if (v->env_time >= ENV_RELEASE_SAMPLES) {
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
    return (int16_t)(((int32_t)raw * env) >> 15);
}
