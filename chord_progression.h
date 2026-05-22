#ifndef CHORD_PROGRESSION_H
#define CHORD_PROGRESSION_H

#include <stdint.h>

/* Probabilistic chord progressions via a Markov chain over scale-
   degree chord functions (I..vii or i..VII depending on mode).
   The current chord-function "root" advances once every two bars;
   chord triggers within that span share the same root.

   The module is one-way coupled to gen.c: the caller passes the
   current scale index and a pre-generated PRNG value into
   chord_progression_step(). The module never reads gen.c file-scope
   state.
*/

#define CHORD_N_DEGREES 7

void    chord_progression_init(void);
void    chord_progression_step(uint32_t rng, uint8_t scale);
uint8_t chord_progression_get_root(void);

#endif
