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

echo "=== audio characteristic bounds ==="
# Compute time- and frequency-domain features per render. Catches:
#   - SVF runaway / reverb feedback overflow (peak / clip)
#   - drum or voice saturation (peak / clip)
#   - filter blown wide open (centroid above ambient range)
#   - filter stuck closed / synth gone to subsonic (centroid low)
#   - synth gone to noise (ZCR high)
#   - synth gone to drone (ZCR low)
# Spectral centroid + ZCR catch character drift without forcing
# golden regen on every algorithmic tweak (those bounds are wide).
# `if ! python3` (not a bare call + $? check): under set -e a bare
# failing python3 kills the script HERE - the golden comparison below
# never ran and the FAIL summary never printed.
if ! python3 - <<'PY' "$TMPDIR" "${SEEDS[@]}"
import sys, struct, os
import numpy as np

SAMPLE_RATE = 48000

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

    # Mono-mix for spectral analysis.
    mono = (samples[0::2].astype(np.float32) + samples[1::2].astype(np.float32)) * 0.5

    # Spectral centroid: magnitude-weighted mean frequency. A measure of
    # spectral brightness; correlates roughly with perceived tone color.
    spec = np.abs(np.fft.rfft(mono))
    freqs = np.fft.rfftfreq(len(mono), 1.0 / SAMPLE_RATE)
    mag_sum = float(spec.sum())
    centroid = float((freqs * spec).sum() / mag_sum) if mag_sum > 0 else 0.0

    # Zero-crossing rate: fraction of adjacent-sample sign changes.
    # Crude spectral-content proxy that doesn't need an FFT to interpret.
    sign_changes = int(np.sum(mono[1:] * mono[:-1] < 0))
    zcr = sign_changes / max(len(mono) - 1, 1)

    # Sanity bounds. Wide enough to absorb normal algorithmic tweaks,
    # tight enough to catch the failure modes above.
    errs = []
    if peak < 500:                 errs.append(f"peak too low ({peak})")
    if peak >= 32767:              errs.append(f"saturated peak ({peak})")
    if clip > 100:                 errs.append(f"clipping count ({clip})")
    if nonzero < len(samples) // 2: errs.append(f"too many silent samples ({nonzero})")
    if not (100.0 <= centroid <= 5000.0):
        errs.append(f"centroid out of [100, 5000] Hz ({centroid:.0f})")
    if not (0.01 <= zcr <= 0.30):
        errs.append(f"ZCR out of [0.01, 0.30] ({zcr:.3f})")

    if errs:
        ok = False
        print(f"  seed {s}: FAIL -- {', '.join(errs)}  "
              f"[peak={peak} rms={rms:.0f} clip={clip} "
              f"centroid={centroid:.0f}Hz zcr={zcr:.3f}]")
    else:
        print(f"  seed {s}: ok  peak={peak} rms={rms:.0f} clip={clip} "
              f"centroid={centroid:.0f}Hz zcr={zcr:.3f}")

sys.exit(0 if ok else 1)
PY
then fail=1; fi

GOLDEN=golden/regression_multiseed.sha256.txt
echo "=== golden hash comparison ==="
if [ -f "$GOLDEN" ]; then
    for s in "${SEEDS[@]}"; do
        expected=$(grep "^seed_${s}:" "$GOLDEN" | awk '{print $2}')
        actual=$(sha256sum "$TMPDIR/a_$s.wav" | awk '{print $1}')
        if [ -z "$expected" ]; then
            # A missing entry is a FAILURE, not a note: a golden file
            # that silently lost a seed row would stop guarding it.
            echo "  seed $s: FAIL  no entry in golden file"
            fail=1
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
