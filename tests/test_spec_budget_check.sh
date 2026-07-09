#!/bin/bash
#
# tests/test_spec_budget_check.sh
#
# Unit tests for tools/spec-budget-check.sh (introduced by PR #122
# / 025-spec-to-make-bridge). 5 cases that catch future regressions
# in the script's extraction regex / arithmetic / drift detection /
# malformed-input handling / state restoration logic:
#
#   1) happy-path:  current Constitution + Makefile -> exit 0
#   2) tamper Constitution: ≤48 KB UPX-packed Windows -> ≤49 KB
#                       -> exit 1, MISMATCH on WIN row
#   3) tamper Makefile: STRIP_TARGET = 51200 -> 51201
#                       -> exit 1, MISMATCH on LIN_STR row
#   4) malformed Makefile: STRIP_TARGET = "abc" (non-integer)
#                       -> exit 2, FATAL (validation rejection)
#   5) recovery: tamper both, run (expect fail), then `git checkout --`
#                       Constitution + Makefile, run again (expect pass)
#
# Exit codes:
#   0  all 5 cases PASS (5/5)
#   1  at least one case FAIL
#   2  FATAL setup failure (script missing, not inside git working tree, etc.)
#
# Style precedent: tests/test_bitexact.sh (set -e, cd to repo root,
# shebang, exit 0/1 with PASS/FAIL lines).
#
# Run: bash tests/test_spec_budget_check.sh
#
# CRITICAL: this test tampers with .specify/memory/constitution.md
# and Makefile. A cleanup trap restores both files via /tmp backup
# copies on any EXIT (success, error, or signal). If the trap
# fails (e.g., /tmp full), the user must restore manually via
# `git checkout -- .specify/memory/constitution.md Makefile`.

set -e

# --- Setup: resolve repo root + state-tracking vars ---
repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "FATAL: not inside a git working tree" >&2
    exit 2
}
cd "${repo_root}"

script="${repo_root}/tools/spec-budget-check.sh"
constitution="${repo_root}/.specify/memory/constitution.md"
makefile="${repo_root}/Makefile"

if [ ! -f "${script}" ]; then
    echo "FATAL: ${script} missing -- this test requires the bridge script from PR #122 to exist" >&2
    exit 2
fi

# Guard: refuse to run if Constitution or Makefile have un-committed
# changes. Case 5's recovery path runs `git checkout --` on both
# files, which silently clobbers un-committed work (this guard
# existed only in the amend test until a dev-box `make test` with a
# modified Makefile lost its uncommitted edits to exactly this path
# during the 044 work). Scope is limited to the 2 files the test
# tampers with, mirroring the amend test's post-#130 scoped guard.
if ! git diff --quiet HEAD -- .specify/memory/constitution.md Makefile 2>/dev/null; then
    echo "FATAL: .specify/memory/constitution.md or Makefile has un-committed changes." >&2
    echo "       Commit or stash your changes before running this test." >&2
    echo "       (Case 5 recovers via 'git checkout --' on these 2 files, which" >&2
    echo "       would clobber un-committed changes.)" >&2
    echo "       Dirty files (Constitution + Makefile only):" >&2
    git diff --name-only HEAD -- .specify/memory/constitution.md Makefile 2>/dev/null | sed 's/^/         /' >&2
    exit 1
fi

# Backup file paths (used by the cleanup trap; .bak extension so
# they're easy to spot in /tmp if cleanup fails partway).
constitution_bak="/tmp/.test_spec_budget_constitution.bak.$$"
makefile_bak="/tmp/.test_spec_budget_makefile.bak.$$"

