#!/bin/bash
# CLI contract test for --help / -h / --version (GNU Coding Standards
# 4.8.1 / 4.8.2): both flags print to STDOUT, exit 0, keep stderr
# clean, and take precedence over every other option and argument -
# including side-effecting ones like --render. The core checks need no
# TTY, no PulseAudio, no audio device. Some later blocks are guarded:
# groff (man page), curl (offline install.sh tests), uname=Linux (the
# no-server UX check, which links libpulse behavior) - each skips
# cleanly where its tool is absent.

set -e
cd "$(dirname "$0")/.."

if [ ! -x ./synth ]; then
    echo "FAIL: ./synth not built; run 'make' first"
    exit 1
fi

# Single mktemp workdir for EVERY temp file in this script (fixed
# /tmp/cli_* names collide across concurrent checkouts and can be
# pre-planted by another user on shared machines). One trap owns
# cleanup - the install.sh block below nests its dirs under $CT so it
# never needs a second (trap-clobbering) EXIT trap.
CT=$(mktemp -d)
trap 'rm -rf "$CT"' EXIT

fail=0

# --- --help: exit 0, stdout starts with "usage:", stderr empty ---
if ! out=$(./synth --help 2>"$CT"/cli_stderr); then
    echo "FAIL: --help exited non-zero"; fail=1
fi
case "$out" in
    usage:*) ;;
    *) echo "FAIL: --help stdout does not start with 'usage:'"; fail=1 ;;
esac
if [ -s "$CT"/cli_stderr ]; then
    echo "FAIL: --help wrote to stderr"; fail=1
fi

# --- -h: same contract as --help ---
if ! ./synth -h >/dev/null 2>&1; then
    echo "FAIL: -h exited non-zero"; fail=1
fi

# --- --version: exit 0, first line "stretto <ver>" (version after the
#     last space per GNU 4.8.1), stderr empty ---
if ! out=$(./synth --version 2>"$CT"/cli_stderr); then
    echo "FAIL: --version exited non-zero"; fail=1
fi
case "$out" in
    "stretto "*) ;;
    *) echo "FAIL: --version first line does not start with 'stretto '"; fail=1 ;;
esac
if [ -s "$CT"/cli_stderr ]; then
    echo "FAIL: --version wrote to stderr"; fail=1
fi

# --- precedence + side-effect suppression: --help wins over --render
#     (exit 0 AND no output file created - help must suppress the
#     render itself, not just the exit code) ---
rm -f "$CT"/cli_help.wav
if ! ./synth --help --render 60 "$CT"/cli_help.wav >/dev/null 2>&1; then
    echo "FAIL: '--help --render ...' exited non-zero"; fail=1
fi
if [ -e "$CT"/cli_help.wav ]; then
    echo "FAIL: --help did not suppress the render (output file created)"
    rm -f "$CT"/cli_help.wav
    fail=1
fi

# --- usage errors still go to stderr with exit 1 (unchanged contract) ---
set +e
timeout 10 ./synth --definitely-not-a-flag >"$CT"/cli_stdout 2>"$CT"/cli_stderr
rc=$?
set -e
if [ "$rc" -ne 1 ]; then
    echo "FAIL: unknown flag exited $rc (expected 1)"; fail=1
fi
if [ -s "$CT"/cli_stdout ]; then
    echo "FAIL: unknown-flag usage error wrote to stdout"; fail=1
fi
if ! grep -q '^usage:' "$CT"/cli_stderr; then
    echo "FAIL: unknown-flag usage error missing 'usage:' on stderr"; fail=1
fi

# --- stdout render ('-' path): byte-identical to a file render ---
rm -f "$CT"/cli_a.wav "$CT"/cli_b.wav
if ! ./synth --render 2 "$CT"/cli_a.wav --seed 0 2>/dev/null; then
    echo "FAIL: file render exited non-zero"; fail=1
fi
if ! ./synth --render 2 - --seed 0 >"$CT"/cli_b.wav 2>/dev/null; then
    echo "FAIL: stdout render exited non-zero"; fail=1
fi
if ! cmp -s "$CT"/cli_a.wav "$CT"/cli_b.wav; then
    echo "FAIL: stdout render differs from file render (must be byte-identical)"
    fail=1
