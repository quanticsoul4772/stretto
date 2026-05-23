#ifndef MIXER_H
#define MIXER_H

#include <stdint.h>
#include "config.h"

/* Produce `frames` interleaved stereo int16 samples into `out`
   (sized 2*frames). Advances the generator, pulls the voice pool
   mix, and applies the master-bus chain: reverb -> delay -> soft
   saturation. Caller-owned buffer. */
void render_chunk(int16_t *out, uint32_t frames);

#endif