# Cleanup trap: restore Constitution + Makefile from .bak if present.
# The trap runs on EXIT (success or failure), and is the safety net
# for any case that tampers but doesn't restore (e.g., if a sed
# command fails partway through).
cleanup() {
    local rc=$?
    if [ -f "${constitution_bak}" ]; then
        cp "${constitution_bak}" "${constitution}" 2>/dev/null || true
        rm -f "${constitution_bak}"
    fi
    if [ -f "${makefile_bak}" ]; then
        cp "${makefile_bak}" "${makefile}" 2>/dev/null || true
        rm -f "${makefile_bak}"
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

# Track test pass/fail counts. The script runs 9 sub-check
# run_case invocations across the 5 main scenarios (some scenarios
# have a post-tamper post-restore verification run_case in
# addition to the main tamper run_case). The summary at the
# end surfaces both numbers so the audit trail is unambiguous.
pass_count=0
fail_count=0
main_case_count=5

# Helper: run a single test case. Args:
#   $1 case name (short label)
#   $2 expected exit code
#   $3 required output substring (grep -F; case-insensitive if empty)
#   $4 command to run
# Side effects: increments pass_count or fail_count.
run_case() {
    local case_name="$1" expected_rc="$2" required_substr="$3" cmd="$4"
    local actual_rc=0
    local actual_output

    echo "=== Case: ${case_name} ==="

    # Run the command; capture stdout + stderr. Temporarily disable
    # `set -e` so we can capture the actual exit code without
    # the test script aborting on a failing run.
    set +e
    actual_output=$(eval "${cmd}" 2>&1)
    actual_rc=$?
    set -e

    if [ "${actual_rc}" -ne "${expected_rc}" ]; then
        echo "  FAIL: expected exit code ${expected_rc}, got ${actual_rc}"
        echo "  Output: ${actual_output}"
        fail_count=$((fail_count + 1))
        return 1
    fi

    if [ -n "${required_substr}" ] && ! echo "${actual_output}" | grep -qF "${required_substr}"; then
        echo "  FAIL: expected output to contain '${required_substr}'"
        echo "  Output: ${actual_output}"
        fail_count=$((fail_count + 1))
        return 1
    fi

    echo "  PASS: exit code ${actual_rc} (expected ${expected_rc}); required substring present"
    pass_count=$((pass_count + 1))
    return 0
}

# Helper: tamper a file with sed -i, backing up first. Args:
#   $1 = file path
#   $2 = sed expression (e.g. 's/old/new/')
#   $3 = .bak path to store the backup
# Returns 1 if the sed did NOT modify the file (i.e., pattern didn't
# match), so callers can flag a stale anchor.
tamper() {
    local file="$1" sed_expr="$2" bak="$3"
    cp "${file}" "${bak}"
    # Disable -e around sed so we can detect "no match" without aborting.
    set +e
    sed -i "${sed_expr}" "${file}"
    local sed_rc=$?
    set -e
    if [ "${sed_rc}" -ne 0 ]; then
        echo "  FAIL: sed -i '${sed_expr}' ${file} returned ${sed_rc}"
        return 1
    fi
    # Sanity: verify the file was actually modified. If the sed
    # pattern didn't match (e.g., anchor is stale), sed -i returns
    # 0 but the file is unchanged. We don't fail hard here, but
    # the run_case below will fail when expected_rc doesn't match.
    return 0
}

# --- Case 1: happy-path ---
# Run the bridge against the current (unmodified) Constitution +
# Makefile. Expect exit 0 + the "PASS: all 3 budgets aligned"
# success-line output. This is the baseline; if it fails here,
# none of the subsequent drift tests can be trusted.
echo
echo "--- Case 1: happy-path run against current Constitution + Makefile ---"
run_case "happy_path" "0" "PASS: all 3 budgets aligned" \
    "bash '${script}'" || true

# --- Case 2: tamper Constitution (≤48 KB -> ≤49 KB on the WIN budget) ---
# Bumping the Windows budget by 1 KB should make the Constitution
# imply 50176 B (49 * 1024) but Makefile's WIN_PACK_BUDGET = 49152
# still, so the bridge's MISMATCH on the WIN row is the expected
# failure mode. Output must contain the MISMATCH token.
echo
echo "--- Case 2: tamper Constitution ≤48 KB -> ≤49 KB on WIN budget ---"
tamper "${constitution}" 's/≤48 KB UPX-packed Windows/≤49 KB UPX-packed Windows/' "${constitution_bak}" || true
run_case "tamper_constitution_win" "1" "MISMATCH" \
    "bash '${script}' 2>&1" || true
# Restore via cp; remove .bak so the trap doesn't double-restore.
cp "${constitution_bak}" "${constitution}"
rm -f "${constitution_bak}"
run_case "after_constitution_restore" "0" "PASS: all 3 budgets aligned" \
    "bash '${script}'" || true

# --- Case 3: tamper Makefile (STRIP_TARGET = 51200 -> 51201) ---
# Bumping STRIP_TARGET by 1 byte should make the Makefile imply
# 51201 B but Constitution's 50 KB * 1024 = 51200 B stays, so the
# bridge's MISMATCH on the LIN_STR row is the expected failure.
# Output must contain the MISMATCH token.
echo
echo "--- Case 3: tamper Makefile STRIP_TARGET = 51200 -> 51201 ---"
tamper "${makefile}" 's/^STRIP_TARGET    = 51200/STRIP_TARGET    = 51201/' "${makefile_bak}" || true
run_case "tamper_makefile_strip" "1" "MISMATCH" \
    "bash '${script}' 2>&1" || true
# Restore via cp; remove .bak so the trap doesn't double-restore.
cp "${makefile_bak}" "${makefile}"
rm -f "${makefile_bak}"
run_case "after_makefile_restore" "0" "PASS: all 3 budgets aligned" \
    "bash '${script}'" || true

# --- Case 4: malformed Makefile constant (STRIP_TARGET = "abc") ---
# Setting STRIP_TARGET to a non-integer string should trigger the
# script's `case` validation in extract_make_var and exit 2 with
# a FATAL message on stderr. Tests the script's defensive
# arithmetic-expression-or-string rejection.
echo
echo "--- Case 4: malformed Makefile constant STRIP_TARGET = abc ---"
tamper "${makefile}" 's/^STRIP_TARGET    = 51200/STRIP_TARGET    = abc/' "${makefile_bak}" || true
run_case "malformed_constant_strip" "2" "FATAL" \
    "bash '${script}' 2>&1" || true
# Restore via cp; remove .bak so the trap doesn't double-restore.
cp "${makefile_bak}" "${makefile}"
rm -f "${makefile_bak}"
run_case "after_malformed_restore" "0" "PASS: all 3 budgets aligned" \
    "bash '${script}'" || true

# --- Case 5: recovery via git checkout ---
# This case is the explicit "the script + working tree can recover
# from a tampered state" test. It tampers BOTH the Constitution
# and the Makefile (so MISMATCH is guaranteed), runs the script
# (expect exit 1), then uses `git checkout -- <files>` to restore
# from HEAD. The post-recovery run should be exit 0. This is the
# closest analogue to a real CI scenario: a PR mutates these files
# (intentionally or otherwise), CI fails, the dev restores via git
# checkout, and the next run passes.
echo
echo "--- Case 5: recovery via git checkout (tamper both, recover, re-run) ---"
cp "${constitution}" "${constitution_bak}"
cp "${makefile}" "${makefile_bak}"
set +e
sed -i 's/≤48 KB UPX-packed Windows/≤49 KB UPX-packed Windows/' "${constitution}"
sed -i 's/^STRIP_TARGET    = 51200/STRIP_TARGET    = 51201/' "${makefile}"
set -e
# Pre-check: tampered state should fail
run_case "tampered_state" "1" "MISMATCH" \
    "bash '${script}' 2>&1" || true
# Now: drop the .bak files (so the trap doesn't double-restore)
# and recover via `git checkout --` from HEAD. This is the
# actual recovery path under test.
rm -f "${constitution_bak}" "${makefile_bak}"
git checkout -- "${constitution}" "${makefile}"
# Verify: clean working tree
if ! git diff --quiet HEAD -- "${constitution}" "${makefile}"; then
    echo "  FAIL: git checkout did not fully restore ${constitution} / ${makefile}"
    fail_count=$((fail_count + 1))
else
    echo "  git checkout verified clean (no diff vs HEAD)"
    # Final post-recovery run
    run_case "after_git_checkout" "0" "PASS: all 3 budgets aligned" \
        "bash '${script}'" || true
fi

# --- Final summary ---
echo
echo "=== Test summary ==="
echo "  main scenarios: ${main_case_count} (1 happy + 1 tamper-const + 1 tamper-makefile + 1 malformed + 1 recovery)"
echo "  sub-checks passed: ${pass_count}"
echo "  sub-checks failed: ${fail_count}"
echo "  working tree: $(git status --porcelain | wc -l) file(s) dirty (Constitution + Makefile should be clean post-trap)"

if [ "${fail_count}" -gt 0 ]; then
    echo
    echo "FAIL: at least one sub-check did not produce the expected exit code or output substring."
    echo "      see FAIL: lines above for the specific sub-check that broke."
    exit 1
fi

echo
echo "PASS: all ${main_case_count} main scenarios green (${pass_count} sub-checks); tools/spec-budget-check.sh is regression-tested."
exit 0
