#!/bin/bash
set -e
cd "$(dirname "$0")/.."

./synth --render 16 /tmp/a.wav >/dev/null 2>&1
./synth --render 16 /tmp/b.wav >/dev/null 2>&1

if ! cmp -s /tmp/a.wav /tmp/b.wav; then
    echo "FAIL: determinism (renders differ)"
    echo "  a.wav: $(sha256sum /tmp/a.wav | awk '{print $1}')"
    echo "  b.wav: $(sha256sum /tmp/b.wav | awk '{print $1}')"
    exit 1
fi

if [ -f golden/regression_16s.sha256 ]; then
    EXPECTED=$(cat golden/regression_16s.sha256)
    ACTUAL=$(sha256sum /tmp/a.wav | awk '{print $1}')
    if [ "$ACTUAL" != "$EXPECTED" ]; then
        echo "FAIL: golden mismatch"
        echo "  expected: $EXPECTED"
        echo "  actual:   $ACTUAL"
        echo "  if synth output changed intentionally, run: make golden"
        exit 1
    fi
    echo "PASS: determinism + golden"
else
    echo "PASS: determinism (no golden file yet; run 'make golden' to capture)"
fi
