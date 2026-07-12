/* Unit tests for main.c's argv parse, error paths, and dispatch (080).
 *
 * main.c is compiled into main_testable.o with -Dmain=stretto_main
 * (Makefile rule; release OBJS untouched) and linked ONLY into this
 * binary. audio_play() is STUBBED below with a sentinel exit: that
 * makes the live dispatch testable AND guarantees a forked child can
 * never enter PulseAudio (or hang when a MIDI open unexpectedly
 * succeeds).
 *
 * Every case forks: main.c's error paths call exit(1), and children
 * must exit() (never _exit) so their gcov counters flush - the same
 * mechanism that makes test_wav's fork tests count for wav.c. The
 * parent NEVER calls stretto_main or test_init_synth: children need
 * to run the real init sequence on a virgin arena.
 *
 * Expected strings are quoted from main.c / USAGE - this harness
 * mirrors tests/test_cli.sh's end-to-end contract, never invents new
 * expectations. */
#include "test.h"
#include "../../audio_midi.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>

int stretto_main(int argc, char **argv);

/* Sentinel stub: covers main.c's live dispatch lines and keeps
   PulseAudio out of the link and out of every child. */
#define LIVE_SENTINEL 42
void audio_play(void) { exit(LIVE_SENTINEL); }

/* Fork stretto_main with argv (NULL-terminated, argv[0] included);
   capture stdout/stderr into out/err (cap bytes each, NUL-terminated)
   and return the child's exit code (-1 on abnormal death). */
