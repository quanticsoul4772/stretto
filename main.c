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
       fixes the PRNG (regression determinism); the --midi* flags are
       captured into locals here so the dispatch block below can read
       them after gen_init/effects_init run. Order does not matter:
       arena_alloc is monotonic (no deallocation), and audio_midi_init
       is safe to call from any point in main(). (code-review Q5 fix
       2026-07-06: earlier comment claimed MIDI init happened BEFORE
       engine init, but the actual code runs gen_init + effects_init
       first and the MIDI dispatch block second.) */
    int positional_argc = 1;
    char *positional[8] = { argv[0] };
    int midi_explicit_no     = 0;  /* --no-midi */
    int midi_channel_filter  = 0;  /* --midi-channel N (0 = all) */
    int midi_list_devices    = 0;  /* --midi-list-devices */
    int midi_explicit_idx    = -1; /* --midi <N>; -1 = unspecified */
    int midi_default_open    = 0;  /* --midi-default alias for --midi 0 (FR-002) */
    int midi_wildcard        = 0;  /* --midi with no arg = subscribe all matching */
    int midi_seen            = 0;  /* any --midi-related flag was seen */

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
            midi_default_open = 1; midi_seen = 1;
        } else if (strcmp(argv[i], "--midi-channel") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--midi-channel: missing argument\n");
                exit(1);
            }
            char *end;
            unsigned long ch = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || ch < 1 || ch > 16) {
                fprintf(stderr,
                    "--midi-channel: expected integer 1..16, got \"%s\"\n",
                    argv[i]);
                exit(1);
            }
            midi_channel_filter = (int)ch;
            midi_seen = 1;
        } else if (strcmp(argv[i], "--midi") == 0) {
            /* Optional numeric index. --midi alone = wildcard (no
               specific device); --midi <N> = explicit index decode.
               A non-numeric following arg is left in positional[] for
               downstream parsing (matches the prior convention). */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                char *end;
                unsigned long idx = strtoul(argv[i + 1], &end, 10);
                if (*end == '\0') {
                    midi_explicit_idx = (int)idx;
                    i++;
                } else {
                    /* a non-numeric arg followed --midi (e.g.
                       "--midi --no-ui") -- treat that as wildcard. */
                    midi_wildcard = 1;
                }
            } else {
                /* No following arg (e.g. "--midi" at the end of argv)
                   OR the following arg starts with '-' (another
                   flag) -- either way, treat as wildcard. */
                midi_wildcard = 1;
            }
            midi_seen = 1;
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
    } else if (midi_explicit_idx >= 0 || midi_default_open || midi_wildcard) {
        /* Resolve device_index to audio_midi_open():
         *   --midi <N>:    explicit N (decode on Linux: client<<8|port,
         *                 pass direct on WinMM)
         *   --midi-default: explicit 0 per FR-002 (alias for --midi 0)
         *   --midi (no N): wildcard -1 (audio_midi_linux_init walks
         *                 enumerate filter and auto-subscribe; winmm
         *                 falls back to WAVE_MAPPER)
         * g_current_device_index on the backend tracks this for
         * idempotency + close-and-rebind on N change. */
        int idx;
        if (midi_explicit_idx >= 0)      idx = midi_explicit_idx;
        else if (midi_default_open)      idx = 0;
        else /* midi_wildcard */         idx = -1;
        audio_midi_init(midi_channel_filter);
        if (audio_midi_open(idx) != 0) {
            /* FR-002 + FR-034 semantics: --midi with a specific N
             * fails loudly if the device is gone; --midi (wildcard)
             * also fails if zero MIDI hw is enumerated. The synth
             * continues playing from internal generative state per
             * FR-005 + FR-034 (no auto-reconnect) so a missing
             * keyboard does NOT crash the run. */
            const char *kind = (idx < 0) ? "(wildcard)"
                            : midi_default_open ? "(default) " : "";
            fprintf(stderr,
                "MIDI: device index %d%s unavailable "
                "(see --midi-list-devices)\n", idx, kind);
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
