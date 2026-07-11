#!/bin/bash
# Live-mode smoke test.
#
# Spawns ./synth with a 2-second timeout. Expects exit code:
#   124 - SIGTERM from timeout (good, the program ran until killed)
#     0 - clean exit (also good)
#   anything else - bug / crash
#
# Two sub-checks:
#   1. Audio smoke: ./synth --no-ui (drives audio via PulseAudio)
#   2. MIDI smoke:  ./synth --midi --no-ui (drives ALSA sequencer
#      via snd-seq-dummy loopback so the wildcard subscribe path is
#      exercised; synthetic controller appears as a MIDI_GENERIC
#      readable port)
#
# Skipped automatically if PulseAudio is unavailable (CI without
# an audio server, or restricted container). The MIDI sub-check
# is independently skipped if snd-seq-dummy is not available in
# the kernel module search path (most production rigs + stock
# Ubuntu runners have it; some minimal containers do not).

set -e
cd "$(dirname "$0")/.."

# All probe scripts + captured stderr live under one mktemp dir:
# fixed /tmp/stretto_* names collide across concurrent checkouts and
# are plantable by other users on shared machines.
ST=$(mktemp -d)
trap 'rm -rf "$ST"' EXIT

# Probe PA availability. If pactl can't reach a server within
# 1 second, skip the test rather than fail it.
if ! timeout 1 pactl info >/dev/null 2>&1; then
    echo "SKIP: no PulseAudio server reachable"
    exit 0
fi

if [ ! -x ./synth ]; then
    echo "FAIL: ./synth not built; run 'make' first"
    exit 1
fi

echo "=== spawning ./synth for 2s ==="
set +e
timeout --preserve-status 2 ./synth --no-ui >/dev/null 2>&1 &
pid=$!
sleep 2
kill -TERM "$pid" 2>/dev/null
wait $pid
rc=$?
set -e

case $rc in
    0|124|143)
        echo "PASS: live-mode smoke (exit $rc)"
        ;;
    139)
        echo "FAIL: SIGSEGV in live mode"
        exit 1
        ;;
    *)
        echo "FAIL: unexpected exit code $rc"
        exit 1
        ;;
esac

# ----- Terminal-restore sub-checks (Linux signal handler + fcntl restore) -----
# A: SIGTERM during live-with-UI mode must leave the PTY with ECHO and
#    ICANON restored (ui.c sig_restore_and_reraise), and the process
#    must die by the signal (wait status 143 via the re-raise).
# B: a clean 'q' quit must clear O_NONBLOCK on stdin. O_NONBLOCK lives
#    on the open file description, so B runs synth in the PTY
#    FOREGROUND with inherited stdin and probes the same fd afterward
#    (a </dev/tty redirect would open a fresh description and make the
#    assertion vacuous).
# A uses SIGTERM, not SIGINT: non-interactive shells start background
# jobs with SIGINT ignored, so a SIGINT-based check would silently not
# kill the synth. Both sub-checks need script(1) + python3 to drive a
# real PTY; skipped cleanly if unavailable. These run BEFORE the MIDI
# sub-check because its modprobe gate exits 0 on non-root runners.
if ! command -v script >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: script(1) or python3 unavailable; terminal-restore sub-checks skipped"
else
    cat > "$ST"/term_probe.py <<'PYEOF'
import termios, fcntl, os, sys
lf = termios.tcgetattr(0)[3]
echo   = bool(lf & termios.ECHO)
icanon = bool(lf & termios.ICANON)
nonblk = bool(fcntl.fcntl(0, fcntl.F_GETFL) & os.O_NONBLOCK)
print("PROBE echo=%d icanon=%d nonblock=%d" % (echo, icanon, nonblk))
sys.exit(0 if (echo and icanon and not nonblk) else 1)
PYEOF

    cat > "$ST"/sigterm_check.sh <<EOF
#!/bin/bash
cd "$PWD"
./synth </dev/tty >/dev/null 2>&1 &
pid=\$!
sleep 2
kill -TERM "\$pid" 2>/dev/null
for _ in \$(seq 1 20); do kill -0 "\$pid" 2>/dev/null || break; sleep 0.5; done
if kill -0 "\$pid" 2>/dev/null; then kill -9 "\$pid"; echo "TERMCHECK-HANG"; exit 1; fi
wait "\$pid"; rc=\$?
python3 "$ST"/term_probe.py || { echo "TERMCHECK-CORRUPT"; exit 1; }
echo "TERMCHECK-OK rc=\$rc"
EOF
    chmod +x "$ST"/sigterm_check.sh

    echo "=== terminal-restore A: SIGTERM during live mode (PTY) ==="
    out_a=$(timeout 30 script -qec "$ST"/sigterm_check.sh /dev/null | tr -d '\r')
    echo "$out_a"
    case "$out_a" in
        *"TERMCHECK-OK rc=143"*)
            echo "PASS: SIGTERM terminal restore (exit 143)"
            ;;
        *)
            pkill -x synth 2>/dev/null || true
            echo "FAIL: SIGTERM left the terminal corrupted, hung, or wrong exit status"
            exit 1
            ;;
    esac

    # synth stderr goes to a file: script(1) merges the PTY streams,
    # so the resume line can't be fished out of the transcript.
    cat > "$ST"/q_check.sh <<EOF
