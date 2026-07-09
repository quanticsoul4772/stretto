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

rm -f /tmp/cli_stderr /tmp/cli_stdout

if [ "$fail" -ne 0 ]; then
    echo "FAIL: CLI contract test"
    exit 1
fi
echo "PASS: CLI contract (--help / -h / --version / usage errors)"
