#ifndef GEN_H
#define GEN_H

#include <stdint.h>

void gen_init(void);
void gen_step(void);
void gen_force_mutate(void);
void gen_set_tempo(int delta_pct);
uint32_t gen_get_step_samples(void);
uint32_t gen_get_bar(void);
uint8_t  gen_get_step(void);

#endif