static int run_main(char **argv, char *out, char *err, size_t cap) {
    char otmp[] = "/tmp/stretto_tm_o_XXXXXX";
    char etmp[] = "/tmp/stretto_tm_e_XXXXXX";
    int ofd = mkstemp(otmp), efd = mkstemp(etmp);
    int argc = 0;
    while (argv[argc]) argc++;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        /* Fork failure must FAIL the caller, not vacuously pass:
           a bare waitpid(-1) would leave status 0 = "exit 0" (082). */
        close(ofd);
        close(efd);
        unlink(otmp);
        unlink(etmp);
        out[0] = err[0] = '\0';
        return -1;
    }
    if (pid == 0) {
        dup2(ofd, 1);
        dup2(efd, 2);
        exit(stretto_main(argc, argv));
    }
    /* Deadline reap (082): children run real ALSA opens and 1 s
       renders - normally fast, but "normally fast" is what the
       cold-runner SIGKILL incident in test_ui was made of. A wedge
       must fail the test, not hang the suite. */
    int status = 0;
    int reaped = 0;
    for (int i = 0; i < 3000 && !reaped; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) reaped = 1;
        else {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    if (!reaped) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    for (int i = 0; i < 2; i++) {
        int fd = i ? efd : ofd;
        char *dst = i ? err : out;
        ssize_t n = pread(fd, dst, cap - 1, 0);
        dst[n > 0 ? n : 0] = '\0';
        close(fd);
    }
    unlink(otmp);
    unlink(etmp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static char g_out[8192], g_err[8192];

#define RUN(...) run_main((char *[]){ "stretto", __VA_ARGS__, NULL }, \
                          g_out, g_err, sizeof g_out)

/* MIDI input devices visible to this host (WSL/CI: normally zero;
   nonzero if snd-seq-dummy is loaded). Branches the --midi live
   assertions the way windows_smoke.py section 4 does. */
static int midi_device_count(void) {
    midi_input_device_t devs[MIDI_LIST_DEVICES_CAP];
    int32_t cnt = 0;
    audio_midi_list_devices(devs, &cnt);
    return (int)cnt;
}

/* ---- help / version (GNU 4.8: stdout, exit 0, ignore other args) */

TEST(main_help_exits_0_stdout) {
    ASSERT_EQ(RUN("--help"), 0);
    ASSERT_TRUE(strncmp(g_out, "usage: stretto", 14) == 0);
    ASSERT_TRUE(strstr(g_out, "print this help and exit") != NULL);
    ASSERT_EQ((int)strlen(g_err), 0);
}

TEST(main_h_short_flag) {
    ASSERT_EQ(RUN("-h"), 0);
    ASSERT_TRUE(strncmp(g_out, "usage:", 6) == 0);
}

TEST(main_help_wins_over_malformed_args) {
    ASSERT_EQ(RUN("--seed", "abc", "--help"), 0);
    ASSERT_TRUE(strncmp(g_out, "usage:", 6) == 0);
}

TEST(main_version_exits_0) {
    ASSERT_EQ(RUN("--version"), 0);
    ASSERT_TRUE(strncmp(g_out, "stretto ", 8) == 0);
    ASSERT_TRUE(strstr(g_out, "NO WARRANTY") != NULL);
}

/* ---- render dispatch ---- */

TEST(main_render_happy_path) {
    char wav[] = "/tmp/stretto_tm_r_XXXXXX";
    int fd = mkstemp(wav);
    close(fd);
    ASSERT_EQ(RUN("--render", "1", wav, "--seed", "0"), 0);
    ASSERT_TRUE(strstr(g_err, "resume with: --seed 0") != NULL);
    ASSERT_TRUE(strstr(g_err, "arena: ") != NULL);
    struct stat st;
    ASSERT_EQ(stat(wav, &st), 0);
    /* 44-byte header + 1 s * 48000 frames * 4 B */
    ASSERT_EQ((long)st.st_size, 44L + 48000L * 4L);
    unlink(wav);
}

TEST(main_render_applies_preset_flags) {
    char wav[] = "/tmp/stretto_tm_p_XXXXXX";
    int fd = mkstemp(wav);
    close(fd);
    /* Named scale + named filter mode + numeric gate: both
       resolve_named_value paths plus the apply loop. */
    ASSERT_EQ(RUN("--scale", "lydian", "--filter-mode", "notch",
                  "--gate", "100", "--render", "1", wav, "--seed", "0"), 0);
    unlink(wav);
}

TEST(main_render_arg_errors) {
    ASSERT_EQ(RUN("--render"), 1);
    ASSERT_TRUE(strstr(g_err, "render: expected --render <seconds> <out.wav|->") != NULL);
    ASSERT_TRUE(strstr(g_err, "usage:") != NULL);
    ASSERT_EQ(RUN("--render", "abc", "/tmp/x.wav"), 1);
    ASSERT_TRUE(strstr(g_err, "render: seconds must be an integer, got \"abc\"") != NULL);
    ASSERT_EQ(RUN("--render", "0", "/tmp/x.wav"), 1);
    ASSERT_TRUE(strstr(g_err, "render: seconds must be in 1..3600, got 0") != NULL);
}

/* ---- seed / usage / positional errors ---- */

TEST(main_seed_must_be_unsigned) {
    ASSERT_EQ(RUN("--seed", "abc"), 1);
    ASSERT_TRUE(strstr(g_err, "seed: must be unsigned integer, got \"abc\"") != NULL);
}

TEST(main_unknown_flag_is_usage_error) {
    ASSERT_EQ(RUN("--definitely-not-a-flag"), 1);
    ASSERT_TRUE(strstr(g_err, "usage:") != NULL);
    ASSERT_EQ((int)strlen(g_out), 0);
}

TEST(main_bare_seed_falls_to_usage) {
    /* "--seed" with no value is left positional (the pre-scan needs
       i+1 < argc), so dispatch rejects it with usage. */
    ASSERT_EQ(RUN("--seed"), 1);
    ASSERT_TRUE(strstr(g_err, "usage:") != NULL);
}

TEST(main_too_many_positionals) {
    ASSERT_EQ(RUN("x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8"), 1);
    ASSERT_TRUE(strstr(g_err, "too many arguments at \"x8\"") != NULL);
    ASSERT_TRUE(strstr(g_err, "usage:") != NULL);
}

/* ---- preset-capture flag errors (table-driven parse) ---- */

TEST(main_scale_named_error) {
    ASSERT_EQ(RUN("--scale", "nope"), 1);
    ASSERT_TRUE(strstr(g_err,
        "--scale: expected dorian|lydian|phrygian|locrian|harmminor|"
        "mixolydian or 0..5, got \"nope\"") != NULL);
}

TEST(main_filter_mode_named_error) {
    ASSERT_EQ(RUN("--filter-mode", "xy"), 1);
    ASSERT_TRUE(strstr(g_err,
        "--filter-mode: expected lp|hp|bp|notch or 0..3, got \"xy\"") != NULL);
}

TEST(main_numeric_range_error) {
    ASSERT_EQ(RUN("--gate", "999"), 1);
    ASSERT_TRUE(strstr(g_err,
        "--gate: expected integer 32..255, got \"999\"") != NULL);
}

TEST(main_missing_flag_argument) {
    ASSERT_EQ(RUN("--reverb"), 1);
    ASSERT_TRUE(strstr(g_err, "--reverb: missing argument") != NULL);
}

/* ---- MIDI flag family ---- */

TEST(main_midi_list_devices_exits_0) {
    ASSERT_EQ(RUN("--midi-list-devices"), 0);
    if (midi_device_count() == 0)
        ASSERT_TRUE(strstr(g_err, "no MIDI input devices found") != NULL);
    else
        ASSERT_TRUE(strlen(g_out) > 0);
}

TEST(main_midi_channel_guards) {
    ASSERT_EQ(RUN("--midi-channel"), 1);
    ASSERT_TRUE(strstr(g_err, "--midi-channel: missing argument") != NULL);
    ASSERT_EQ(RUN("--midi-channel", "17", "--midi"), 1);
    ASSERT_TRUE(strstr(g_err,
        "--midi-channel: expected integer 1..16, got \"17\"") != NULL);
    ASSERT_EQ(RUN("--midi-channel", "5"), 1);
    ASSERT_TRUE(strstr(g_err,
        "--midi-channel: requires --midi or --midi-default") != NULL);
}

TEST(main_midi_index_bound) {
    ASSERT_EQ(RUN("--midi", "4294967295"), 1);
    ASSERT_TRUE(strstr(g_err,
        "--midi: device index out of range: \"4294967295\" "
        "(see --midi-list-devices)") != NULL);
}

TEST(main_midi_explicit_index_unavailable) {
    /* 65535 = ALSA client 255 / port 255 - deterministically absent
       (client 255 is reserved), so this exercises the open-fail exit
       regardless of snd-seq-dummy. --midi-channel 3 rides along to
       cover the channel-filter init line on the same path. */
    ASSERT_EQ(RUN("--midi", "65535", "--midi-channel", "3", "--no-ui"), 1);
    ASSERT_TRUE(strstr(g_err,
        "MIDI: device index 65535 unavailable (see --midi-list-devices)") != NULL);
}

TEST(main_midi_wildcard_live) {
    int rc = RUN("--midi", "--no-ui");
    if (midi_device_count() == 0) {
        ASSERT_EQ(rc, 1);
        ASSERT_TRUE(strstr(g_err,
            "MIDI: no MIDI input devices found (see --midi-list-devices)") != NULL);
    } else {
        /* Open succeeded: the child reached the audio_play stub. */
        ASSERT_EQ(rc, LIVE_SENTINEL);
    }
}

TEST(main_midi_default_live) {
    int rc = RUN("--midi-default", "--no-ui");
    if (midi_device_count() == 0) {
        ASSERT_EQ(rc, 1);
        ASSERT_TRUE(strstr(g_err, "device index 0 unavailable") != NULL);
    } else {
        /* Devices exist but index 0 is client<<8|port encoded and
           rarely 0 - either outcome is contract-valid. */
        ASSERT_TRUE(rc == 1 || rc == LIVE_SENTINEL);
    }
}

TEST(main_no_midi_render) {
    char wav[] = "/tmp/stretto_tm_n_XXXXXX";
    int fd = mkstemp(wav);
    close(fd);
    ASSERT_EQ(RUN("--no-midi", "--render", "1", wav, "--seed", "0"), 0);
    unlink(wav);
}

TEST(main_midi_ignored_in_render) {
    char wav[] = "/tmp/stretto_tm_m_XXXXXX";
    int fd = mkstemp(wav);
    close(fd);
    ASSERT_EQ(RUN("--midi", "--render", "1", wav, "--seed", "0"), 0);
    ASSERT_TRUE(strstr(g_err, "MIDI: --midi is ignored in --render mode") != NULL);
    unlink(wav);
}

TEST(main_midi_nonnumeric_arg_is_wildcard_then_positional) {
    /* "--midi x": x is non-numeric, so --midi becomes wildcard and x
       stays positional. The wildcard OPEN runs before dispatch: on a
       deviceless host the child dies there ("no MIDI input devices
       found"); with devices the open succeeds and dispatch then
       rejects the stray positional with usage. Exit 1 either way. */
    ASSERT_EQ(RUN("--midi", "x"), 1);
    ASSERT_TRUE(strstr(g_err, "no MIDI input devices found") != NULL
             || strstr(g_err, "usage:") != NULL);
}

/* ---- live dispatch (audio_play stub) ---- */

TEST(main_live_dispatch_hits_stub) {
    ASSERT_EQ(RUN(NULL), LIVE_SENTINEL);
}

TEST(main_no_ui_live_dispatch_hits_stub) {
    ASSERT_EQ(RUN("--no-ui"), LIVE_SENTINEL);
}

int main(void) {
    return RUN_ALL();
}
