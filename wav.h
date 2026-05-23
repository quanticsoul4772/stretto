#ifndef WAV_H
#define WAV_H

#include <stdint.h>

/* Render `seconds` of audio to a 16-bit stereo PCM WAV file at
   SAMPLE_RATE. Calls render_chunk() in BUFFER_FRAMES-sized blocks
   so output buffer stays bounded. exit(1) on file-open failure. */
void render_wav(int seconds, const char *path);

#endif
