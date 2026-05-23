#include "wav.h"
#include "mixer.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void write_wav_header(FILE *f, uint32_t num_samples) {
    /* num_samples is per-channel frame count; stereo writes 4 bytes
       per frame (2 channels * 2 bytes). */
    uint32_t data_size   = num_samples * 4u;
    uint32_t file_size   = data_size + 36u;
    uint32_t fmt_size    = 16u;
    uint16_t audio_fmt   = 1u;
    uint16_t n_chan      = 2u;
    uint32_t sample_rate = SAMPLE_RATE;
    uint32_t byte_rate   = SAMPLE_RATE * 4u;
    uint16_t block_align = 4u;
    uint16_t bits        = 16u;

    fwrite("RIFF",     1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size,  4, 1, f);
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&n_chan,    2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits,      2, 1, f);
    fwrite("data",     1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

void render_wav(int seconds, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "open %s\n", path);
        exit(1);
    }
    uint32_t total = (uint32_t)SAMPLE_RATE * (uint32_t)seconds;
    write_wav_header(f, total);
    int16_t *buf = arena_alloc(BUFFER_FRAMES * 2 * sizeof(int16_t));
    uint32_t remaining = total;
    while (remaining > 0) {
        uint32_t n = remaining > BUFFER_FRAMES ? BUFFER_FRAMES : remaining;
        render_chunk(buf, n);
        fwrite(buf, 2, n * 2, f);
        remaining -= n;
    }
    fclose(f);
}
