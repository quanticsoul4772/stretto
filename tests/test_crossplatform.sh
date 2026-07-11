#!/bin/bash
# Cross-platform bit-exactness test (Constitution Principle III).
#
# Renders the four multiseed seeds (4 s) plus the 16 s golden seed with
# BOTH binaries built from this tree - the Linux ./synth and the
# mingw-cross-compiled ./stretto.exe - and asserts every pair is
# byte-identical, and that the 16 s render matches the committed
# golden on both platforms.
#
# Where it runs: a dev box that can execute both binaries - WSL with
# Windows interop (the .exe runs natively via the interop layer), or
# any setup where ./stretto.exe is directly executable. CI cannot run
# it (Linux runners can't execute PE binaries), so it is NOT part of
# `make test`; run it via `make test-crossplatform` after toolchain or
# engine changes.
#
# Scope note: both binaries share the build-host-generated lookup
# tables (gen_*_table.c runs once, on the host). This test therefore
# proves the RUNTIME engine is platform-independent (integer-only DSP,
# xorshift32 PRNG, little-endian RIFF emit); table generation itself
# is pinned by building releases on one runner image, which is exactly
# why ci.yml/release.yml pin ubuntu-24.04.
#
# The .exe gets RELATIVE output paths: Windows fopen cannot resolve
# /mnt/... WSL paths, but the interop layer translates the working
# directory, so a relative name lands in the same directory for both.

set -e
cd "$(dirname "$0")/.."

if [ ! -x ./synth ]; then
    echo "FAIL: ./synth not built; run 'make' first"
    exit 1
fi
if [ ! -f ./stretto.exe ]; then
    echo "SKIP: stretto.exe not built (run 'make win'); cross-platform test skipped"
    exit 0
fi
if ! ./stretto.exe --version >/dev/null 2>&1; then
    echo "SKIP: cannot execute stretto.exe here (no WSL interop / not Windows)"
    exit 0
fi

# Temp dir must live under the repo: the .exe writes via the interop
# cwd translation, and the repo is the one tree both worlds can see.
T=$(mktemp -d ./xplat.XXXXXX)
trap 'rm -rf "$T"' EXIT

SEEDS=(0 1 42 12345)
fail=0

echo "=== rendering ${#SEEDS[@]} seeds x 4 s on both platforms ==="
for s in "${SEEDS[@]}"; do
    ./synth --render 4 "$T/linux_$s.wav" --seed "$s" >/dev/null 2>&1
    (cd "$T" && ../stretto.exe --render 4 "win_$s.wav" --seed "$s" >/dev/null 2>&1)
    if cmp -s "$T/linux_$s.wav" "$T/win_$s.wav"; then
        echo "  seed $s: identical ($(sha256sum "$T/linux_$s.wav" | cut -c1-16)...)"
    else
        echo "  seed $s: FAIL - Linux and Windows renders differ"
        echo "    linux: $(sha256sum "$T/linux_$s.wav" | awk '{print $1}')"
        echo "    win:   $(sha256sum "$T/win_$s.wav"   | awk '{print $1}')"
        fail=1
    fi
done

echo "=== 16 s seed-0 render vs the committed golden, both platforms ==="
./synth --render 16 "$T/linux_16s.wav" --seed 0 >/dev/null 2>&1
(cd "$T" && ../stretto.exe --render 16 "win_16s.wav" --seed 0 >/dev/null 2>&1)
golden=$(cat golden/regression_16s.sha256)
for side in linux win; do
    h=$(sha256sum "$T/${side}_16s.wav" | awk '{print $1}')
    if [ "$h" = "$golden" ]; then
        echo "  $side: matches golden"
    else
        echo "  $side: FAIL - expected $golden, got $h"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "FAIL: cross-platform bit-exactness"
    exit 1
fi
echo
echo "PASS: cross-platform bit-exactness (Linux glibc == mingw-w64 Windows, byte-for-byte)"
