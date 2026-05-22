#ifndef GEN_H
#define GEN_H

#include <stdint.h>

void gen_seed(uint32_t seed);
void gen_init(void);
void gen_step(void);
void gen_force_mutate(void);
void gen_set_tempo(int delta_pct);
uint32_t gen_get_step_samples(void);
uint32_t gen_get_bar(void);
uint8_t  gen_get_step(void);
void     gen_cycle_scale(void);
uint8_t  gen_get_scale(void);
void     gen_adjust_gate(int delta);
uint8_t  gen_get_gate(void);
uint8_t  gen_get_degree(void);
uint8_t  gen_get_active_mask(void);
uint8_t  gen_get_chord_pattern(void);

#endif
