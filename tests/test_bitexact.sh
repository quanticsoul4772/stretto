#!/bin/bash
set -e
cd "$(dirname "$0")/.."

# mktemp, not fixed /tmp/a.wav names: those collide across concurrent
# checkouts (two CI jobs / two worktrees on one box overwrite each
# other's renders and "fail" determinism spuriously).
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

./synth --render 16 "$T/a.wav" --seed 0 >/dev/null 2>&1
./synth --render 16 "$T/b.wav" --seed 0 >/dev/null 2>&1

if ! cmp -s "$T/a.wav" "$T/b.wav"; then
    echo "FAIL: determinism (renders differ)"
    echo "  a.wav: $(sha256sum "$T/a.wav" | awk '{print $1}')"
    echo "  b.wav: $(sha256sum "$T/b.wav" | awk '{print $1}')"
    exit 1
fi

if [ -f golden/regression_16s.sha256 ]; then
    EXPECTED=$(cat golden/regression_16s.sha256)
    ACTUAL=$(sha256sum "$T/a.wav" | awk '{print $1}')
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
