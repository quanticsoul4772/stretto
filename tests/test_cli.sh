#!/bin/bash
# CLI contract test for --help / -h / --version (GNU Coding Standards
# 4.8.1 / 4.8.2): both flags print to STDOUT, exit 0, keep stderr
# clean, and take precedence over every other option and argument -
# including side-effecting ones like --render. Needs no TTY, no
# PulseAudio, no audio device, so it runs unconditionally everywhere
# (unlike the smoke test's PA-gated sub-checks).

set -e
cd "$(dirname "$0")/.."

if [ ! -x ./synth ]; then
    echo "FAIL: ./synth not built; run 'make' first"
    exit 1
fi

fail=0

# --- --help: exit 0, stdout starts with "usage:", stderr empty ---
if ! out=$(./synth --help 2>/tmp/cli_stderr); then
    echo "FAIL: --help exited non-zero"; fail=1
fi
case "$out" in
    usage:*) ;;
    *) echo "FAIL: --help stdout does not start with 'usage:'"; fail=1 ;;
esac
if [ -s /tmp/cli_stderr ]; then
    echo "FAIL: --help wrote to stderr"; fail=1
fi

# --- -h: same contract as --help ---
if ! ./synth -h >/dev/null 2>&1; then
    echo "FAIL: -h exited non-zero"; fail=1
fi

# --- --version: exit 0, first line "stretto <ver>" (version after the
#     last space per GNU 4.8.1), stderr empty ---
if ! out=$(./synth --version 2>/tmp/cli_stderr); then
    echo "FAIL: --version exited non-zero"; fail=1
fi
case "$out" in
    "stretto "*) ;;
    *) echo "FAIL: --version first line does not start with 'stretto '"; fail=1 ;;
esac
if [ -s /tmp/cli_stderr ]; then
    echo "FAIL: --version wrote to stderr"; fail=1
fi

# --- precedence + side-effect suppression: --help wins over --render
#     (exit 0 AND no output file created - help must suppress the
#     render itself, not just the exit code) ---
rm -f /tmp/cli_help.wav
if ! ./synth --help --render 60 /tmp/cli_help.wav >/dev/null 2>&1; then
    echo "FAIL: '--help --render ...' exited non-zero"; fail=1
fi
if [ -e /tmp/cli_help.wav ]; then
    echo "FAIL: --help did not suppress the render (output file created)"
    rm -f /tmp/cli_help.wav
    fail=1
fi

# --- usage errors still go to stderr with exit 1 (unchanged contract) ---
set +e
./synth --definitely-not-a-flag >/tmp/cli_stdout 2>/tmp/cli_stderr
rc=$?
set -e
if [ "$rc" -ne 1 ]; then
    echo "FAIL: unknown flag exited $rc (expected 1)"; fail=1
fi
if [ -s /tmp/cli_stdout ]; then
    echo "FAIL: unknown-flag usage error wrote to stdout"; fail=1
fi
if ! grep -q '^usage:' /tmp/cli_stderr; then
    echo "FAIL: unknown-flag usage error missing 'usage:' on stderr"; fail=1
fi

# --- stdout render ('-' path): byte-identical to a file render ---
rm -f /tmp/cli_a.wav /tmp/cli_b.wav
if ! ./synth --render 2 /tmp/cli_a.wav --seed 0 2>/dev/null; then
    echo "FAIL: file render exited non-zero"; fail=1
fi
if ! ./synth --render 2 - --seed 0 >/tmp/cli_b.wav 2>/dev/null; then
    echo "FAIL: stdout render exited non-zero"; fail=1
fi
if ! cmp -s /tmp/cli_a.wav /tmp/cli_b.wav; then
    echo "FAIL: stdout render differs from file render (must be byte-identical)"
    fail=1
fi

# --- non-TTY stdin/stdout degrades to headless (050) ---
# `./synth </dev/null` used to die on the ENOTTY tcgetattr before
# playing a sample. It must now degrade to --no-ui (parity with the
# Windows GetConsoleMode path): headless notice on stderr, no
# tcgetattr error, and the process either runs until the timeout
# kills it (124/143) or exits 1 later at PulseAudio connect on
# PA-less machines - either way it got PAST terminal setup.
set +e
timeout --preserve-status 2 ./synth </dev/null >/tmp/cli_stdout 2>/tmp/cli_stderr
rc=$?
set -e
if grep -q "tcgetattr" /tmp/cli_stderr; then
    echo "FAIL: non-TTY stdin still dies on tcgetattr"; fail=1
fi
if ! grep -q "running headless" /tmp/cli_stderr; then
    echo "FAIL: non-TTY run missing the headless notice"; fail=1
fi
case "$rc" in
    124|143|1|0) ;;
    *) echo "FAIL: non-TTY run exited $rc (expected 124/143 or a PA-connect 1)"; fail=1 ;;
esac

# --- preset-capture flags (specs/004-preset-capture) ---
# (a) All flags at their explicit defaults (minus --cutoff, whose
#     compile-time default 200 sits above the 30..180 dial range)
#     must render byte-identically to a flagless run: proves the
#     setters consume no PRNG and perturb nothing.
DEFAULT_FLAGS="--scale dorian --bar-ms 2000 --gate 200 --mod-depth 1500 \
--resonance 100 --lfo-depth 80 --filter-mode lp --reverb 60 --delay 100 \
--feedback 140 --comp-threshold 20000"
rm -f /tmp/cli_p0.wav /tmp/cli_p1.wav /tmp/cli_p2.wav /tmp/cli_p3.wav
if ! ./synth --render 2 /tmp/cli_p0.wav --seed 0 2>/dev/null; then
    echo "FAIL: flagless preset baseline render exited non-zero"; fail=1
