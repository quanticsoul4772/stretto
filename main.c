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
#include "audio_midi.h"

/* stretto entry point. Parses argv, initializes the synth, then
   dispatches to either render-to-WAV mode or live-audio playback.

   Usage:
     stretto [--no-ui] [--seed N]           live mode
     stretto --render <seconds> <out.wav> [--seed N]
*/
int main(int argc, char **argv) {
    voice_pool_init();

    /* Pre-scan argv for --seed N + the five --midi* flags. --seed
       fixes the PRNG (regression determinism); the --midi* flags
       initialize the audio_midi module BEFORE gen_init/effects_init
       so the ring-buffer arena allocation happens once at startup
       (if --midi is set) rather than lazily on the first event. */
    int positional_argc = 1;
    char *positional[8] = { argv[0] };
    int midi_explicit_no    = 0;  /* --no-midi */
    int midi_channel_filter = 0;  /* --midi-channel N (0 = all) */
    int midi_list_devices   = 0;  /* --midi-list-devices */
    int midi_explicit_idx   = -1; /* --midi <N>; -1 = unspecified */
    int midi_default        = 0;  /* --midi-default alias */
    int midi_seen           = 0;  /* any --midi-related flag was seen */

    for (int i = 1; i < argc && positional_argc < 8; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            char *end;
            unsigned long s = strtoul(argv[++i], &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "seed: must be unsigned integer, got \"%s\"\n", argv[i]);
                exit(1);
            }
            gen_seed((uint32_t)s);
        } else if (strcmp(argv[i], "--no-midi") == 0) {
            midi_explicit_no = 1; midi_seen = 1;
        } else if (strcmp(argv[i], "--midi-list-devices") == 0) {
            midi_list_devices = 1; midi_seen = 1;
        } else if (strcmp(argv[i], "--midi-default") == 0) {
            midi_default = 1; midi_seen = 1;
        } else if (strcmp(argv[i], "--midi-channel") == 0 && i + 1 < argc) {
            char *end;
            unsigned long ch = strtoul(argv[++i], &end, 10);
            if (*end == '\0' && ch >= 1 && ch <= 16) {
                midi_channel_filter = (int)ch; midi_seen = 1;
            }
        } else if (strcmp(argv[i], "--midi") == 0) {
            /* Optional numeric index. --midi alone = default device;
               --midi <N> = explicit index. Non-numeric following arg
               is left in positional[] for downstream parsing. */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                char *end;
                unsigned long idx = strtoul(argv[i + 1], &end, 10);
                if (*end == '\0') {
                    midi_explicit_idx = (int)idx;
                    i++;
                }
            }
            midi_default = 1; midi_seen = 1;
        } else {
            positional[positional_argc++] = argv[i];
        }
    }
    argc = positional_argc;
    argv = positional;

    gen_init();
    effects_init();

    /* MIDI dispatch (per FR-001..FR-006 + spec.md CLI surface).
       Phase 1+2 wires the parser + init; real backend opens happen
       in US3 (T036..T038) when audio_midi_linux_init /
       audio_midi_winmm_init land their pthread_create + midiInOpen
       implementations. */
    if (midi_list_devices) {
        midi_input_device_t devs[32];
        int32_t cnt = 0;
        audio_midi_list_devices(devs, &cnt);
        for (int32_t i = 0; i < cnt; i++) {
            printf("%d %s\n", devs[i].index, devs[i].name);
        }
        return 0;
    }
    if (midi_explicit_no) {
        audio_midi_init(-1);  /* opt-out: queue stays disabled */
    } else if (midi_explicit_idx >= 0 || midi_default) {
        int idx = midi_explicit_idx >= 0 ? midi_explicit_idx : 0;
        audio_midi_init(midi_channel_filter);
        if (audio_midi_open(idx) != 0) {
            /* Phase 1+2: backend is a stub returning -1. US3 lands a
               real pthread_create + ALSA sequencer open and fails
               loudly if the device is unavailable. */
            fprintf(stderr,
                "MIDI: device index %d unavailable "
                "(backend lands in US3 T022/T023)\n", idx);
            /* Continue without MIDI — synth still plays from
               internal generative state per FR-005. */
        }
    } else if (midi_seen) {
        /* Saw --midi-channel only; init with that filter but don't open. */
        audio_midi_init(midi_channel_filter);
    }
    /* If no MIDI flag was seen at all (the default), audio_midi_init
       is never called - g_enabled stays 0 (BSS), drain() is a no-op,
       and --no-midi byte-identity with golden/regression_16s.sha256
       is preserved (FR-050 / FR-053 / Constitution III v1.0.1). */

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
