/* Unit tests for ui.c's draw path, write paths, headless degrade,
 * and POSIX signal handlers (081) - the last PTY-free-reachable
 * third of the file, graduating ui.c into the coverage-gated set.
 *
 * INVARIANTS:
 * - The PARENT never calls a ui write path, ui_draw_oscilloscope, or
 *   ui_set_no_ui: no_color_enabled's cache and no_ui_flag are
 *   per-process, and RUN_ALL prints to stdout between tests - every
 *   test that touches fd 1, the environment, or signal dispositions
 *   FORKS (children exit() so their gcov counters flush; the
 *   test_main/test_wav mechanism).
 * - Every fork is reaped through reap_or_kill: a wedged or stopped
 *   child would HANG the coverage loop (not fail it), and a child-
 *   side alarm() cannot protect a stopped process (SIGALRM stays
 *   pending during the stop). The synth-spawn-deadline rule,
 *   generalized.
 *
 * Semantics authority: tests/test_smoke_live.sh pins the signal
 * handling on a REAL PTY (raw-mode restore, fg re-raw). These forks
 * are gcov-measured line coverage over the same paths; the termios
 * calls no-op harmlessly on non-TTY fds. */
#include "test.h"
#include "../../ui.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>

/* Reap pid with a ~30 s deadline; SIGKILL + final blocking reap if
   it neither exits nor (when flags & WUNTRACED) stops in time.
   Returns the waitpid status. 30 s, not 5: the first CI run of this
   suite SIGKILLed a child that was still faulting in its pages on a
   cold runner - the deadline exists to prevent hangs, not to race
   the scheduler (WNOHANG polling costs nothing when the child is
   fast). */
static int reap_or_kill(pid_t pid, int flags) {
    int status = 0;
    for (int i = 0; i < 3000; i++) {
        pid_t r = waitpid(pid, &status, flags | WNOHANG);
        if (r == pid) return status;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return status;
}

/* Read a whole (small) file into buf, NUL-terminated. */
static void slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, cap - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = '\0';
}

/* True if buf contains an SGR sequence (ESC [ ... m). */
static int has_sgr(const char *buf) {
    for (const char *p = buf; (p = strchr(p, 0x1b)) != NULL; p++) {
        if (p[1] != '[') continue;
        const char *e = p + 2;
        while (*e && !(*e >= 0x40 && *e <= 0x7e)) e++;
        if (*e == 'm') return 1;
    }
    return 0;
}

/* Fill a stereo frame buffer with a full-range amplitude ramp
   (positives and negatives) so the scope grid hits all 7 heat
   levels and the abs branch. */
static void fill_ramp(int16_t *buf, int frames) {
    for (int i = 0; i < frames; i++) {
        int16_t v = (int16_t)(((i * 65536) / frames) - 32768 + (i % 2 ? 500 : -500));
        buf[2 * i] = v;
        buf[2 * i + 1] = (int16_t)-v;
    }
}

TEST(ui_draw_scope_rich_panel) {
    char out[] = "/tmp/stretto_ui_d_XXXXXX";
    int fd = mkstemp(out);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) return;   /* never reach reap/kill paths with pid -1 */
    if (pid == 0) {
        unsetenv("NO_COLOR");
        dup2(fd, 1);
        test_init_synth();
        static int16_t sig[256 * 2];
        fill_ramp(sig, 256);
        /* Non-TTY stdout: ioctl fails, 80x24 defaults hold ->
           th=24 >= 20 -> the rich panel path. >= 80 frames keeps
           w=80. */
        ui_draw_oscilloscope(sig, 256);
        exit(0);
    }
    int st = reap_or_kill(pid, 0);
    ASSERT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    static char buf[32768];
    slurp(out, buf, sizeof buf);
    close(fd);
    unlink(out);
    ASSERT_TRUE(strstr(buf, "stretto") != NULL);
    ASSERT_TRUE(strstr(buf, "scale") != NULL);
    /* has_sgr, not a bare ESC search: the functional escapes
       (\x1b[H, \x1b[2K) survive NO_COLOR stripping, so a bare
       "contains ESC" assert could not detect an always-stripping
       regression in no_color_enabled (082). */
    ASSERT_TRUE(has_sgr(buf));
}

TEST(ui_draw_scope_no_color_strips_sgr) {
    char out[] = "/tmp/stretto_ui_n_XXXXXX";
    int fd = mkstemp(out);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) return;   /* never reach reap/kill paths with pid -1 */
    if (pid == 0) {
        setenv("NO_COLOR", "1", 1);
        dup2(fd, 1);
        test_init_synth();
        static int16_t sig[256 * 2];
        fill_ramp(sig, 256);
        ui_draw_oscilloscope(sig, 256);
        exit(0);
    }
    int st = reap_or_kill(pid, 0);
    ASSERT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    static char buf[32768];
    slurp(out, buf, sizeof buf);
    close(fd);
    unlink(out);
    ASSERT_TRUE(strlen(buf) > 100);
    ASSERT_FALSE(has_sgr(buf));
    /* Functional escapes survive the strip. */
    ASSERT_TRUE(strstr(buf, "\x1b[H") != NULL);
    ASSERT_TRUE(strstr(buf, "\x1b[2K") != NULL);
}

TEST(ui_show_help_and_clear_write_paths) {
    char out[] = "/tmp/stretto_ui_h_XXXXXX";
    int fd = mkstemp(out);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) return;   /* never reach reap/kill paths with pid -1 */
    if (pid == 0) {
        dup2(fd, 1);        /* no_ui stays default 0 in the child */
        test_init_synth();
        ui_show_help();
        ui_clear_screen();
        exit(0);
    }
    int st = reap_or_kill(pid, 0);
    ASSERT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    static char buf[8192];
    slurp(out, buf, sizeof buf);
    close(fd);
    unlink(out);
    ASSERT_TRUE(strstr(buf, "stretto keys") != NULL);
    ASSERT_TRUE(strstr(buf, "\x1b[H\x1b[2J") != NULL);
}

