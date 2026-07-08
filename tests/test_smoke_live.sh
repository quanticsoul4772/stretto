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

