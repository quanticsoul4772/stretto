#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <alsa/asoundlib.h>

#include "arena.h"
#include "voice.h"
#include "gen.h"

#define SAMPLE_RATE    44100
#define BUFFER_FRAMES  1024
#define LATENCY_US     100000

static void render_chunk(int16_t *out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        gen_step();
        out[i] = voice_pool_mix();
    }
}

static void write_wav_header(FILE *f, uint32_t num_samples) {
    uint32_t data_size   = num_samples * 2u;
    uint32_t file_size   = data_size + 36u;
    uint32_t fmt_size    = 16u;
    uint16_t audio_fmt   = 1u;
    uint16_t n_chan      = 1u;
    uint32_t sample_rate = SAMPLE_RATE;
    uint32_t byte_rate   = SAMPLE_RATE * 2u;
    uint16_t block_align = 2u;
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

static void render_wav(int seconds, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "open %s\n", path);
        exit(1);
    }
    uint32_t total = (uint32_t)SAMPLE_RATE * (uint32_t)seconds;
    write_wav_header(f, total);
    int16_t *buf = arena_alloc(BUFFER_FRAMES * sizeof(int16_t));
    uint32_t remaining = total;
    while (remaining > 0) {
        uint32_t n = remaining > BUFFER_FRAMES ? BUFFER_FRAMES : remaining;
        render_chunk(buf, n);
        fwrite(buf, 2, n, f);
        remaining -= n;
    }
    fclose(f);
}

static void play_alsa(void) {
    snd_pcm_t *pcm;
    int err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "alsa: %s\n", snd_strerror(err));
        exit(1);
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             1, SAMPLE_RATE, 1, LATENCY_US);
    if (err < 0) {
        fprintf(stderr, "alsa: %s\n", snd_strerror(err));
        exit(1);
    }
    int16_t *buf = arena_alloc(BUFFER_FRAMES * sizeof(int16_t));
    for (;;) {
        render_chunk(buf, BUFFER_FRAMES);
        err = snd_pcm_writei(pcm, buf, BUFFER_FRAMES);
        if (err < 0) {
            fprintf(stderr, "alsa: %s\n", snd_strerror(err));
            exit(1);
        }
    }
}

int main(int argc, char **argv) {
    voice_pool_init();
    gen_init();

    if (argc >= 2 && strcmp(argv[1], "--render") == 0) {
        if (argc != 4) {
            fprintf(stderr, "usage: %s --render <seconds> <output.wav>\n", argv[0]);
            exit(1);
        }
        render_wav(atoi(argv[2]), argv[3]);
        fprintf(stderr, "arena: %zu/%d bytes used\n", arena_used(), HEAP_BYTES);
    } else if (argc == 1) {
        play_alsa();
    } else {
        fprintf(stderr, "usage: %s [--render <seconds> <output.wav>]\n", argv[0]);
        exit(1);
    }
    return 0;
}
