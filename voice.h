#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>

#define KS_MAX_LEN 512
#define N_VOICES   4

enum { VOICE_OFF = 0, VOICE_KS, VOICE_FM };
enum { ENV_OFF = 0, ENV_A, ENV_D, ENV_R };

typedef struct {
    uint8_t  type;
    uint8_t  note;
    uint8_t  env_phase;
    uint8_t  _pad;
    uint16_t env_amp;
    uint16_t env_time;
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
void    voice_trigger(Voice *v, uint8_t note, uint8_t type);
int16_t voice_step(Voice *v);

#endif
