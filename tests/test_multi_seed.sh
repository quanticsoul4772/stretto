#!/bin/bash
# Multi-seed integration test.
#
# Renders 4 seconds with each of a fixed set of seeds, checks:
#   1. Each render is byte-identical across two runs (determinism).
#   2. Each render's sha256 differs from every other (seeds vary output).
#   3. Audio characteristics (peak, RMS, clipping) within sane bounds.
#      Catches runaway-state bugs like the SVF self-oscillation we
#      shipped and reverted in feat/filter-controls / fix/svf-stability.
#
# The expected sha256s are stored in golden/regression_multiseed.sha256.txt
# so future runs catch unintentional output drift.

set -e
cd "$(dirname "$0")/.."

SEEDS=(0 1 42 12345)
SECS=4
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

fail=0

echo "=== rendering each seed twice ==="
for s in "${SEEDS[@]}"; do
    ./synth --render $SECS "$TMPDIR/a_$s.wav" --seed $s >/dev/null 2>&1
    ./synth --render $SECS "$TMPDIR/b_$s.wav" --seed $s >/dev/null 2>&1
    if ! cmp -s "$TMPDIR/a_$s.wav" "$TMPDIR/b_$s.wav"; then
        echo "FAIL: seed $s not deterministic across two runs"
        fail=1
    fi
done

echo "=== verifying all four seeds produce different output ==="
declare -A hashes
for s in "${SEEDS[@]}"; do
    h=$(sha256sum "$TMPDIR/a_$s.wav" | awk '{print $1}')
    if [[ -n "${hashes[$h]}" ]]; then
        echo "FAIL: seed $s collides with seed ${hashes[$h]} (hash $h)"
        fail=1
    fi
    hashes[$h]=$s
done

echo "=== audio sanity bounds ==="
# Use python to compute peak / rms / clip-count and apply bounds.
# Catches SVF runaway, reverb feedback overflow, drum saturation, etc.
python3 - <<'PY' "$TMPDIR" "${SEEDS[@]}"
import sys, struct, os
import numpy as np

tmpdir = sys.argv[1]
seeds = sys.argv[2:]

ok = True
for s in seeds:
    path = os.path.join(tmpdir, f"a_{s}.wav")
    with open(path, "rb") as f:
        f.read(44)
        raw = f.read()
    samples = np.array(struct.unpack(f"<{len(raw)//2}h", raw), dtype=np.int32)
    peak = int(np.max(np.abs(samples)))
    rms  = float(np.sqrt((samples.astype(np.float32) ** 2).mean()))
    clip = int(np.sum((samples == 32767) | (samples == -32768)))
    nonzero = int(np.sum(samples != 0))

    # Sanity bounds. The render is 4 seconds stereo at 48 kHz = 384k
    # samples. We expect:
    #   - some signal: peak > 500 (not silent)
    #   - bounded: peak < 32000 (not saturated)
    #   - low clipping: clip < 100 samples out of 384000
    #   - mostly non-zero: > 50% of samples not exactly 0
    errs = []
    if peak < 500:       errs.append(f"peak too low ({peak})")
    if peak >= 32767:    errs.append(f"saturated peak ({peak})")
    if clip > 100:       errs.append(f"clipping count ({clip})")
    if nonzero < len(samples) // 2: errs.append(f"too many silent samples ({nonzero})")

    if errs:
        ok = False
        print(f"  seed {s}: FAIL -- {', '.join(errs)}  [peak={peak} rms={rms:.0f} clip={clip}]")
    else:
        print(f"  seed {s}: ok  peak={peak} rms={rms:.0f} clip={clip}")

sys.exit(0 if ok else 1)
PY
audio_status=$?
if [ $audio_status -ne 0 ]; then fail=1; fi

GOLDEN=golden/regression_multiseed.sha256.txt
echo "=== golden hash comparison ==="
if [ -f "$GOLDEN" ]; then
    for s in "${SEEDS[@]}"; do
        expected=$(grep "^seed_${s}:" "$GOLDEN" | awk '{print $2}')
        actual=$(sha256sum "$TMPDIR/a_$s.wav" | awk '{print $1}')
        if [ -z "$expected" ]; then
            echo "  seed $s: no entry in golden file"
        elif [ "$expected" != "$actual" ]; then
            echo "  seed $s: FAIL  expected $expected, got $actual"
            fail=1
        else
            echo "  seed $s: ok"
        fi
    done
else
    echo "  no golden file; run 'make golden-multiseed' to capture"
fi

if [ $fail -eq 0 ]; then
    echo
    echo "PASS: multi-seed integration"
else
    echo
    echo "FAIL: multi-seed integration"
    exit 1
fi
