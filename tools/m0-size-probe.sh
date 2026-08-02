#!/bin/bash
#
# tools/m0-size-probe.sh
#
# THROWAWAY. Branch 089-m0-size-probe, never merged.
#
# Answers one question the Zig-port work has been getting wrong twice:
# when a module moves from C to Zig, what exactly is the binary paying
# for?
#
# The prior measurement said "+88 B for density, +68 B for
# chord_progression" and framed the open question as flat-per-module vs
# proportional-to-lines. Both framings ignore the mechanism. CFLAGS
# carries -Os -flto -fuse-linker-plugin, so every .c is GIMPLE bitcode
# that gcc inlines across module boundaries at link time. A
# `zig build-obj` output is a plain ELF object; the GCC LTO plugin does
# not claim it. So a ported module leaves the LTO unit, and its callers
# lose cross-module inlining into it. That cost has nothing to do with
# Zig.
#
# Variant 1 separates the two terms without writing any Zig: density.c,
# still C, still gcc, compiled -fno-lto. Same LTO boundary as a Zig
# object, identical source semantics. Whatever that costs is LTO exit;
# whatever a Zig object costs beyond it is codegen.
#
# Variants 3 and 4 then check whether the Zig rule was even tuned. The
# release CFLAGS carry -ffunction-sections -fdata-sections (so
# -Wl,--gc-sections has something to work with), -fno-asynchronous-
# unwind-tables (and note `strip -s` does NOT remove .eh_frame), and
# -fno-pic. None of those transferred to the measured `zig build-obj`
# invocation, so the published numbers price an untuned rule.
#
# Method notes that make the numbers meaningful:
#
#  * STRETTO_VERSION is pinned on the command line for every variant.
#    version.h embeds `git describe --tags --always --dirty` and is
#    compiled into main.o. Swapping density.c for density.zig makes the
#    tree dirty by construction, so the control (clean) and the Zig
#    variants (dirty) would embed strings of different length -- worth
#    up to ~16 B against a budget with ~670 B of headroom. Pinning it
#    removes the term entirely. STRETTO_VERSION is a `:=` variable, so
#    a command-line assignment wins.
#
#  * Every variant is built in ONE job at ONE commit, for the same
#    reason.
#
#  * The compile line for variant 1 is lifted from `make -n` rather
#    than hand-copied, so it is CFLAGS parity by construction. Only
#    -flto -> -fno-lto differs.
#
#  * Each variant replaces density.o AFTER make has built it, leaving
#    density.o newer than density.c. make therefore relinks without
#    recompiling. `make synth` also runs the strip, so stripped sizes
#    stay comparable.
#
#  * Page-cliff headroom is reported per variant. The code segment pays
#    file size in whole 4 KB pages; tools/size-budget-gate.sh records
#    that the 063 arc lost a CI round-trip to a cliff no local
#    measurement showed. A 70 B module that crosses a boundary costs
#    4096 B stripped, and that is not visible in the byte delta alone.

set -euo pipefail

VER=size-probe
RESULTS=/tmp/m0-results.txt
: > "$RESULTS"

banner() { printf '\n=== %s ===\n' "$1"; }

# Build, pack, and record one variant. Assumes density.o (and possibly
# chord_progression.o) are already in place and newer than their
# sources, so this relinks rather than recompiling.
measure() {
    local label="$1"
    make -s STRETTO_VERSION="$VER" synth
    rm -f synth.packed
    upx -qq --ultra-brute synth -o synth.packed

    local stripped packed seg pad
    stripped=$(stat -c%s synth)
    packed=$(stat -c%s synth.packed)

    # Same LOAD-segment selection the Makefile's `size` target uses:
    # first R+E segment. Guard before arithmetic -- $(( )) on an empty
    # string is 0, which would report a maximal false headroom.
    seg=$(readelf -lW synth 2>/dev/null | \
          awk '$1 == "LOAD" && $7 == "R" && $8 == "E" { print $5; exit }')
    if [ -n "$seg" ]; then
        seg=$((seg)); pad=$(( (4096 - seg % 4096) % 4096 ))
    else
        seg=missing; pad=missing
    fi

    printf '%-38s stripped=%-8s packed=%-8s code_seg=%-8s cliff_headroom=%s\n' \
        "$label" "$stripped" "$packed" "$seg" "$pad" | tee -a "$RESULTS"
}

