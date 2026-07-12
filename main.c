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
#include "version.h"

/* stretto entry point. Parses argv, initializes the synth, then
   dispatches to either render-to-WAV mode or live-audio playback.

   Usage:
     stretto [--no-ui] [--seed N] [--midi [N] | --midi-default | --no-midi]
             [--midi-channel <1..16>]        live mode
     stretto --render <seconds> <out.wav> [--seed N]
     stretto --midi-list-devices
     stretto --help | -h | --version
*/

/* Single source for the usage synopsis: printed to stderr on usage
   errors and to stdout by --help, so the two can never drift. The
   program name is the constant "stretto", not argv[0], per GNU
   Coding Standards 4.8.1 (the Linux binary is ./synth; the canonical
   name is deliberate). */
static const char USAGE[] =
    "usage: stretto [--render <seconds> <out.wav|->] [--no-ui] [--seed N]\n"
    "               [--midi [N] | --midi-default | --no-midi]\n"
    "               [--midi-channel <1..16>] [--midi-list-devices]\n"
    "               [--scale S] [--bar-ms N] [--gate N] [--mod-depth N]\n"
    "               [--cutoff N] [--resonance N] [--lfo-depth N]\n"
    "               [--filter-mode M] [--reverb N] [--delay N]\n"
    "               [--feedback N] [--comp-threshold N] [--swing N]\n"
    "               [-h | --help] [--version]\n";

static const char HELP_BODY[] =
    "\n"
    "stretto - a tiny generative music synthesizer (C99, no malloc,\n"
    "single 128 KB arena). Runs live with an ASCII oscilloscope, or\n"
    "renders to a 48 kHz stereo 16-bit WAV.\n"
    "\n"
    "  --render <s> <out.wav>  render s seconds (1..3600) to a WAV file;\n"
    "                          '-' streams the WAV to stdout for piping\n"
    "  --seed N                fix all generative state; with the same\n"
    "                          flags, output is byte-identical per seed\n"
    "  --no-ui                 headless live mode (no TTY required)\n"
    "  --midi [N]              open MIDI input device N, or subscribe to\n"
    "                          every input port if N is omitted (Windows:\n"
    "                          first device); exits 1 if none available\n"
    "  --midi-default          alias for --midi 0\n"
    "  --no-midi               explicit MIDI opt-out (same as no flag)\n"
    "  --midi-channel <1..16>  accept only this MIDI channel\n"
    "  --midi-list-devices     list MIDI input devices and exit\n"
    "  -h, --help              print this help and exit\n"
    "  --version               print version information and exit\n"
    "\n"
    "Initial-state flags (preset capture; ranges are hard errors):\n"
    "  --scale <name|0-5>      dorian|lydian|phrygian|locrian|harmminor|\n"
    "                          mixolydian\n"
    "  --bar-ms <760-7600>     bar length in ms (default 2000)\n"
    "  --gate <32-255>         melody gate probability\n"
    "  --mod-depth <100-8000>  FM modulation depth\n"
    "  --cutoff <30-180>       filter cutoff base (untouched default is\n"
    "                          200, above this dial range - omit the flag\n"
    "                          to keep it)\n"
    "  --resonance <0-180>     filter resonance base\n"
    "  --lfo-depth <0-255>     filter LFO depth\n"
    "  --filter-mode <m|0-3>   lp|hp|bp|notch\n"
    "  --reverb <0-256>        reverb wet mix\n"
    "  --delay <0-256>         delay wet mix\n"
    "  --feedback <0-200>      delay feedback\n"
    "  --comp-threshold <8000-30000>  compressor threshold\n"
    "  --swing <0-100>         shuffle (0 straight, 100 ~triplet)\n"
    "\n"
    "On quit (q, Ctrl-C, SIGTERM), live mode prints a pasteable\n"
    "'resume with: --seed N ...' line with the parameters you set -\n"
    "recall reproduces the run from bar 0 with those values.\n"
    "\n"
    "In live mode, press ? for the key map, q to quit.\n"
    "NO_COLOR (non-empty) disables status/oscilloscope colors.\n"
    "Report bugs: https://github.com/quanticsoul4772/stretto/issues\n";

static const char VERSION_TEXT[] =
    "stretto " STRETTO_VERSION "\n"
    "Copyright (C) 2026 rbsmith4\n"
    "License MIT: <https://opensource.org/license/mit>\n"
    "This is free software: you are free to change and redistribute it.\n"
    "There is NO WARRANTY, to the extent permitted by law.\n";

/* PARAM_FLAGS (the preset-capture flag table) is defined in ui.c and
   shared via ui.h: the help overlay reads names/ranges from it, and
   unit-test binaries link ui.o without main.o. */

