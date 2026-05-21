#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>

#define KS_MAX_LEN 512
#define N_VOICES   8

enum { VOICE_OFF = 0, VOICE_KS, VOICE_FM };
enum { ENV_OFF = 0, ENV_A, ENV_D, ENV_R };
enum { ROLE_BASS = 0, ROLE_CHORD, ROLE_MELODY };

typedef struct {
    int16_t l;
    int16_t r;
} Stereo;

typedef struct {
    uint8_t  type;
    uint8_t  note;
    uint8_t  env_phase;
    uint8_t  role;
    /* Stereo placement: 0 = full left, 128 = center, 255 = full right.
       LFO modulates around this base for slow continuous movement. */
    uint8_t  pan;
    uint8_t  _pad[3];
    uint16_t env_amp;
    uint16_t env_time;
    uint32_t lfo_phase;
    uint32_t lfo_inc;
    /* Per-voice peak normalization. peak_seen tracks max |output| during
       the first PEAK_WINDOW samples; gain is recomputed each time a new
       peak is found so it monotonically decreases as the peak grows.
       After window, gain stays fixed for the rest of the voice's life. */
    uint16_t peak_seen;
    uint16_t gain;            /* 8.8 fixed: 256 = 1.0x, 1024 = 4.0x cap */
    uint16_t peak_window;     /* samples remaining in the measurement window */
    uint16_t _norm_pad;
    /* SVF state is int32, not int16. At Q ~ 2.56 (q=100, damp=q/256),
       resonance can ring the filter state to roughly 2.5x input
       amplitude; int16 would wrap and produce broadband clicks. */
    int32_t  svf_lp;
    int32_t  svf_bp;
    union {
        struct {
            int16_t  buf[KS_MAX_LEN];
            uint16_t idx;
            uint16_t len;
        } ks;
        struct {
            uint32_t phase_c;
            uint32_t phase_m;
            uint32_t inc_c;
            uint32_t inc_m;
            uint16_t mod_depth;
        } fm;
    } u;
} Voice;

void    voice_init(Voice *v);
void    voice_trigger(Voice *v, uint8_t note, uint8_t type, uint8_t role);
int16_t voice_step(Voice *v);

void    voice_pool_init(void);
void    voice_pool_trigger_role(uint8_t note, uint8_t type, uint8_t role);
Stereo  voice_pool_mix(void);

void     voice_set_mod_depth(uint16_t d);
uint16_t voice_get_mod_depth(void);
uint32_t voice_pool_active_mask(void);

#endif