# Render 16 s at seed 0 and compare against the committed golden. A
# size number from a binary that renders different audio is not a
# measurement of anything.
check_hash() {
    local label="$1" golden h
    golden=$(cat golden/regression_16s.sha256)
    if ./synth --render 16 /tmp/probe.wav --seed 0 >/dev/null 2>&1; then
        h=$(sha256sum /tmp/probe.wav | cut -d' ' -f1)
        if [ "$h" = "$golden" ]; then
            printf '%-38s hash=MATCHES golden\n' "$label" | tee -a "$RESULTS"
        else
            printf '%-38s hash=DIFFERS %s\n' "$label" "$h" | tee -a "$RESULTS"
        fi
    else
        printf '%-38s hash=RENDER FAILED\n' "$label" | tee -a "$RESULTS"
    fi
}

banner "toolchain"
gcc --version | head -1
zig version
upx --version | head -1
echo "describe: $(git describe --tags --always --dirty)"
echo "STRETTO_VERSION pinned to: $VER"

banner "zig build-obj accepted flags (diagnostic)"
# Printed unconditionally so that if a tuned-variant flag is rejected
# below, the reason is in the same log rather than a second CI round.
zig build-obj --help 2>&1 | \
    grep -E '^\s*-(f(no-)?(function-sections|data-sections|unwind-tables|omit-frame-pointer|PIC|PIE|stack-check|sanitize)|O[A-Za-z]*|target)' \
    || echo "(flag grep found nothing; full help follows)"

banner "variant 0: control, all gcc -flto"
make STRETTO_VERSION="$VER"
measure "0 control (all C, gcc -flto)"
check_hash "0 control"

banner "variant 1: density.c compiled -fno-lto (THE LTO/CODEGEN SPLIT)"
# Lift the exact compile line make would use, then flip one flag. This
# is CFLAGS parity by construction rather than by transcription.
touch density.c
CC_LINE=$(make -n STRETTO_VERSION="$VER" density.o | grep -m1 '^gcc')
echo "make would run : $CC_LINE"
NOLTO_LINE=${CC_LINE/-flto/-fno-lto}
echo "probe runs     : $NOLTO_LINE"
eval "$NOLTO_LINE"
measure "1 density.c -fno-lto (still C)"
check_hash "1 density.c -fno-lto"

banner "variant 2: density.zig, untuned rule (as previously measured)"
zig build-obj -OReleaseSmall -target x86_64-linux-gnu \
    -femit-bin=density.o zig/density.zig
echo "--- density.o sections ---"
readelf -SW density.o | awk '{print $2, $3, $7}' | grep -E '^\.' || true
echo "--- density.o undefined symbols (Principle II check) ---"
nm -u density.o || true
measure "2 density.zig untuned"
check_hash "2 density.zig untuned"

banner "variant 3: density.zig, tuned to match CFLAGS intent"
zig build-obj -OReleaseSmall -target x86_64-linux-gnu \
    -ffunction-sections -fdata-sections \
    -fno-unwind-tables -fomit-frame-pointer \
    -fno-PIC -fno-PIE \
    -femit-bin=density.o zig/density.zig
echo "--- density.o sections (tuned) ---"
readelf -SW density.o | awk '{print $2, $3, $7}' | grep -E '^\.' || true
measure "3 density.zig tuned"
check_hash "3 density.zig tuned"

banner "variant 4: density.zig + chord_progression.zig, tuned"
zig build-obj -OReleaseSmall -target x86_64-linux-gnu \
    -ffunction-sections -fdata-sections \
    -fno-unwind-tables -fomit-frame-pointer \
    -fno-PIC -fno-PIE \
    -femit-bin=chord_progression.o zig/chord_progression.zig
measure "4 density+chord .zig tuned"
check_hash "4 density+chord .zig tuned"

banner "RESULTS"
cat "$RESULTS"

banner "BUDGETS (Constitution Principle I)"
echo "budget_linux_synth_stripped=51200"
echo "budget_linux_synth_packed=30720"
echo
echo "CI-authoritative baseline for comparison: 30048 B packed"
echo "(constitution.md Principle I, run 29211125164 on c7db9fc)"
echo
echo "Read variant 1 first: it is the whole cost of leaving the LTO"
echo "unit, measured in C. Variant 2 minus variant 1 is Zig codegen."
echo "Variant 3 minus variant 2 is what the untuned rule was wasting."
