#ifndef EFFECTS_H
#define EFFECTS_H

#include <stdint.h>

/* Master-bus stereo effects: delay, Schroeder reverb, soft saturation,
   and the shared int16 saturation helper. Operate in-place on
   interleaved L,R,L,R int16 buffers.

   Buffers are allocated from the arena in effects_init(); call once
   before any process function. */

void     effects_init(void);

void     delay_process(int16_t *buf, uint32_t frames);
void     delay_adjust_wet(int delta);
void     delay_adjust_feedback(int delta);
uint16_t delay_get_wet(void);
uint16_t delay_get_feedback(void);

void     reverb_process(int16_t *buf, uint32_t frames);
void     reverb_adjust_wet(int delta);
uint16_t reverb_get_wet(void);

/* Section-driven additive bias on reverb wet, applied per-sample in
   reverb_process. Replaces the old weak-stub plumbing that lived
   across main.c / section.c / gen.c. Pure exported function now. */
void     reverb_set_wet_bias(int8_t bias);

void     saturate_process(int16_t *buf, uint32_t frames);

/* int16 saturating clamp. Shared so voice.c can call it instead of
   the inline if/else duplicated in voice_step. */
int16_t  sat16(int32_t v);

#endif