#!/bin/bash
cd "$PWD"
./synth >/dev/null 2>"$ST"/q_stderr
rc=\$?
python3 "$ST"/term_probe.py || { echo "QCHECK-CORRUPT"; exit 1; }
echo "QCHECK-OK rc=\$rc"
EOF
    chmod +x "$ST"/q_check.sh

    echo "=== terminal-restore B: clean 'q' quit clears O_NONBLOCK (PTY) ==="
    # Feed 's' (cycle scale -> lydian) before 'q' so the resume line
    # must capture the touched parameter, not just the seed.
    out_b=$( (sleep 2; printf s; sleep 1; printf q; sleep 1) | timeout 30 script -qec "$ST"/q_check.sh /dev/null | tr -d '\r')
    echo "$out_b"
    case "$out_b" in
        *"QCHECK-OK rc=0"*)
            echo "PASS: clean-q fcntl + termios restore"
            ;;
        *)
            pkill -x synth 2>/dev/null || true
            echo "FAIL: clean 'q' quit left O_NONBLOCK set or termios raw"
            exit 1
            ;;
    esac

    # Preset capture (specs/004-preset-capture): the quit path must
    # print a pasteable resume line with the seed and the parameter
    # touched by the 's' keypress above.
    if grep -qE '^resume with: --seed [0-9]+ --scale lydian$' "$ST"/q_stderr; then
        echo "PASS: clean-q resume line captures seed + touched scale"
    else
        echo "FAIL: resume line missing or wrong; synth stderr was:"
        cat "$ST"/q_stderr
        exit 1
    fi

    # C: SIGTSTP (Ctrl-Z) must restore the terminal BEFORE stopping,
    #    and re-enter raw mode on `fg`. This needs a REAL interactive
    #    shell with job control: in script(1)'s harness the synth sits
    #    in an ORPHANED process group, where POSIX makes the kernel
    #    discard stop signals entirely - the handler's stop is a no-op
    #    and nothing suspends. The driver below forks interactive bash
    #    on a fresh PTY (job control on, synth gets its own foreground
    #    pgrp), types the actual Ctrl-Z byte, and probes the SLAVE
    #    termios directly from outside the session at each phase:
    #    raw while running -> sane + state T while stopped -> raw
    #    again after fg -> sane after clean q.
    cat > "$ST"/tstp_pty.py <<PYEOF
import os, pty, sys, termios, time, subprocess, signal

# Probe notes:
# - Slave termios is only meaningful while the SYNTH owns the
#   foreground: whenever bash is at its prompt, readline installs its
#   own raw-ish modes, masking whatever the synth left behind.
# - O_NONBLOCK lives on the OPEN FILE DESCRIPTION shared by bash and
#   the synth (inherited fd 0) and readline never touches it, so
#   /proc/<pid>/fdinfo/0 is the unambiguous restore/re-raw signal in
#   every phase. Pre-fix, a stopped synth left it set - the exact
#   historical leak the fcntl restore exists for.

def probe_raw(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY)
    try:
        lf = termios.tcgetattr(fd)[3]
        return not (lf & termios.ECHO) and not (lf & termios.ICANON)
    finally:
        os.close(fd)

def fd0_nonblock(pid):
    with open("/proc/%d/fdinfo/0" % pid) as f:
        for line in f:
            if line.startswith("flags:"):
                return bool(int(line.split()[1], 8) & 0o4000)
    return False

def synth_state():
    p = subprocess.run(["pgrep", "-nx", "synth"], capture_output=True, text=True)
    if p.returncode != 0:
        return None, ""
    pid = int(p.stdout.strip())
    with open("/proc/%d/stat" % pid) as f:
        return pid, f.read().split(") ", 1)[1].split()[0]

def die(marker, child):
    print(marker)
    subprocess.run(["pkill", "-9", "-x", "synth"], capture_output=True)
    os.kill(child, signal.SIGKILL)
    sys.exit(1)

