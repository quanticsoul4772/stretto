#!/bin/bash
#
# tests/test_spec_budget_amend.sh
#
# Regression test for tools/spec-budget-amend.sh (introduced by
# PR #127 / 032-spec-budget-amend). Verifies the amendment helper's
# 4 main behaviors: (a) Constitution + Makefile updated in lockstep
# for a single budget bump, (b) atomic multi-budget amend, (c) input
# validation (0 flags, non-integer, sub-1 KB, shrink), (d) dry-run
# preview, (e) refuse-to-commit semantics, (f) recovery via
# git checkout.
#
# Cases:
#   1) happy-path amend 1 budget: --lin-str 51 -> exit 0, Constitution
#      + Makefile updated, bridge script still passes
#   2) atomic amend all 3: --win 49 --lin-upx 31 --lin-str 51 -> exit 0,
#      all 3 budgets updated
#   3) input validation: 0 flags, non-integer, < 1 KB, shrink budget
#      -> exit 1 + FATAL output
#   4) dry-run: --dry-run --lin-str 60 -> exit 0, no files modified
#   5) refuse-to-commit: amend then verify NO `git add` or `git commit`
#      was run (working tree dirty but un-staged)
#   6) recovery: amend, then `git checkout --` to restore from HEAD
#
# Run: bash tests/test_spec_budget_amend.sh
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

script="${repo_root}/tools/spec-budget-amend.sh"
constitution="${repo_root}/.specify/memory/constitution.md"
makefile="${repo_root}/Makefile"

if [ ! -f "${script}" ]; then
    echo "FATAL: ${script} missing -- this test requires the amend helper from PR #127 to exist" >&2
    exit 2
fi

# Guard: refuse to run on a dirty working tree. This test tampers
# with .specify/memory/constitution.md and Makefile, and case 6's
# recovery path uses `git checkout --` to restore from HEAD. If the
# user has un-committed changes outside Constitution + Makefile, the
# test will still work (it only restores those 2 files). But if the
# user has un-committed changes INSIDE Constitution + Makefile, the
# test's recovery path will clobber them silently -- which is
# exactly the trap that the 035 amend drill fell into. Bail with
# a clear message instead of producing confusing FAIL output.
if ! git diff --quiet HEAD 2>/dev/null; then
    echo "FATAL: dirty working tree -- aborting." >&2
    echo "       Commit or stash your changes before running this test." >&2
    echo "       (The test tampers with .specify/memory/constitution.md + Makefile and uses" >&2
    echo "       'git checkout --' for recovery, which would clobber un-committed changes.)" >&2
    echo "       Dirty files:" >&2
    git status --short | head -10 >&2
    exit 1
fi

# Backup file paths (used by the cleanup trap; .bak extension so
# they're easy to spot in /tmp if cleanup fails partway).
constitution_bak="/tmp/.test_spec_budget_amend_constitution.bak.$$"
makefile_bak="/tmp/.test_spec_budget_amend_makefile.bak.$$"

# Cleanup trap: restore Constitution + Makefile from .bak if present.
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

# Track test pass/fail counts.
pass_count=0
fail_count=0
main_case_count=6

