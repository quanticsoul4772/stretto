#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <termios.h>

#include "arena.h"
#include "voice.h"
#include "gen.h"

#define SAMPLE_RATE    44100
#define BUFFER_FRAMES  1024
#define LATENCY_US     100000

#define TIOCGWINSZ     0x5413

static struct termios saved_termios;
static int termios_saved = 0;

#define SNDRV_PCM_IOCTL_HW_PARAMS _IOWR('A', 0x11, char)
#define SNDRV_PCM_IOCTL_SW_PARAMS _IOWR('A', 0x13, char)
#define SNDRV_PCM_IOCTL_PREPARE   _IO('A', 0x40)
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES _IOW('A', 0x50, char)

#define SNDRV_PCM_FORMAT_S16_LE 2
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED 3

typedef struct {
    unsigned int flags;
    unsigned int masks[3];
    unsigned int mintervals[5][2];
    unsigned int intervals[4][2];
    unsigned int rmask;
    unsigned int cmask;
    unsigned int info;
    unsigned int msbits;
    unsigned int rate_num;
    unsigned int rate_den;
    unsigned long long fifo_size;
    unsigned char reserved[64];
} snd_pcm_hw_params_t;

typedef struct {
    int tstamp_mode;
    unsigned int period_step;
    unsigned int sleep_min;
    unsigned int avail_min;
    unsigned int xfer_align;
    unsigned long long start_threshold;
    unsigned long long stop_threshold;
    unsigned long long silence_threshold;
    unsigned long long silence_size;
    unsigned long long boundary;
    unsigned int proto;
    unsigned int tstamp_type;
    unsigned char reserved[56];
} snd_pcm_sw_params_t;

typedef struct {
    int16_t *buf;
    unsigned long frames;
    int result;
} snd_xferi_t;

static void render_chunk(int16_t *out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        gen_step();
        out[i] = voice_pool_mix();
    }
}

static void restore_terminal(void) {
    if (termios_saved) {
        tcsetattr(0, TCSANOW, &saved_termios);
        write(1, "\x1b[?25h\n", 8);
    }
}

static void draw_oscilloscope(int16_t *buf, uint32_t frames) {
    unsigned short ws[4];
    ws[0] = 24; ws[1] = 80;
    ioctl(1, TIOCGWINSZ, ws);
    if (ws[1] == 0) ws[1] = 80;
    if (ws[0] == 0) ws[0] = 24;

    uint32_t w = ws[1] > 120 ? 120 : ws[1];
    uint32_t h = ws[0] > 1 ? ws[0] - 1 : 23;
    if (w > frames) w = frames;

    write(1, "\x1b[H\x1b[?25l", 10);

    char line[120];
    for (uint32_t r = 0; r < h; r++) {
        int32_t t = 32767 - (int32_t)((r * 65536u) / h);
        for (uint32_t c = 0; c < w; c++) {
            int16_t s = buf[(c * frames) / w];
            int32_t a = s < 0 ? -s : s;
            line[c] = (a > t) ? (a > 30000 ? '@' : a > 25000 ? '#' : a > 20000 ? '*' : a > 15000 ? '+' : a > 10000 ? '-' : '.') : ' ';
        }
        write(1, line, w);
        if (r < h - 1) write(1, "\r\n", 2);
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
    fcntl(0, F_SETFL, flags | O_NONBLOCK);

    int fd = open("/dev/snd/pcmC0D0p", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "open /dev/snd/pcmC0D0p: %s\n", strerror(errno));
        exit(1);
    }

    snd_pcm_hw_params_t hw_params;
    memset(&hw_params, 0, sizeof(hw_params));
    hw_params.flags = 0;
    hw_params.masks[0] = (1u << SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_params.masks[1] = (1u << SNDRV_PCM_FORMAT_S16_LE);
    hw_params.masks[2] = 0;
    hw_params.intervals[0][0] = 1;
    hw_params.intervals[0][1] = 1;
    hw_params.intervals[1][0] = SAMPLE_RATE;
    hw_params.intervals[1][1] = SAMPLE_RATE;
    hw_params.intervals[2][0] = BUFFER_FRAMES;
    hw_params.intervals[2][1] = BUFFER_FRAMES;
    hw_params.intervals[3][0] = BUFFER_FRAMES * 4;
    hw_params.intervals[3][1] = BUFFER_FRAMES * 4;

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw_params) < 0) {
        fprintf(stderr, "ioctl HW_PARAMS: %s\n", strerror(errno));
        exit(1);
    }

    snd_pcm_sw_params_t sw_params;
    memset(&sw_params, 0, sizeof(sw_params));
    sw_params.start_threshold = BUFFER_FRAMES;
    sw_params.avail_min = BUFFER_FRAMES;
    sw_params.boundary = BUFFER_FRAMES * 16;

    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw_params) < 0) {
        fprintf(stderr, "ioctl SW_PARAMS: %s\n", strerror(errno));
        exit(1);
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        fprintf(stderr, "ioctl PREPARE: %s\n", strerror(errno));
        exit(1);
    }

    int16_t *buf = arena_alloc(BUFFER_FRAMES * sizeof(int16_t));
    snd_xferi_t xferi;
    xferi.buf = buf;
    xferi.frames = BUFFER_FRAMES;

    for (;;) {
        render_chunk(buf, BUFFER_FRAMES);
        if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xferi) < 0) {
            fprintf(stderr, "ioctl WRITEI: %s\n", strerror(errno));
            exit(1);
        }

        draw_oscilloscope(buf, BUFFER_FRAMES);

        char ch;
        while (read(0, &ch, 1) > 0) {
            if (ch == ' ') gen_force_mutate();
            else if (ch == '+') gen_set_tempo(-10);
            else if (ch == '-') gen_set_tempo(+10);
            else if (ch == 'q') {
                close(fd);
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