child, master = pty.fork()
if child == 0:
    os.execvp("bash", ["bash", "--norc", "-i"])
slave = os.readlink("/proc/%d/fd/0" % child)

def drain():
    # keep the PTY's output buffer from filling and blocking the shell
    import select
    while select.select([master], [], [], 0)[0]:
        try:
            if not os.read(master, 65536): break
        except OSError:
            break

time.sleep(1); drain()
os.write(master, b"cd " + os.environ["SYNTH_DIR"].encode() + b" && ./synth\n")
time.sleep(2); drain()
pid, st = synth_state()
if pid is None: die("TSTP-PTY-NO-SYNTH", child)
if not probe_raw(slave): die("TSTP-PTY-NOT-RAW-AT-START", child)
if not fd0_nonblock(pid): die("TSTP-PTY-NO-NONBLOCK-AT-START", child)

os.write(master, b"\x1a")          # the actual Ctrl-Z byte
time.sleep(1.5); drain()
pid, st = synth_state()
if st != "T": die("TSTP-PTY-NOSTOP st=%s" % st, child)
if fd0_nonblock(pid): die("TSTP-PTY-NONBLOCK-WHILE-STOPPED", child)

os.write(master, b"fg\n")
time.sleep(1.5); drain()
pid, st = synth_state()
if st not in ("R", "S"): die("TSTP-PTY-NO-RESUME st=%s" % st, child)
if not probe_raw(slave): die("TSTP-PTY-NOT-RERAW-AFTER-FG", child)
if not fd0_nonblock(pid): die("TSTP-PTY-NO-NONBLOCK-AFTER-FG", child)

os.write(master, b"q")             # clean quit from the resumed session
time.sleep(1.5); drain()
pid, st = synth_state()
if pid is not None: die("TSTP-PTY-STILL-RUNNING-AFTER-Q", child)
if fd0_nonblock(child): die("TSTP-PTY-NONBLOCK-LEAKED-TO-SHELL", child)

os.write(master, b"exit\n")
time.sleep(0.5); drain()
os.waitpid(child, 0)
print("TSTP-PTY-OK")
PYEOF

    echo "=== terminal-restore C: Ctrl-Z restores, fg re-raws (interactive PTY) ==="
    out_c=$(SYNTH_DIR="$PWD" timeout 60 python3 "$ST"/tstp_pty.py | tr -d '\r')
    echo "$out_c"
    case "$out_c" in
        *"TSTP-PTY-OK"*)
            echo "PASS: SIGTSTP suspend/resume terminal handling"
            ;;
        *)
            pkill -x synth 2>/dev/null || true
            echo "FAIL: SIGTSTP suspend/resume terminal handling (marker above)"
            exit 1
            ;;
    esac

    rm -f "$ST"/term_probe.py "$ST"/sigterm_check.sh \
          "$ST"/q_check.sh "$ST"/q_stderr "$ST"/tstp_pty.py
fi

# ----- MIDI smoke sub-check (specs/003-midi-input/T039) -----
# Try to load snd-seq-dummy. If modprobe fails (container without
# the module, kernel built without SND_SEQUENCER, etc.), skip the
# MIDI sub-check rather than fail the whole smoke test. We try
# snd_seq_dummy (underscore form) too because some kernel module
# registries use the underscore instead of the dash - whichever
# modprobe resolves first wins.
if ! modprobe snd-seq-dummy 2>/dev/null && ! modprobe snd_seq_dummy 2>/dev/null; then
    echo "SKIP: snd-seq-dummy not loadable; MIDI smoke sub-check skipped"
    exit 0
fi

echo "=== spawning ./synth --midi --no-ui for 2s (snd-seq-dummy loopback) ==="
set +e
timeout --preserve-status 2 ./synth --midi --no-ui >/dev/null 2>&1 &
pid=$!
sleep 2
kill -TERM "$pid" 2>/dev/null
wait $pid
rc=$?
set -e

case $rc in
    0|124|143)
        echo "PASS: --midi wildcard smoke (exit $rc)"
        ;;
    139)
        echo "FAIL: SIGSEGV in --midi wildcard mode"
        exit 1
        ;;
    *)
        echo "FAIL: unexpected --midi wildcard exit code $rc"
        exit 1
        ;;
esac

# Best-effort cleanup of the virtual sequencer port so a subsequent
# run sees a fresh loopback (modprobe -r may not always succeed
# depending on consumer state - silence error to keep the smoke
# test green on the happy path).
modprobe -r snd-seq-dummy 2>/dev/null || true

