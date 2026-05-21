#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <termios.h>
#include <alsa/asoundlib.h>

#include "arena.h"
#include "voice.h"
#include "gen.h"

#define SAMPLE_RATE    44100
#define BUFFER_FRAMES  1024
#define LATENCY_US     100000

#define TIOCGWINSZ     0x5413

static struct termios saved_termios;
static int termios_saved = 0;

static void render_chunk(int16_t *out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        gen_step();
        out[i] = voice_pool_mix();
    }
}

static void restore_terminal(void) {
    if (termios_saved) {
        tcsetattr(0, TCSANOW, &saved_termios);
        (void)!write(1, "\x1b[?25h\n", 8);
    }
}

static void draw_oscilloscope(int16_t *buf, uint32_t frames) {
    unsigned short ws[4];
    ws[0] = 24; ws[1] = 80;
    ioctl(1, TIOCGWINSZ, ws);
    if (ws[1] == 0) ws[1] = 80;
    if (ws[0] == 0) ws[0] = 24;

    uint32_t w = ws[1] > 120 ? 120 : ws[1];
    uint32_t h = ws[0] > 2 ? ws[0] - 2 : 22;
    if (w > frames) w = frames;

    (void)!write(1, "\x1b[H\x1b[?25l\x1b[2K", 14);

    uint32_t mask = voice_pool_active_mask();
    char s[40];
    int p = 0;
    s[p++] = 'M'; s[p++] = ':';
    unsigned v = voice_get_mod_depth();
    char t[6]; int n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n > 0) s[p++] = t[--n];
    s[p++] = ' '; s[p++] = 'V'; s[p++] = ':';
    for (int i = 0; i < N_VOICES; i++) s[p++] = (mask & (1u << i)) ? '*' : '.';
    (void)!write(1, s, p);
    (void)!write(1, "\r\n", 2);

    char line[120];
    for (uint32_t r = 0; r < h; r++) {
        int32_t t = 8000 - (int32_t)((r * 16000u) / h);
        for (uint32_t c = 0; c < w; c++) {
            int16_t s = buf[(c * frames) / w];
            int32_t a = s < 0 ? -s : s;
            line[c] = (a > t) ? (a > 7000 ? '@' : a > 5000 ? '#' : a > 3500 ? '*' : a > 2500 ? '+' : a > 1500 ? '-' : '.') : ' ';
        }
        (void)!write(1, "\x1b[2K", 4);
        (void)!write(1, line, w);
        if (r < h - 1) (void)!write(1, "\r\n", 2);
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
    if (tcgetattr(0, &saved_termios) < 0) {
        fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
        exit(1);
    }
    termios_saved = 1;
    atexit(restore_terminal);

    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) < 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        exit(1);
    }

    int flags = fcntl(0, F_GETFL, 0);
    if (flags < 0 || fcntl(0, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "fcntl: %s\n", strerror(errno));
        exit(1);
    }

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
        snd_pcm_sframes_t frames = snd_pcm_writei(pcm, buf, BUFFER_FRAMES);
        if (frames < 0) {
            /* Attempt recovery from xruns (EPIPE), suspends (ESTRPIPE),
               and interrupts. snd_pcm_recover handles those internally;
               for anything else it returns the original error. */
            int rc = snd_pcm_recover(pcm, (int)frames, 1);
            if (rc < 0) {
                fprintf(stderr, "alsa: %s\n", snd_strerror(rc));
                exit(1);
            }
            /* Recovered. Skip this buffer's oscilloscope update to keep
               the gen state advancing; the next iteration will catch up. */
            continue;
        }

        draw_oscilloscope(buf, BUFFER_FRAMES);

        char ch;
        while (read(0, &ch, 1) > 0) {
            if (ch == ' ') gen_force_mutate();
            else if (ch == '+') gen_set_tempo(-10);
            else if (ch == '-') gen_set_tempo(+10);
            else if (ch == '[') {
                uint16_t d = voice_get_mod_depth();
                voice_set_mod_depth(d > 200 ? d - 200 : 100);
            }
            else if (ch == ']') {
                voice_set_mod_depth(voice_get_mod_depth() + 200);
            }
            else if (ch == 'q') {
                snd_pcm_close(pcm);
                restore_terminal();
                exit(0);
            }
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
        char *end;
        long seconds = strtol(argv[2], &end, 10);
        if (*argv[2] == '\0' || *end != '\0') {
            fprintf(stderr, "render: seconds must be an integer, got \"%s\"\n", argv[2]);
            exit(1);
        }
        if (seconds < 1 || seconds > 3600) {
            fprintf(stderr, "render: seconds must be in 1..3600, got %ld\n", seconds);
            exit(1);
        }
        render_wav((int)seconds, argv[3]);
        fprintf(stderr, "arena: %zu/%d bytes used\n", arena_used(), HEAP_BYTES);
    } else if (argc == 1) {
        play_alsa();
    } else {
        fprintf(stderr, "usage: %s [--render <seconds> <output.wav>]\n", argv[0]);
        exit(1);
    }
    return 0;
}
