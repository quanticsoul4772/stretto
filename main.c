#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <alsa/asoundlib.h>

#include "sin_table.h"

#define SAMPLE_RATE 44100
#define FREQ_HZ 440.0
#define BUFFER_FRAMES 1024
#define LATENCY_US 100000

static const uint32_t PHASE_INC_A440 = (uint32_t)((FREQ_HZ * 4294967296.0) / SAMPLE_RATE);

static void write_wav_header(FILE *f, uint32_t num_samples) {
    uint32_t data_size = num_samples * 2;
    uint32_t file_size = data_size + 36;

    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);

    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;
    uint16_t num_channels = 1;
    uint32_t sample_rate = SAMPLE_RATE;
    uint32_t byte_rate = SAMPLE_RATE * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;

    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);

    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

static void render_wav(int seconds, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "failed to open %s\n", path);
        exit(1);
    }

    uint32_t num_samples = SAMPLE_RATE * seconds;
    write_wav_header(f, num_samples);

    uint32_t phase = 0;
    int16_t buffer[BUFFER_FRAMES];

    for (uint32_t i = 0; i < num_samples; i++) {
        buffer[i % BUFFER_FRAMES] = sin_table[phase >> 22];
        phase += PHASE_INC_A440;

        if ((i + 1) % BUFFER_FRAMES == 0 || i == num_samples - 1) {
            size_t frames = (i % BUFFER_FRAMES) + 1;
            fwrite(buffer, 2, frames, f);
        }
    }

    fclose(f);
}

static void play_alsa(void) {
    snd_pcm_t *pcm;
    int err;

    err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
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

    int16_t buffer[BUFFER_FRAMES];
    uint32_t phase = 0;

    for (;;) {
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            buffer[i] = sin_table[phase >> 22];
            phase += PHASE_INC_A440;
        }

        err = snd_pcm_writei(pcm, buffer, BUFFER_FRAMES);
        if (err < 0) {
            fprintf(stderr, "alsa: %s\n", snd_strerror(err));
            exit(1);
        }
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--render") == 0) {
        if (argc != 4) {
            fprintf(stderr, "usage: %s --render <seconds> <output.wav>\n", argv[0]);
            exit(1);
        }
        int seconds = atoi(argv[2]);
        render_wav(seconds, argv[3]);
    } else if (argc == 1) {
        play_alsa();
    } else {
        fprintf(stderr, "usage: %s [--render <seconds> <output.wav>]\n", argv[0]);
        exit(1);
    }

    return 0;
}
