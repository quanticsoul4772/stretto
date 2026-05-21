#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>

#define KS_MAX_LEN 512
#define N_VOICES   8

enum { VOICE_OFF = 0, VOICE_KS, VOICE_FM };
enum { ENV_OFF = 0, ENV_A, ENV_D, ENV_R };
enum { ROLE_BASS = 0, ROLE_CHORD, ROLE_MELODY };

typedef struct {
    uint8_t  type;
    uint8_t  note;
    uint8_t  env_phase;
    uint8_t  role;
    uint16_t env_amp;
    uint16_t env_time;
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
void    voice_pool_trigger(uint8_t note, uint8_t type);
void    voice_pool_trigger_role(uint8_t note, uint8_t type, uint8_t role);
int16_t voice_pool_mix(void);

void     voice_set_mod_depth(uint16_t d);
uint16_t voice_get_mod_depth(void);
uint32_t voice_pool_active_mask(void);

#endif
