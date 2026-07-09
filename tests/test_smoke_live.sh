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
    cat > /tmp/stretto_term_probe.py <<'PYEOF'
import termios, fcntl, os, sys
lf = termios.tcgetattr(0)[3]
echo   = bool(lf & termios.ECHO)
icanon = bool(lf & termios.ICANON)
nonblk = bool(fcntl.fcntl(0, fcntl.F_GETFL) & os.O_NONBLOCK)
print("PROBE echo=%d icanon=%d nonblock=%d" % (echo, icanon, nonblk))
sys.exit(0 if (echo and icanon and not nonblk) else 1)
PYEOF

    cat > /tmp/stretto_sigterm_check.sh <<EOF
#!/bin/bash
cd "$PWD"
./synth </dev/tty >/dev/null 2>&1 &
pid=\$!
sleep 2
kill -TERM "\$pid" 2>/dev/null
for _ in \$(seq 1 20); do kill -0 "\$pid" 2>/dev/null || break; sleep 0.5; done
if kill -0 "\$pid" 2>/dev/null; then kill -9 "\$pid"; echo "TERMCHECK-HANG"; exit 1; fi
wait "\$pid"; rc=\$?
python3 /tmp/stretto_term_probe.py || { echo "TERMCHECK-CORRUPT"; exit 1; }
echo "TERMCHECK-OK rc=\$rc"
EOF
    chmod +x /tmp/stretto_sigterm_check.sh

    echo "=== terminal-restore A: SIGTERM during live mode (PTY) ==="
    out_a=$(timeout 30 script -qec /tmp/stretto_sigterm_check.sh /dev/null | tr -d '\r')
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

    cat > /tmp/stretto_q_check.sh <<EOF
#!/bin/bash
cd "$PWD"
./synth >/dev/null 2>&1
rc=\$?
python3 /tmp/stretto_term_probe.py || { echo "QCHECK-CORRUPT"; exit 1; }
echo "QCHECK-OK rc=\$rc"
EOF
    chmod +x /tmp/stretto_q_check.sh

    echo "=== terminal-restore B: clean 'q' quit clears O_NONBLOCK (PTY) ==="
    out_b=$( (sleep 2; printf q; sleep 1) | timeout 30 script -qec /tmp/stretto_q_check.sh /dev/null | tr -d '\r')
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
    rm -f /tmp/stretto_term_probe.py /tmp/stretto_sigterm_check.sh /tmp/stretto_q_check.sh
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