/* Resolve a named flag value (scale / filter-mode); returns 1 on
   match with *out set. Numeric fallback is handled by the caller. */
static int resolve_named_value(const ParamFlag *pf, const char *s, int *out) {
    if (pf->named == 1) {
        for (int i = 0; i <= 5; i++)
            if (strcmp(s, ui_scale_name(i)) == 0) { *out = i; return 1; }
    } else if (pf->named == 2) {
        for (int i = 0; i <= 3; i++)
            if (strcmp(s, ui_filter_mode_name(i)) == 0) { *out = i; return 1; }
    }
    return 0;
}

int main(int argc, char **argv) {
    /* GNU Coding Standards 4.8: --help / --version print to stdout,
       exit successfully, and ignore every other option and argument -
       including malformed ones (`--seed abc --help` prints help; that
       is the standard's behavior, not a bug). Checked before any
       engine init so neither flag can reach the seed-parse or
       MIDI-open error paths. First occurrence of either flag wins. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fputs(USAGE, stdout);
            fputs(HELP_BODY, stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            fputs(VERSION_TEXT, stdout);
            return 0;
        }
    }

    /* Status-panel version stamp. Unconditional (dead in --render /
       headless, harmless); ui.c never includes version.h so only this
       translation unit rebuilds on a version bump. */
    ui_set_version(STRETTO_VERSION);

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
    int param_values[UI_PARAM_COUNT];
    unsigned param_given = 0;      /* bit k = PARAM_FLAGS[k] present */

    /* No positional-cap guard in the loop condition: stopping the
       scan early silently dropped everything after the 8th
       positional, INCLUDING recognized flags like a trailing --seed.
       Overflow is an explicit usage error in the else branch
       instead. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            char *end;
            unsigned long s = strtoul(argv[++i], &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "seed: must be unsigned integer, got \"%s\"\n", argv[i]);
                exit(1);
            }
            gen_seed((uint32_t)s);
        } else if (strcmp(argv[i], "--no-midi") == 0) {
            midi_explicit_no = 1;
        } else if (strcmp(argv[i], "--midi-list-devices") == 0) {
            midi_list_devices = 1;
        } else if (strcmp(argv[i], "--midi-default") == 0) {
            midi_default_open = 1;
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
        } else if (strcmp(argv[i], "--midi") == 0) {
            /* Optional numeric index. --midi alone = wildcard (no
               specific device); --midi <N> = explicit index decode.
               A non-numeric following arg is left in positional[] for
               downstream parsing (matches the prior convention). */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                char *end;
                unsigned long idx = strtoul(argv[i + 1], &end, 10);
                if (*end == '\0') {
                    /* Bound before the int cast: strtoul saturates
                       overflow at ULONG_MAX, and an unchecked (int)
                       cast wraps e.g. 4294967295 into the -1 wildcard
                       sentinel silently. 65535 = max (client<<8)|port
                       encoding on Linux; Windows ids are far smaller. */
                    if (idx > 65535) {
                        fprintf(stderr,
                            "--midi: device index out of range: \"%s\" "
                            "(see --midi-list-devices)\n", argv[i + 1]);
                        exit(1);
                    }
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
        } else {
            /* Preset-capture flags: table lookup, value parse (name
               or integer), range check. Unmatched tokens stay
               positional. */
            const ParamFlag *pf = NULL;
            int k;
            for (k = 0; k < UI_PARAM_COUNT; k++) {
                if (strcmp(argv[i], PARAM_FLAGS[k].name) == 0) {
                    pf = &PARAM_FLAGS[k];
                    break;
                }
            }
            if (pf) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "%s: missing argument\n", pf->name);
                    exit(1);
                }
                const char *val = argv[++i];
                int v;
                if (!resolve_named_value(pf, val, &v)) {
                    char *end;
                    long lv = strtol(val, &end, 10);
                    if (*val == '\0' || *end != '\0'
                        || lv < pf->min || lv > pf->max) {
                        if (pf->named == 1)
                            fprintf(stderr, "--scale: expected dorian|lydian|"
                                "phrygian|locrian|harmminor|mixolydian or 0..5, "
                                "got \"%s\"\n", val);
                        else if (pf->named == 2)
                            fprintf(stderr, "--filter-mode: expected "
                                "lp|hp|bp|notch or 0..3, got \"%s\"\n", val);
                        else
                            fprintf(stderr, "%s: expected integer %d..%d, "
                                "got \"%s\"\n", pf->name, pf->min, pf->max, val);
                        exit(1);
                    }
                    v = (int)lv;
                }
                param_values[k] = v;
                param_given |= 1u << k;
            } else {
                if (positional_argc >= 8) {
                    fprintf(stderr, "too many arguments at \"%s\"\n", argv[i]);
                    fputs(USAGE, stderr);
                    exit(1);
                }
                positional[positional_argc++] = argv[i];
            }
        }
    }
    argc = positional_argc;
    argv = positional;

    gen_init();
    effects_init();

    /* Apply the preset-capture flags AFTER gen_init/effects_init
       (those reset tempo/scale/gate/threshold to defaults). Setters
       consume no PRNG draws, so output stays a pure function of
       (seed, flags). gen_init's early density_update ran with the
       default gate, but density recomputes at substep 0 of the first
       gen_step - before any trigger - so there is no byte-level
       consequence; do not "fix" the ordering. */
    for (int k = 0; k < UI_PARAM_COUNT; k++) {
        if (param_given & (1u << k)) {
            PARAM_FLAGS[k].set(param_values[k]);
            ui_mark_param_set(k);
        }
    }

    /* MIDI dispatch (per FR-001..FR-006 + specs/003-midi-input/
       contracts/cli.md). --midi-list-devices short-circuits to
       list-and-exit; --midi / --midi-default open the backend and
       fail loudly per FR-002; --render never opens MIDI so a seeded
       render stays a pure function of (--seed, argv) per
       Constitution III. */
    int is_render = (argc >= 2 && strcmp(argv[1], "--render") == 0);
    int midi_open_requested =
        (midi_explicit_idx >= 0 || midi_default_open || midi_wildcard);

    if (midi_channel_filter != 0 && !midi_open_requested) {
        fprintf(stderr, "--midi-channel: requires --midi or --midi-default\n");
        exit(1);
    }
    if (midi_list_devices) {
        midi_input_device_t devs[MIDI_LIST_DEVICES_CAP];
        int32_t cnt = 0;
        audio_midi_list_devices(devs, &cnt);
        for (int32_t i = 0; i < cnt; i++) {
            printf("%d %s\n", devs[i].index, devs[i].name);
        }
        if (cnt == 0) {
            fprintf(stderr, "no MIDI input devices found\n");
        }
        return 0;
    }
    if (midi_explicit_no) {
        audio_midi_init(-1);  /* opt-out: queue stays disabled */
    } else if (midi_open_requested && is_render) {
        /* render_chunk drains the MIDI queue, so an open device could
           inject events into a seeded render. Never open in render
           mode; g_enabled stays 0 and the render is byte-identical
           to a no-MIDI run (Constitution III). */
        fprintf(stderr, "MIDI: --midi is ignored in --render mode\n");
    } else if (midi_open_requested) {
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
            /* FR-002: an explicit MIDI request that cannot be
             * satisfied is a startup error (exit non-zero). The
             * continue-without-MIDI path is FR-034's MID-SESSION
             * disconnect, not a failed startup open. audio_midi_open
             * already reset g_enabled = 0 on this path. */
            if (idx < 0) {
                fprintf(stderr, "MIDI: no MIDI input devices found "
                                "(see --midi-list-devices)\n");
            } else {
                fprintf(stderr, "MIDI: device index %d unavailable "
                                "(see --midi-list-devices)\n", idx);
            }
            exit(1);
        }
    }
    /* If no MIDI flag was seen at all (the default), audio_midi_init
       is never called - g_enabled stays 0 (BSS), drain() is a no-op,
       and --no-midi byte-identity with golden/regression_16s.sha256
       is preserved (FR-050 / FR-053 / Constitution III v1.0.1). */

    if (argc >= 2 && strcmp(argv[1], "--render") == 0) {
        if (argc != 4) {
            fprintf(stderr, "render: expected --render <seconds> <out.wav|->\n");
            fputs(USAGE, stderr);
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
        /* Capture line for renders (any exit path, including Ctrl-C,
           which has no handler on this path): seed only - the other
           flags are already on the user's command line. stderr, since
           stdout may be carrying WAV bytes for --render N -. */
        fprintf(stderr, "resume with: --seed %u\n",
                (unsigned)gen_get_seed_input());
        render_wav((int)seconds, argv[3]);
        fprintf(stderr, "arena: %zu/%d bytes used\n", arena_used(), HEAP_BYTES);
    } else if (argc == 1 || (argc == 2 && strcmp(argv[1], "--no-ui") == 0)) {
        if (argc == 2) ui_set_no_ui(1);
        audio_play();
    } else {
        fputs(USAGE, stderr);
        exit(1);
    }
    return 0;
}
