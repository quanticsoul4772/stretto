#ifndef DENSITY_H
#define DENSITY_H

#include <stdint.h>

/* Adaptive density. Derives a per-bar "tension" scalar from the
   current active-degree mask + gate probability and biases gate and
   reverb wet counter-cyclically. Effect: when the texture is already
   busy, density is pulled back so peaks stay legible; when sparse,
   density is nudged up so calm passages still have presence. Energy
   self-balances over bar timescales.
 *
 * Composes with section.c (long-term, 96-bar) - section bias is
 * additive, density bias is additive on top, both clamped at the
 * call sites in gen.c (gate, effects.c (reverb wet).
 */

#define DENSITY_TENSION_MID 128u   /* tension center; biases are zero here */

/* Recompute tension from the current bar's active mask + gate
   probability. Called from gen.c at each bar boundary. */
void    density_update(uint8_t active_mask, uint8_t gate_prob);

/* Latest tension value, 0..255. 0 = sparsest (one degree, low gate),
   ~190 typical peak (all 7 degrees + max gate). For the status row. */
uint8_t density_get_tension(void);

/* Signed deltas applied on top of user gate / reverb-wet values
   (already biased by section). Counter-cyclical: high tension =
   negative bias (pull back), low tension = positive bias (fill in).
   Magnitudes kept modest so the effect is felt over bars, not jarring. */
int8_t  density_bias_gate(void);    /* approx +/-16 */
int8_t  density_bias_reverb(void);  /* approx +/-32 */

#endif
