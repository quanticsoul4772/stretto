#ifndef SECTION_H
#define SECTION_H

#include <stdint.h>

/* Song-section state machine.
 *
 * Cycles through 4 sections (INTRO -> BODY -> TENSION -> RESOLVE),
 * each SECTION_LEN_BARS bars long. Within a section, biases are
 * constant. Near boundaries (within SECTION_FADE_BARS / 2 of each
 * edge) continuous biases interpolate linearly between the adjacent
 * sections. Discrete-choice biases (kick pattern index, L-system
 * character index) switch instantly at the boundary.
 *
 * Pure function of bar count; no PRNG involvement. Deterministic
 * with --seed N.
 */

#define SECTION_PERIOD_BARS  96u    /* one full cycle */
#define SECTION_LEN_BARS     24u    /* per section */
#define SECTION_FADE_BARS     8u    /* total crossfade window (4 entering + 4 leaving) */

#define SEC_INTRO    0u
#define SEC_BODY     1u
#define SEC_TENSION  2u
#define SEC_RESOLVE  3u
#define SECTION_COUNT 4u

void        section_init(void);
void        section_step(uint32_t bar);
uint8_t     section_current(void);
const char *section_name(void);

/* Smoothed biases: continuous interpolation across boundaries. */
int8_t  section_bias_gate(void);
int8_t  section_bias_cutoff(void);
int8_t  section_bias_reverb(void);
int8_t  section_bias_mutation_interval(void);

/* Discrete biases: switch at section boundary. */
uint8_t section_kick_pattern(void);
uint8_t section_lsystem_character(void);
/* Chord voice type: VOICE_FM (cutting glassy) or VOICE_WT (animated
   pad). INTRO + BODY use WT; TENSION + RESOLVE use FM. */
uint8_t section_chord_voice_type(void);

#endif