fi
if ! ./synth --render 2 /tmp/cli_p1.wav --seed 0 $DEFAULT_FLAGS 2>/dev/null; then
    echo "FAIL: explicit-defaults render exited non-zero"; fail=1
fi
if ! cmp -s /tmp/cli_p0.wav /tmp/cli_p1.wav; then
    echo "FAIL: explicit-default flags changed the output (setters must be inert at defaults)"
    fail=1
fi
# (b) A non-default flag must change the output...
if ! ./synth --render 2 /tmp/cli_p2.wav --seed 0 --scale lydian --cutoff 180 2>/dev/null; then
    echo "FAIL: flagged render exited non-zero"; fail=1
fi
if cmp -s /tmp/cli_p0.wav /tmp/cli_p2.wav; then
    echo "FAIL: --scale lydian --cutoff 180 did not change the output"; fail=1
fi
# (c) ...deterministically: same flags twice, byte-identical.
if ! ./synth --render 2 /tmp/cli_p3.wav --seed 0 --scale lydian --cutoff 180 2>/dev/null; then
    echo "FAIL: flagged render (repeat) exited non-zero"; fail=1
fi
if ! cmp -s /tmp/cli_p2.wav /tmp/cli_p3.wav; then
    echo "FAIL: flagged render is not reproducible"; fail=1
fi
# (d) Out-of-range and malformed values are usage errors (exit 1,
#     stderr), never silent clamps.
for bad in "--gate 999" "--gate abc" "--scale nope" "--bar-ms 100" \
           "--filter-mode xy" "--comp-threshold 7999"; do
    set +e
    ./synth --render 1 /tmp/cli_bad.wav $bad >/tmp/cli_stdout 2>/tmp/cli_stderr
    rc=$?
    set -e
    if [ "$rc" -ne 1 ]; then
        echo "FAIL: '$bad' exited $rc (expected 1)"; fail=1
    fi
    if [ -s /tmp/cli_stdout ]; then
        echo "FAIL: '$bad' wrote to stdout"; fail=1
    fi
done
rm -f /tmp/cli_bad.wav
# (e) Render mode prints the seed capture line on stderr.
if ! ./synth --render 1 /tmp/cli_p0.wav --seed 42 2>/tmp/cli_stderr; then
    echo "FAIL: seed-line render exited non-zero"; fail=1
fi
if ! grep -q '^resume with: --seed 42$' /tmp/cli_stderr; then
    echo "FAIL: render did not print 'resume with: --seed 42' on stderr"; fail=1
fi
rm -f /tmp/cli_p0.wav /tmp/cli_p1.wav /tmp/cli_p2.wav /tmp/cli_p3.wav

# --- broken pipe: downstream closing early must terminate the render
#     promptly. 141 = died by SIGPIPE (default disposition, standard
#     pipeline behavior); 1 = the fwrite error check, for parents
#     that inherit SIGPIPE ignored. Asserting exactly 141 would be a
#     latent flake for zero extra confidence. ---
set +e
timeout 20 bash -c './synth --render 60 - --seed 0 2>/dev/null | head -c 1000 >/dev/null; exit ${PIPESTATUS[0]}'
rc=$?
set -e
case "$rc" in
    141|1) ;;
    124) echo "FAIL: broken-pipe render hung (timeout)"; fail=1 ;;
    *) echo "FAIL: broken-pipe render exited $rc (expected 141 or 1)"; fail=1 ;;
esac

# --- man page (048-packaging) ---
# Skipped where groff is unavailable (e.g. Git Bash on Windows dev
# boxes); ubuntu runners have it.
if ! command -v groff >/dev/null 2>&1; then
    echo "SKIP: groff unavailable; man-page checks skipped"
else
    # (a) Lint, ENFORCING: groff exits 0 on warnings (stderr only),
    #     so the gate is "-ww (all warnings) + stderr must be empty".
    #     -t preprocesses the tbl key table.
    err=$(groff -man -t -ww -Tutf8 -z stretto.1 2>&1)
    if [ -n "$err" ]; then
        echo "FAIL: groff reports man-page problems:"
        echo "$err"
        fail=1
    fi
    # (b) Help<->man drift gate: every --flag token the binary's
    #     --help emits must appear in stretto.1. A checker, not a
    #     generator - the man page stays hand-written.
    # Normalize the roff escapes (\- renders as -) before matching.
    man_flat=$(sed 's/\\-/-/g' stretto.1)
    help_flags=$(./synth --help | grep -oE -- '--[a-z][a-z-]+' | sort -u)
    for f in $help_flags; do
        if ! printf '%s' "$man_flat" | grep -q -- "$f"; then
            echo "FAIL: --help flag '$f' is missing from stretto.1"
            fail=1
        fi
    done
fi

rm -f /tmp/cli_stderr /tmp/cli_stdout /tmp/cli_a.wav /tmp/cli_b.wav

if [ "$fail" -ne 0 ]; then
    echo "FAIL: CLI contract test"
    exit 1
fi
echo "PASS: CLI contract (--help / -h / --version / usage errors)"