TEST(ui_term_read_key_pipe_and_eof) {
    int p[2];
    ASSERT_EQ(pipe(p), 0);
    int saved0 = dup(0);
    (void)!write(p[1], "s", 1);
    dup2(p[0], 0);
    char ch = 0;
    ASSERT_EQ(ui_term_read_key(&ch), 1);
    ASSERT_EQ(ch, 's');
    close(p[1]);                      /* EOF: read returns 0 */
    ASSERT_EQ(ui_term_read_key(&ch), 0);
    dup2(saved0, 0);
    close(saved0);
    close(p[0]);
}

TEST(ui_term_raw_mode_headless_degrade) {
    char err[] = "/tmp/stretto_ui_e_XXXXXX";
    int efd = mkstemp(err);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) return;   /* never reach reap/kill paths with pid -1 */
    if (pid == 0) {
        int nul = open("/dev/null", 0);
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(efd, 2);
        ui_term_raw_mode();           /* non-TTY: degrade, no exit */
        exit(ui_get_no_ui() == 1 ? 0 : 1);
    }
    int st = reap_or_kill(pid, 0);
    ASSERT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    static char buf[4096];
    slurp(err, buf, sizeof buf);
    close(efd);
    unlink(err);
    ASSERT_TRUE(strstr(buf, "running headless") != NULL);
}

TEST(ui_term_restore_mode_noop_when_unsaved) {
    /* termios_saved == 0: sig_term_restore's guard is false, nothing
       is written. Safe in-process. */
    ui_term_restore_mode();
    ASSERT_TRUE(1);
}

TEST(ui_sigterm_reraise_semantics) {
    /* Semantics only (SA_RESETHAND + re-raise -> honest signal
       death). A child killed by a signal never flushes gcov
       counters, so this test deliberately carries NO line-coverage
       expectation - see the SIGWINCH carrier below. */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) return;   /* never reach reap/kill paths with pid -1 */
    if (pid == 0) {
        int nul = open("/dev/null", 1);
        dup2(nul, 2);
        ui_install_signal_handlers();
        raise(SIGTERM);
        exit(9);                      /* unreachable */
    }
    int st = reap_or_kill(pid, 0);
    ASSERT_TRUE(WIFSIGNALED(st));
    ASSERT_EQ(WTERMSIG(st), SIGTERM);
}

TEST(ui_fatal_handler_lines_via_sigwinch) {
    /* THE line-coverage carrier for sig_restore_and_reraise +
       ui_install_signal_handlers + the guarded sig_term_restore +
       the resume-line write branch: retrieve the installed handler
       and re-install it on SIGWINCH (default action = ignore) with
       SA_RESETHAND. The handler runs fully; its re-raise becomes a
       post-handler ignore; the child SURVIVES and exit(0) flushes
       its counters. */
    char err[] = "/tmp/stretto_ui_w_XXXXXX";
    int efd = mkstemp(err);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) return;   /* never reach reap/kill paths with pid -1 */
    if (pid == 0) {
        dup2(efd, 2);
        ui_install_signal_handlers();
        ui_set_resume_line("resume with: --seed 7");
        struct sigaction old;
        sigaction(SIGTERM, NULL, &old);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = old.sa_handler;
        sa.sa_flags = SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGWINCH, &sa, NULL);
        raise(SIGWINCH);
        exit(0);
    }
    int st = reap_or_kill(pid, 0);
    ASSERT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    static char buf[4096];
    slurp(err, buf, sizeof buf);
    close(efd);
    unlink(err);
    ASSERT_TRUE(strstr(buf, "resume with: --seed 7") != NULL);
}

TEST(ui_sigtstp_stop_and_resume) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) return;   /* never reach reap/kill paths with pid -1 */
    if (pid == 0) {
        /* Own process group: a stop signal to an ORPHANED group is
           DISCARDED by POSIX (the script(1) lesson pinned in
           test_smoke_live). With the parent in the same session but
           a different group, the child's group is non-orphaned and
           the handler's raise(SIGTSTP) genuinely stops it. */
        setpgid(0, 0);
        int nul = open("/dev/null", 1);
        dup2(nul, 2);
        ui_install_signal_handlers();
        raise(SIGTSTP);               /* handler stops the process */
        exit(0);                      /* resumes here after SIGCONT */
    }
    int st = reap_or_kill(pid, WUNTRACED);
    if (WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP) {
        kill(pid, SIGCONT);
        st = reap_or_kill(pid, 0);
        ASSERT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    } else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        /* Session-less environment discarded the stop anyway: the
           handler lines still executed and flushed (that's the
           coverage), only the actual suspension was skipped. The
           real stop/fg cycle is pinned on a PTY by test_smoke_live
           sub-check C. */
    } else {
        fprintf(stderr,
                "    [diag] raw status=0x%x exited=%d(%d) signaled=%d(%d) stopped=%d(%d)\n",
                (unsigned)st, WIFEXITED(st), WIFEXITED(st) ? WEXITSTATUS(st) : -1,
                WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : -1,
                WIFSTOPPED(st), WIFSTOPPED(st) ? WSTOPSIG(st) : -1);
        kill(pid, SIGKILL);           /* never leak a stopped child */
        waitpid(pid, &st, 0);
        ASSERT_TRUE(0);
    }
}

int main(void) {
    return RUN_ALL();
}
