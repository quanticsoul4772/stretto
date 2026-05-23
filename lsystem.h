#ifndef LSYSTEM_H
#define LSYSTEM_H

#include <stdint.h>

/* Lindenmayer-system melodic phrase generator. Replaces the
   Markov walker on the main melody. The counter-melody keeps its
   independent Markov walker so the two voices contrast (phrased
   vs walked).

   Operation:
     - The L-system holds a static rule set ("character") plus an
       axiom. lsystem_reset() rewrites the axiom for a fixed number
       of generations, producing an output string up to 256 symbols.
     - lsystem_next(active_mask) reads the next symbol, advances a
       running scale-degree pointer, snaps to the nearest in-mask
       degree, and returns it. Wraps the buffer and re-expands when
       the cursor reaches the end.
     - The 'rest' symbol returns LSYSTEM_REST so the caller can
       skip the melody trigger for breathing room.
     - lsystem_mutate(rng) drifts the rule set / axiom / character
       index so the melodic style evolves over the piece.
*/

#define LSYSTEM_REST 0xFFu     /* sentinel returned by lsystem_next for the '.' symbol */

void    lsystem_reset(void);
uint8_t lsystem_next(uint8_t active_mask);
void    lsystem_mutate(uint32_t rng);

/* Force the current grammar character (0..2). If different from the
   current character, re-expands the output buffer immediately so
   the next phrase uses the new grammar. No-op if already set. */
void    lsystem_set_character(uint8_t idx);

#endif
