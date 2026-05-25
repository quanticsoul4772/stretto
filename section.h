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
/* Chord playback mode: 0 = block (3 simultaneous voices on chord beat),
   1 = arpeggio (one voice per arp step, cycling chord notes). TENSION
   arpeggiates for energy; other sections play blocks. */
uint8_t section_chord_arpeggio(void);

/* Voice-family bitmask. Schedulers check the current section's mask
   before triggering a voice family. INTRO uses a randomized 1-3 voice
   subset (curated combos), RESOLVE is drumless, BODY + TENSION use
   the full ensemble. */
#define VF_KICK    (1u << 0)
#define VF_SNARE   (1u << 1)
#define VF_HAT     (1u << 2)
#define VF_BASS    (1u << 3)
#define VF_CHORD   (1u << 4)
#define VF_MELODY  (1u << 5)   /* L-system main melody */
#define VF_COUNTER (1u << 6)   /* Markov counter-melody */
#define VF_ALL     0x7Fu

uint8_t section_voice_mask(void);

/* Select an INTRO combo (0..7, higher bits ignored). gen.c calls this
   on each cycle boundary with a PRNG draw so each INTRO is a different
   sparse palette. The setter is separate from gen.c's draw so tests
   can pin the combo deterministically. */
void section_set_intro_combo(uint8_t idx);

#endif
