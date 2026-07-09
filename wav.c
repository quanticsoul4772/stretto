#include "wav.h"
#include "mixer.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

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

static void write_die(void) {
    fprintf(stderr, "render: write error: %s\n", strerror(errno));
    exit(1);
}

void render_wav(int seconds, const char *path) {
    /* "-" streams the WAV to stdout (sox/ffmpeg filename convention).
       The RIFF header is written up front from the known duration -
       no seek-back - so pipes work with the identical byte stream a
       file gets. A file literally named "-" needs ./-. All
       diagnostics in render mode go to stderr; stdout carries only
       WAV bytes. */
    int to_stdout = strcmp(path, "-") == 0;
    FILE *f;
    if (to_stdout) {
#ifdef _WIN32
        /* Text-mode stdout would translate LF bytes to CRLF and
           corrupt the RIFF stream. */
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        f = stdout;
    } else {
        f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "render: cannot open %s: %s\n",
                    path, strerror(errno));
            exit(1);
        }
    }
    uint32_t total = (uint32_t)SAMPLE_RATE * (uint32_t)seconds;
    write_wav_header(f, total);
    int16_t *buf = arena_alloc(BUFFER_FRAMES * 2 * sizeof(int16_t));
    uint32_t remaining = total;
    while (remaining > 0) {
        uint32_t n = remaining > BUFFER_FRAMES ? BUFFER_FRAMES : remaining;
        render_chunk(buf, n);
        /* Short write = disk full, or EPIPE with SIGPIPE ignored by a
           parent (default SIGPIPE disposition kills us before this
           check fires - standard pipeline behavior, kept). A header-
           write failure also lands here: it sets the stream error
           flag, so the first chunk fwrite returns short. */
        if (fwrite(buf, 2, n * 2, f) != (size_t)n * 2) write_die();
        remaining -= n;
    }
    /* stdio is fully buffered; the tail of the render lives in the
       stdio buffer until this flush, so an unchecked close would
       exit 0 with a truncated file on late ENOSPC. Never fclose
       stdout. */
    if (to_stdout) {
        if (fflush(stdout) != 0) write_die();
    } else {
        if (fclose(f) != 0) write_die();
    }
}
