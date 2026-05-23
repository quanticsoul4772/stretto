#ifndef CONFIG_H
#define CONFIG_H

/* Project-wide audio configuration.
 *
 * Constants that more than one module needs. Module-local constants
 * (e.g. DELAY_SAMPLES in effects.c, LATENCY_US in audio_pulse.c,
 * WAVE_BUFFERS in audio_winmm.c, N_VOICES in voice.h) stay with
 * the module that owns them. */

#define SAMPLE_RATE    48000  /* Hz, fixed across the entire pipeline */
#define BUFFER_FRAMES  1024   /* per render_chunk call; ~21 ms per buffer */

#endif
