#include "density.h"
#include <stdint.h>

static uint8_t tension = DENSITY_TENSION_MID;

/* popcount of the low 7 bits of an 8-bit value. */
static uint8_t popcount7(uint8_t v) {
    v &= 0x7Fu;
    uint8_t n = 0;
    while (v) { n += v & 1u; v >>= 1; }
    return n;
}

void density_update(uint8_t active_mask, uint8_t gate_prob) {
    /* Tension = active-degree count weighted high + gate weighted
       moderate. Empirical scaling: full mask (7) + max gate (255)
       sums to ~189, near the upper end of the 0..255 range; one
       degree + minimum gate (32) sums to ~26. The MID constant
       (128) sits roughly midway. */
    uint16_t t = (uint16_t)(popcount7(active_mask) * 18u)
               + (uint16_t)(gate_prob >> 2);
    if (t > 255u) t = 255u;
    tension = (uint8_t)t;
}

uint8_t density_get_tension(void) {
    return tension;
}

int8_t density_bias_gate(void) {
    /* Counter-cyclical: above center pulls gate down, below center
       pushes gate up. Divide by 8 for a modest +/-16 swing. */
    int delta = ((int)DENSITY_TENSION_MID - (int)tension) / 8;
    return (int8_t)delta;
}

int8_t density_bias_reverb(void) {
    /* Same direction, larger magnitude. High tension = drier (less
       wash to keep punch); low tension = washier (texture). */
    int delta = ((int)DENSITY_TENSION_MID - (int)tension) / 4;
    return (int8_t)delta;
}