fi

# --- positional overflow is an explicit usage error (051 / D9) ---
# The pre-scan used to STOP at the 8th positional, silently dropping
# everything after it - including a trailing recognized flag like
# --seed. Overflow must now be a loud usage error.
set +e
timeout 10 ./synth x1 x2 x3 x4 x5 x6 x7 x8 --seed 5 >"$CT"/cli_stdout 2>"$CT"/cli_stderr
rc=$?
set -e
if [ "$rc" -ne 1 ]; then
    echo "FAIL: positional overflow exited $rc (expected 1)"; fail=1
fi
if ! grep -q '^too many arguments' "$CT"/cli_stderr; then
    echo "FAIL: positional overflow missing 'too many arguments' on stderr"; fail=1
fi
if [ -s "$CT"/cli_stdout ]; then
    echo "FAIL: positional overflow wrote to stdout"; fail=1
fi

# --- non-TTY stdin/stdout degrades to headless (050) ---
# `./synth </dev/null` used to die on the ENOTTY tcgetattr before
# playing a sample. It must now degrade to --no-ui (parity with the
# Windows GetConsoleMode path): headless notice on stderr, no
# tcgetattr error, and the process either runs until the timeout
# kills it (124/143) or exits 1 later at PulseAudio connect on
# PA-less machines - either way it got PAST terminal setup.
set +e
timeout --preserve-status 2 ./synth </dev/null >"$CT"/cli_stdout 2>"$CT"/cli_stderr
rc=$?
set -e
if grep -q "tcgetattr" "$CT"/cli_stderr; then
    echo "FAIL: non-TTY stdin still dies on tcgetattr"; fail=1
fi
if ! grep -q "running headless" "$CT"/cli_stderr; then
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
rm -f "$CT"/cli_p0.wav "$CT"/cli_p1.wav "$CT"/cli_p2.wav "$CT"/cli_p3.wav
if ! ./synth --render 2 "$CT"/cli_p0.wav --seed 0 2>/dev/null; then
    echo "FAIL: flagless preset baseline render exited non-zero"; fail=1
fi
if ! ./synth --render 2 "$CT"/cli_p1.wav --seed 0 $DEFAULT_FLAGS 2>/dev/null; then
    echo "FAIL: explicit-defaults render exited non-zero"; fail=1
fi
if ! cmp -s "$CT"/cli_p0.wav "$CT"/cli_p1.wav; then
    echo "FAIL: explicit-default flags changed the output (setters must be inert at defaults)"
    fail=1
fi
# (b) A non-default flag must change the output...
if ! ./synth --render 2 "$CT"/cli_p2.wav --seed 0 --scale lydian --cutoff 180 2>/dev/null; then
    echo "FAIL: flagged render exited non-zero"; fail=1
fi
if cmp -s "$CT"/cli_p0.wav "$CT"/cli_p2.wav; then
    echo "FAIL: --scale lydian --cutoff 180 did not change the output"; fail=1
fi
# (c) ...deterministically: same flags twice, byte-identical.
if ! ./synth --render 2 "$CT"/cli_p3.wav --seed 0 --scale lydian --cutoff 180 2>/dev/null; then
    echo "FAIL: flagged render (repeat) exited non-zero"; fail=1
fi
if ! cmp -s "$CT"/cli_p2.wav "$CT"/cli_p3.wav; then
    echo "FAIL: flagged render is not reproducible"; fail=1
fi
# (d) Out-of-range and malformed values are usage errors (exit 1,
#     stderr), never silent clamps.
for bad in "--gate 999" "--gate abc" "--scale nope" "--bar-ms 100" \
           "--filter-mode xy" "--comp-threshold 7999"; do
    set +e
    ./synth --render 1 "$CT"/cli_bad.wav $bad >"$CT"/cli_stdout 2>"$CT"/cli_stderr
    rc=$?
    set -e
    if [ "$rc" -ne 1 ]; then
        echo "FAIL: '$bad' exited $rc (expected 1)"; fail=1
    fi
    if [ -s "$CT"/cli_stdout ]; then
        echo "FAIL: '$bad' wrote to stdout"; fail=1
    fi
