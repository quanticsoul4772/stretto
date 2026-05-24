#include "mixer.h"
#include "gen.h"
#include "voice.h"
#include "effects.h"

/* Master-bus mixer. Order matters: voices -> reverb -> delay ->
   soft saturation. Saturation runs last so peaks introduced by
   reverb or delay get smoothed before going to the output device. */
void render_chunk(int16_t *out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        gen_step();
        Stereo s = voice_pool_mix();
        out[2 * i]     = s.l;
        out[2 * i + 1] = s.r;
    }
    reverb_process(out, frames);
    delay_process(out, frames);
    saturate_process(out, frames);
    compressor_process(out, frames);
}
