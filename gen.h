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
/* Maps a scale index (0..N_SCALES-1) + letter-degree index (0..6) to an
   absolute MIDI note number. Used by audio_midi.c's drain to apply the
   FR-010 scale-degree mapping live (vs. the static semitone-offset
   helper in tests/unit/test_midi.c which tests the same formula).
   Bounds-checked: out-of-range scale wraps to 0, out-of-range degree
   is taken modulo 7. */
uint8_t  gen_get_scale_note(uint8_t scale_idx, uint8_t degree);
void     gen_adjust_gate(int delta);
uint8_t  gen_get_gate(void);
uint8_t  gen_get_degree(void);
uint8_t  gen_get_active_mask(void);
uint8_t  gen_get_chord_pattern(void);
uint8_t  gen_get_chord_root(void);
const char *gen_get_section_name(void);
uint8_t  gen_get_tension(void);
int      gen_motif_replaying(void);

#endif
