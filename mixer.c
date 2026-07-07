#include "mixer.h"
#include "gen.h"
#include "voice.h"
#include "effects.h"
#include "audio_midi.h"

/* Master-bus mixer. Order matters: voices -> reverb -> delay ->
   soft saturation. Saturation runs last so peaks introduced by
   reverb or delay get smoothed before going to the output device.

   audio_midi_drain runs at the very top of the chunk so any MIDI
   events queued during the previous ~21 ms render window are
   consumed before voice_/gen_step iterate. This is the audio thread
   being the SINGLE CONSUMER of the MIDI SPSC ring buffer (per FR-033). */
void render_chunk(int16_t *out, uint32_t frames) {
    audio_midi_drain();
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