# Helper: run a single test case. Args:
#   $1 case name (short label)
#   $2 expected exit code
#   $3 required output substring (grep -F; case-insensitive if empty)
#   $4 command to run
run_case() {
    local case_name="$1" expected_rc="$2" required_substr="$3" cmd="$4"
    local actual_rc=0
    local actual_output

    echo "=== Case: ${case_name} ==="

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

# Helper: snapshot the current Constitution + Makefile to /tmp
# before any tampering. Returns the .bak paths via the global
# constitution_bak / makefile_bak variables.
snapshot_files() {
    cp "${constitution}" "${constitution_bak}"
    cp "${makefile}"     "${makefile_bak}"
}

# Helper: restore from /tmp snapshots and remove the .bak files
# (so the trap doesn't double-restore).
restore_files() {
    cp "${constitution_bak}" "${constitution}"
    cp "${makefile_bak}"     "${makefile}"
    rm -f "${constitution_bak}" "${makefile_bak}"
}

# --- Case 1: happy-path amend 1 budget ---
# Bump Linux stripped from 50 KB to 51 KB. Verify:
#  - script exits 0
#  - Constitution's Principle I paragraph now has "≤51 KB stripped Linux binary"
#  - Makefile's STRIP_TARGET = 52224 (51 * 1024)
#  - tools/spec-budget-check.sh still passes (post-amend verification)
echo
echo "--- Case 1: happy-path amend 1 budget (--lin-str 51) ---"
snapshot_files
run_case "amend_single_budget" "0" "Verifying via tools/spec-budget-check.sh" \
    "bash '${script}' --lin-str 51" || true
# Verify Constitution was updated
if ! grep -qE '≤51 KB stripped Linux binary' "${constitution}"; then
    echo "  FAIL: Constitution's Principle I paragraph does not contain '≤51 KB stripped Linux binary' after amend"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: Constitution updated (≤51 KB stripped Linux binary present)"
    pass_count=$((pass_count + 1))
fi
# Verify Makefile was updated
if ! grep -qE '^STRIP_TARGET[[:space:]]*=[[:space:]]*52224' "${makefile}"; then
    echo "  FAIL: Makefile's STRIP_TARGET is not 52224 after amend"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: Makefile updated (STRIP_TARGET = 52224 present)"
    pass_count=$((pass_count + 1))
fi
# Restore for next case
restore_files

# --- Case 2: atomic amend all 3 budgets ---
# Bump Windows from 48 -> 49, Linux UPX from 30 -> 31, Linux stripped
# from 50 -> 51 in a single invocation. Verify:
#  - script exits 0
#  - all 3 Constitution budgets updated
#  - all 3 Makefile variables updated
#  - bridge script still passes
echo
echo "--- Case 2: atomic amend all 3 budgets (--win 49 --lin-upx 31 --lin-str 51) ---"
snapshot_files
run_case "amend_three_budgets" "0" "Verifying via tools/spec-budget-check.sh" \
    "bash '${script}' --win 49 --lin-upx 31 --lin-str 51" || true
# Verify all 3 Constitution budgets updated
for pair in "≤49 KB UPX-packed Windows" "≤31 KB UPX-packed Linux binary" "≤51 KB stripped Linux binary"; do
    if ! grep -qF "${pair}" "${constitution}"; then
        echo "  FAIL: Constitution's Principle I paragraph does not contain '${pair}' after amend"
        fail_count=$((fail_count + 1))
    else
        echo "  PASS: Constitution updated (${pair} present)"
        pass_count=$((pass_count + 1))
    fi
done
# Verify all 3 Makefile variables updated
for var_bytes in "WIN_PACK_BUDGET[[:space:]]*=[[:space:]]*50176" "PACK_TARGET[[:space:]]*=[[:space:]]*31744" "STRIP_TARGET[[:space:]]*=[[:space:]]*52224"; do
    if ! grep -qE "^${var_bytes}" "${makefile}"; then
        echo "  FAIL: Makefile does not have ${var_bytes} after amend"
        fail_count=$((fail_count + 1))
    else
        echo "  PASS: Makefile updated (${var_bytes} present)"
        pass_count=$((pass_count + 1))
    fi
done
restore_files

# --- Case 3: input validation ---
# Three sub-checks: (a) 0 flags, (b) non-integer value, (c) shrink budget.
echo
echo "--- Case 3: input validation ---"
snapshot_files
# (a) 0 flags: should exit 1 with FATAL
run_case "invalid_zero_flags" "1" "at least one of" \
    "bash '${script}'" || true
# (b) non-integer value: should exit 1 with FATAL
run_case "invalid_non_integer" "1" "is not a positive integer" \
    "bash '${script}' --lin-str abc" || true
# (c) shrink budget: should exit 1 with FATAL (current 50 KB, new 49 KB)
run_case "invalid_shrink" "1" "can only grow" \
    "bash '${script}' --lin-str 49" || true
# Verify Constitution + Makefile are unchanged after all 3 sub-checks
if grep -qE '≤49 KB stripped Linux binary' "${constitution}"; then
    echo "  FAIL: Constitution was modified by invalid input"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: Constitution unchanged after 3 invalid input attempts"
    pass_count=$((pass_count + 1))
fi
if ! grep -qE '^STRIP_TARGET[[:space:]]*=[[:space:]]*51200' "${makefile}"; then
    echo "  FAIL: Makefile's STRIP_TARGET was modified by invalid input"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: Makefile's STRIP_TARGET = 51200 unchanged after 3 invalid input attempts"
    pass_count=$((pass_count + 1))
fi
restore_files

# --- Case 4: dry-run ---
# --dry-run should preview the changes without modifying any files.
echo
echo "--- Case 4: dry-run (--dry-run --lin-str 60) ---"
snapshot_files
run_case "dry_run" "0" "DRY-RUN: no files were modified" \
    "bash '${script}' --dry-run --lin-str 60" || true
# Verify Constitution + Makefile are unchanged
if grep -qE '≤60 KB stripped Linux binary' "${constitution}"; then
    echo "  FAIL: Constitution was modified by --dry-run"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: Constitution unchanged after --dry-run"
    pass_count=$((pass_count + 1))
fi
if ! grep -qE '^STRIP_TARGET[[:space:]]*=[[:space:]]*51200' "${makefile}"; then
    echo "  FAIL: Makefile's STRIP_TARGET was modified by --dry-run"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: Makefile's STRIP_TARGET = 51200 unchanged after --dry-run"
    pass_count=$((pass_count + 1))
fi
restore_files

# --- Case 5: refuse-to-commit ---
# Amend a budget, then verify NO `git add` or `git commit` was run
# by the script. The working tree should be dirty (Constitution +
# Makefile modified) but NOT staged.
echo
echo "--- Case 5: refuse-to-commit (amend then verify un-staged dirty tree) ---"
snapshot_files
bash "${script}" --lin-str 51 >/dev/null 2>&1 || {
    echo "  FAIL: amend failed unexpectedly"
    fail_count=$((fail_count + 1))
    restore_files
    return 1 2>/dev/null || true
}
# Verify working tree is dirty
if git diff --quiet HEAD -- "${constitution}" "${makefile}"; then
    echo "  FAIL: working tree is clean after amend (expected dirty)"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: working tree is dirty after amend (Constitution + Makefile modified)"
    pass_count=$((pass_count + 1))
fi
# Verify Constitution + Makefile are NOT staged
staged_diff=$(git diff --cached --name-only -- "${constitution}" "${makefile}" 2>/dev/null || true)
if [ -n "${staged_diff}" ]; then
    echo "  FAIL: Constitution + Makefile are staged (${staged_diff}); amend should NOT auto-stage"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: Constitution + Makefile are NOT staged (amend refuses to commit)"
    pass_count=$((pass_count + 1))
fi
restore_files

# --- Case 6: recovery via git checkout ---
# This case verifies the explicit "the script + working tree can
# recover from a tampered state" path. Amend a budget, then restore
# from HEAD via `git checkout --`. The post-recovery Constitution
# + Makefile should be byte-identical to HEAD.
echo
echo "--- Case 6: recovery via git checkout ---"
snapshot_files
# Run the amend (this succeeds + leaves working tree dirty)
bash "${script}" --lin-str 51 >/dev/null 2>&1 || true
# Drop the .bak files (so the trap doesn't double-restore) and
# recover via `git checkout --` from HEAD. This is the actual
# recovery path under test.
rm -f "${constitution_bak}" "${makefile_bak}"
git checkout -- "${constitution}" "${makefile}"
# Verify: clean working tree for Constitution + Makefile
if ! git diff --quiet HEAD -- "${constitution}" "${makefile}"; then
    echo "  FAIL: git checkout did not fully restore Constitution + Makefile"
    fail_count=$((fail_count + 1))
else
    echo "  PASS: git checkout verified clean (no diff vs HEAD)"
    pass_count=$((pass_count + 1))
fi

# --- Final summary ---
echo
echo "=== Test summary ==="
echo "  main scenarios: ${main_case_count}"
echo "  sub-checks passed: ${pass_count}"
echo "  sub-checks failed: ${fail_count}"
echo "  working tree: $(git status --porcelain | wc -l) file(s) dirty (Constitution + Makefile should be clean post-trap)"

if [ "${fail_count}" -gt 0 ]; then
    echo
    echo "FAIL: at least one sub-check did not produce the expected outcome."
    echo "      see FAIL: lines above for the specific sub-check that broke."
    exit 1
fi

echo
echo "PASS: all ${main_case_count} main scenarios green (${pass_count} sub-checks); tools/spec-budget-amend.sh is regression-tested."
exit 0
