#!/bin/bash
set -e

./synth --render 30 /tmp/a.wav
./synth --render 30 /tmp/b.wav

if cmp -s /tmp/a.wav /tmp/b.wav; then
    echo "PASS: bit-exact determinism confirmed"
    exit 0
else
    echo "FAIL: renders differ"
    echo "a.wav: $(sha256sum /tmp/a.wav)"
    echo "b.wav: $(sha256sum /tmp/b.wav)"
    exit 1
fi