done
rm -f "$CT"/cli_bad.wav
# (e) Render mode prints the seed capture line on stderr.
if ! ./synth --render 1 "$CT"/cli_p0.wav --seed 42 2>"$CT"/cli_stderr; then
    echo "FAIL: seed-line render exited non-zero"; fail=1
fi
if ! grep -q '^resume with: --seed 42$' "$CT"/cli_stderr; then
    echo "FAIL: render did not print 'resume with: --seed 42' on stderr"; fail=1
fi
rm -f "$CT"/cli_p0.wav "$CT"/cli_p1.wav "$CT"/cli_p2.wav "$CT"/cli_p3.wav

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

# --- no-audio-server UX (058): the error must be legible ---
# An explicit PULSE_SERVER disables libpulse's autospawn/fallback
# chain, so pointing it at a nonexistent socket fails deterministically
# (async PA_CONTEXT_FAILED -> exit 1) even on machines WITH a working
# server. Linux-gated: ./synth is a Linux ELF and the check exercises
# libpulse behavior.
if [ "$(uname -s)" = "Linux" ]; then
    set +e
    PULSE_SERVER=unix:/nonexistent/socket timeout --preserve-status 10 \
        ./synth --no-ui >"$CT"/cli_stdout 2>"$CT"/cli_stderr
    rc=$?
    set -e
    if [ "$rc" -ne 1 ]; then
        echo "FAIL: no-server run exited $rc (expected 1)"; fail=1
    fi
    if ! grep -q "pipewire-pulse" "$CT"/cli_stderr; then
        echo "FAIL: no-server error does not mention pipewire-pulse; stderr was:"
        cat "$CT"/cli_stderr
        fail=1
    fi
else
    echo "SKIP: no-server UX check (non-Linux dev box)"
fi

# --- install.sh (058): offline checksum-handling tests ---
# The deep-research rationale for a self-written installer was "test
# your own checksum handling" - these run it for real, offline, via a
# file:// base URL (needs curl; wget cannot fetch file://). A fake
# dist dir stands in for a release; install.sh never EXECUTES the
# binary, which is what makes fake binaries valid here. Everything
# under mktemp -d; STRETTO_INSTALL_DIR is ALWAYS set so no fake
# artifact can ever land in a real ~/.local/bin.
if ! sh -n install.sh; then
    echo "FAIL: install.sh does not parse under sh -n"; fail=1
fi
# Linux gate as well as curl: install.sh's platform check dies on any
# other uname, so on a curl-equipped mac/Git-Bash box the happy path
# below would "fail" for the wrong reason.
if [ "$(uname -s)" != "Linux" ]; then
    echo "SKIP: offline install.sh tests (install.sh is Linux-only; this is $(uname -s))"
elif ! command -v curl >/dev/null 2>&1; then
    echo "SKIP: curl unavailable; offline install.sh tests skipped"
