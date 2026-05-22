#!/bin/bash
# Live-mode smoke test.
#
# Spawns ./synth with a 2-second timeout. Expects exit code:
#   124 - SIGTERM from timeout (good, the program ran until killed)
#     0 - clean exit (also good)
#   anything else - bug / crash
#
# Skipped automatically if PulseAudio is unavailable (CI without
# an audio server, or restricted container).

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
