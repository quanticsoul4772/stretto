#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arena.h"
#include "voice.h"
#include "gen.h"
#include "effects.h"
#include "ui.h"
#include "audio.h"
#include "wav.h"

/* stretto entry point. Parses argv, initializes the synth, then
   dispatches to either render-to-WAV mode or live-audio playback.

   Usage:
     stretto [--no-ui] [--seed N]           live mode
     stretto --render <seconds> <out.wav> [--seed N]
*/
int main(int argc, char **argv) {
    voice_pool_init();

    /* Pre-scan argv for --seed N. If found, fix the PRNG to that
       value (used by the regression test and to reproduce a
       specific run). If absent, gen_init seeds from the system
       clock so every launch sounds different. */
    int positional_argc = 1;
    char *positional[8] = { argv[0] };
    for (int i = 1; i < argc && positional_argc < 8; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            char *end;
            unsigned long s = strtoul(argv[++i], &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "seed: must be unsigned integer, got \"%s\"\n", argv[i]);
                exit(1);
            }
            gen_seed((uint32_t)s);
        } else {
            positional[positional_argc++] = argv[i];
        }
    }
    argc = positional_argc;
    argv = positional;

    gen_init();
    effects_init();

    if (argc >= 2 && strcmp(argv[1], "--render") == 0) {
        if (argc != 4) {
            fprintf(stderr,
                "usage: %s --render <seconds> <output.wav> [--seed N]\n",
                argv[0]);
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
    } else if (argc == 1 || (argc == 2 && strcmp(argv[1], "--no-ui") == 0)) {
        if (argc == 2) ui_set_no_ui(1);
        audio_play();
    } else {
        fprintf(stderr,
            "usage: %s [--render <seconds> <output.wav>] [--no-ui] [--seed N]\n",
            argv[0]);
        exit(1);
    }
    return 0;
}