else
    idist=$(mktemp -d -p "$CT")
    idest=$(mktemp -d -p "$CT")
    printf 'fake stretto binary\n' > "$idist/stretto-vTEST-linux-x86_64"
    printf 'fake upx binary\n'     > "$idist/stretto-vTEST-linux-x86_64-upx"
    printf '.TH STRETTO 1\n'       > "$idist/stretto.1"
    printf '# fake bash completion\n' > "$idist/stretto.bash"
    printf '#compdef stretto\n'       > "$idist/_stretto"
    (cd "$idist" && sha256sum -- * > sha256sums.txt)

    # (1) happy path: installs all four files, prints the epilogue.
    # $idist is absolute (mktemp -d), so file://$idist is file:///abs.
    if ! out=$(STRETTO_BASE_URL="file://$idist" STRETTO_INSTALL_DIR="$idest" sh install.sh 2>&1); then
        echo "FAIL: offline install.sh happy path exited non-zero:"
        printf '%s\n' "$out"
        fail=1
    fi
    case "$out" in
        *"hear it now"*) ;;
        *) echo "FAIL: install.sh epilogue missing"; fail=1 ;;
    esac
    if [ ! -x "$idest/stretto" ] || [ ! -f "$idest/stretto.1" ]; then
        echo "FAIL: install.sh did not install binary + man page"; fail=1
    fi
    # Flat mode keeps the ASSET names (068): stretto.bash / _stretto -
    # the bash completion's canonical name "stretto" would clobber the
    # binary in this mode.
    if [ ! -f "$idest/stretto.bash" ] || [ ! -f "$idest/_stretto" ]; then
        echo "FAIL: install.sh did not install the completion files"; fail=1
    fi

    # (1b) v1.4.0-shape dist: NO completion assets. The header's
    # compatibility promise covers every published release, so this
    # must still succeed and install only binary + man page.
    iold=$(mktemp -d -p "$CT")
    idest_old=$(mktemp -d -p "$CT")
    printf 'fake stretto binary\n' > "$iold/stretto-vTEST-linux-x86_64"
    printf 'fake upx binary\n'     > "$iold/stretto-vTEST-linux-x86_64-upx"
    printf '.TH STRETTO 1\n'       > "$iold/stretto.1"
    (cd "$iold" && sha256sum -- * > sha256sums.txt)
    if ! STRETTO_BASE_URL="file://$iold" STRETTO_INSTALL_DIR="$idest_old" sh install.sh >/dev/null 2>&1; then
        echo "FAIL: install.sh broke on a pre-completions release layout"; fail=1
    fi
    if [ ! -x "$idest_old/stretto" ] || [ -e "$idest_old/stretto.bash" ]; then
        echo "FAIL: pre-completions layout installed the wrong file set"; fail=1
    fi

    # (1c) sums LISTS stretto.bash but the asset is missing: presence
    # in sums makes the download mandatory - must die loudly, nothing
    # installed (the exact boundary of the conditionality).
    rm -f "$iold/sha256sums.txt"
    printf '# fake bash completion\n' > "$iold/stretto.bash"
    (cd "$iold" && sha256sum -- * > sha256sums.txt)
    rm -f "$iold/stretto.bash"
    idest_404=$(mktemp -d -p "$CT")
    set +e
    out=$(STRETTO_BASE_URL="file://$iold" STRETTO_INSTALL_DIR="$idest_404" sh install.sh 2>&1)
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        echo "FAIL: install.sh ignored a sums-listed completion that failed to download"; fail=1
    fi
    if [ -e "$idest_404/stretto" ]; then
        echo "FAIL: completion-404 install left a binary behind"; fail=1
    fi

    # (2) tampered binary: loud mismatch, nothing installed.
    printf 'tampered\n' >> "$idist/stretto-vTEST-linux-x86_64"
    idest2=$(mktemp -d -p "$CT")
    set +e
    out=$(STRETTO_BASE_URL="file://$idist" STRETTO_INSTALL_DIR="$idest2" sh install.sh 2>&1)
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        echo "FAIL: install.sh accepted a tampered binary"; fail=1
    fi
    case "$out" in
        *"MISMATCH"*) ;;
        *) echo "FAIL: tampered install missing the MISMATCH message"; fail=1 ;;
    esac
    if [ -e "$idest2/stretto" ]; then
        echo "FAIL: tampered install left a binary behind"; fail=1
    fi

    # (3) asset missing from the sums file: refuse. Regenerate sums
    # first so the (still-tampered) binary matches again - otherwise
    # this test would die at the binary MISMATCH before reaching the
    # missing-entry path for stretto.1.
    (cd "$idist" && sha256sum stretto-vTEST-linux-x86_64 \
        stretto-vTEST-linux-x86_64-upx stretto.1 > sha256sums.txt)
    grep -v "stretto\.1" "$idist/sha256sums.txt" > "$idist/sums.tmp" \
        && mv "$idist/sums.tmp" "$idist/sha256sums.txt"
    set +e
    out=$(STRETTO_BASE_URL="file://$idist" STRETTO_INSTALL_DIR="$idest2" sh install.sh 2>&1)
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        echo "FAIL: install.sh accepted an asset absent from sha256sums.txt"; fail=1
    fi
    case "$out" in
        *"no entry for"*) ;;
        *) echo "FAIL: missing-entry install lacked the refusal message"; fail=1 ;;
    esac
    rm -rf "$idist" "$idest" "$idest2"   # eager; $CT's trap is the backstop
fi

# --- man page (048-packaging) ---
# Skipped where groff is unavailable (e.g. Git Bash on Windows dev
# boxes); ubuntu runners have it.
if ! command -v groff >/dev/null 2>&1; then
    echo "SKIP: groff unavailable; man-page checks skipped"
else
    # (a) Lint, ENFORCING: groff exits 0 on warnings (stderr only),
    #     so the gate is "-ww (all warnings) + stderr must be empty".
    #     -t preprocesses the tbl key table.
    # `if ! err=$(...)`: a bare err=$(groff ...) under set -e kills
    # the whole script on a non-zero groff exit, skipping the FAIL
    # report (and every check after it).
    if ! err=$(groff -man -t -ww -Tutf8 -z stretto.1 2>&1) || [ -n "$err" ]; then
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
    # Vacuity guard: an empty extraction would loop zero times and
    # pass silently. --help documents 15+ flags; demand a sane floor.
    if [ "$(printf '%s\n' $help_flags | wc -w)" -lt 10 ]; then
        echo "FAIL: --help flag extraction looks broken (got: $help_flags)"
        fail=1
    fi
    for f in $help_flags; do
        # Right-boundary match: a plain substring grep lets --midi
        # ride on --midi-list-devices' entry.
        if ! printf '%s' "$man_flat" | grep -qE -- "$f([^a-z-]|\$)"; then
            echo "FAIL: --help flag '$f' is missing from stretto.1"
            fail=1
        fi
    done
fi

# --- shell completions (068): bidirectional help<->completions drift ---
# Deliberately OUTSIDE the groff guard: this needs only ./synth --help
# and grep, and the dev boxes most likely to hand-edit completions are
# exactly the ones without groff. Checker, not generator - both
# completion files stay hand-written.
comp_help_flags=$(./synth --help | grep -oE -- '--[a-z][a-z-]+' | sort -u)
if [ "$(printf '%s\n' $comp_help_flags | wc -w)" -lt 10 ]; then
    echo "FAIL: --help flag extraction looks broken (got: $comp_help_flags)"
    fail=1
fi
# Syntax gates: bash always (bash runs this script); zsh when present
# (not preinstalled on the CI runner - a skip, not a hole: the drift
# gates below are pure text and always run).
if ! bash -n completions/stretto.bash; then
    echo "FAIL: completions/stretto.bash does not parse under bash -n"; fail=1
fi
if command -v zsh >/dev/null 2>&1; then
    if ! zsh -n completions/_stretto; then
        echo "FAIL: completions/_stretto does not parse under zsh -n"; fail=1
    fi
else
    echo "SKIP: zsh unavailable; _stretto syntax check skipped"
fi
for cf in completions/stretto.bash completions/_stretto; do
    if [ ! -f "$cf" ]; then
        echo "FAIL: $cf missing"; fail=1; continue
    fi
    # Forward: every help flag must be completable.
    for f in $comp_help_flags; do
        if ! grep -qE -- "$f([^a-z-]|\$)" "$cf"; then
            echo "FAIL: --help flag '$f' is missing from $cf"
            fail=1
        fi
    done
    # Reverse: every flag the completion advertises must still exist
    # in --help (the forward gate can never catch a REMOVED flag
    # lingering here).
    comp_flags=$(grep -oE -- '--[a-z][a-z-]+' "$cf" | sort -u)
    if [ "$(printf '%s\n' $comp_flags | wc -w)" -lt 10 ]; then
        echo "FAIL: flag extraction from $cf looks broken (got: $comp_flags)"
        fail=1
    fi
    for f in $comp_flags; do
        if ! printf '%s\n' $comp_help_flags | grep -qx -- "$f"; then
            echo "FAIL: $cf advertises '$f' which is not in --help"
            fail=1
        fi
    done
done

rm -f "$CT"/cli_stderr "$CT"/cli_stdout "$CT"/cli_a.wav "$CT"/cli_b.wav

if [ "$fail" -ne 0 ]; then
    echo "FAIL: CLI contract test"
    exit 1
fi
echo "PASS: CLI contract (--help / -h / --version / usage errors)"
