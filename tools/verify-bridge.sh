#!/bin/bash
#
# tools/verify-bridge.sh
#
# One-command local dev check for the Constitution<->Makefile bridge.
# Sequentially runs the 3 spec<->build verification artifacts in order,
# exiting on first failure with a clear per-step status. Bundles:
#
#   1. tools/spec-budget-check.sh
#      Read-only bridge: asserts Makefile byte value = Constitution
#      KB value * 1024 for all 3 budgets (Windows UPX, Linux UPX,
#      Linux stripped). ~1 s of in-memory regex extraction.
#      Exit 0 = pass, exit 1 = drift detected, exit 2 = malformed input.
#
#   2. tests/test_spec_budget_check.sh
#      5-scenario / 9-sub-check regression for the bridge (tamper
#      Constitution, tamper Makefile, malformed constant, recovery
#      via git checkout). ~5 s of in-memory file tamper + restore.
#      Exit 0 = pass, exit 1 = sub-check failed.
#
#   3. tests/test_spec_budget_amend.sh
#      6-scenario / 21-sub-check regression for the amend helper
#      (happy-path amend 1, atomic amend all 3, input validation,
#      dry-run, refuse-to-commit, recovery via git checkout). ~5-10 s
#      of in-memory file tamper + restore. Has a scoped dirty-tree
#      guard that aborts with FATAL if Constitution + Makefile have
#      un-committed changes (so the test's `git checkout --` recovery
#      path can't clobber them silently).
#      Exit 0 = pass, exit 1 = sub-check failed, exit 1 (from guard)
#      = dirty Constitution or Makefile.
#
# This is the dev-box equivalent of running the 3 dedicated ci.yml
# steps (`Bridge regression test (Constitution<->Makefile)` + `Amend
# helper regression test (Constitution<->Makefile)` + the inline
# `Binary size budget gate` step's pre-flight) in one command.
#
# Usage:
#   bash tools/verify-bridge.sh         # from repo root
#   make verify                         # via the Makefile target
#
# Exit codes:
#   0  all 3 steps green
#   1  at least one step failed (the failing step's exit code is
#      echoed to stderr for diagnosis)
#   2  FATAL setup failure (script not found, not in a git working tree)
#
# Why this is a wrapper and not a Makefile-only target: the test
# scripts use `/tmp/.bak.$$` files for their cleanup traps, and the
# amend test's scoped dirty-tree guard needs a subshell context that
# matches the ci.yml step's environment. Re-implementing that logic
# in a Makefile recipe would be ~50 lines of bash vs ~30 lines in a
# dedicated script with a clear scope.
#
# Post-test cleanliness: the 3 scripts have their own cleanup traps
# that restore any tampered files via /tmp .bak copies, so this
# wrapper leaves the working tree in its pre-invocation state on
# both success and failure paths.

set -e

# --- 1. Resolve repo root + the 3 scripts ---
repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "FATAL: not inside a git working tree" >&2
    exit 2
}
cd "${repo_root}"

bridge="${repo_root}/tools/spec-budget-check.sh"
bridge_test="${repo_root}/tests/test_spec_budget_check.sh"
amend_test="${repo_root}/tests/test_spec_budget_amend.sh"

for f in "${bridge}" "${bridge_test}" "${amend_test}"; do
    [ -f "${f}" ] || { echo "FATAL: missing required file: ${f}" >&2; exit 2; }
done

# No chmod: the test scripts are committed mode 100755. A chmod here
# would flip tracked mode bits on Linux and make `git describe
# --dirty` mark every subsequent build dirty (the retired #129/#130
# chmod trap). They are invoked via `bash` below anyway.

# --- 2. Helper: run a single step with a clear header ---
# Args:
#   $1 step number (1-3)
#   $2 step description (short label)
#   $3 command to run
# Returns: the command's exit code
run_step() {
    local step_num="$1" step_desc="$2" cmd="$3"
    echo
    echo "==================================================================="
    echo "  Step ${step_num}/3: ${step_desc}"
    echo "==================================================================="
    echo "+ ${cmd}"
    echo
    local rc=0
    set +e
    eval "${cmd}"
    rc=$?
    set -e
    echo
    if [ "${rc}" -eq 0 ]; then
        echo "  [PASS] Step ${step_num}/3: ${step_desc}"
    else
        echo "  [FAIL] Step ${step_num}/3: ${step_desc} (exit code ${rc})" >&2
    fi
    return "${rc}"
}

# --- 3. Run the 3 steps in order ---
overall_rc=0

if ! run_step 1 "Read-only bridge (Constitution<->Makefile KB alignment)" \
        "bash '${bridge}'"; then
    overall_rc=1
    echo
    echo "==================================================================="
    echo "  FAIL: Step 1/3 failed. Aborting."
    echo "==================================================================="
    echo "  The 2 regression tests in steps 2-3 were NOT run because"
    echo "  the read-only bridge check failed first. Fix the Constitution"
    echo "  <-> Makefile KB alignment (per the FATAL hint above), then"
    echo "  re-run 'make verify'."
    echo
    echo "  Common fix: amend BOTH .specify/memory/constitution.md"
    echo "  Principle I paragraph AND the corresponding Makefile"
    echo "  size-budget variable (STRIP_TARGET / PACK_TARGET /"
    echo "  WIN_PACK_BUDGET) to the same value, OR use"
    echo "  'tools/spec-budget-amend.sh' to do it atomically."
    echo "==================================================================="
    exit "${overall_rc}"
fi

if ! run_step 2 "Bridge regression test (5 scenarios / 9 sub-checks)" \
        "bash '${bridge_test}'"; then
    overall_rc=1
    echo
    echo "==================================================================="
    echo "  FAIL: Step 2/3 failed. Aborting."
    echo "==================================================================="
    echo "  The amend regression test in step 3 was NOT run because"
    echo "  the bridge regression test failed first. See the FAIL:"
    echo "  line(s) above for the specific sub-check that broke, then"
    echo "  re-run 'make verify'."
    echo
    echo "  Common cause: the bridge script's regex extraction or"
    echo "  arithmetic logic drifted. Check tools/spec-budget-check.sh"
    echo "  against the Constitution Principle I paragraph format."
    echo "==================================================================="
    exit "${overall_rc}"
fi

if ! run_step 3 "Amend helper regression test (6 scenarios / 21 sub-checks)" \
        "bash '${amend_test}'"; then
    overall_rc=1
    echo
    echo "==================================================================="
    echo "  FAIL: Step 3/3 failed. Aborting."
    echo "==================================================================="
    echo "  See the FAIL: line(s) above for the specific sub-check that"
    echo "  broke. Common causes:"
    echo "    - dirty Constitution or Makefile (the amend test's scoped"
    echo "      dirty-tree guard aborts with FATAL before any sub-check"
    echo "      runs -- commit or stash your changes first)"
    echo "    - amend script's CLI parsing or KB validation drifted"
    echo "    - sed-based Constitution or Makefile update logic drifted"
    echo "    - refuse-to-commit or rollback trap behavior changed"
    echo "  Re-run 'make verify' after fixing the root cause."
    echo "==================================================================="
    exit "${overall_rc}"
fi

# --- 4. Final summary ---
echo
echo "==================================================================="
echo "  PASS: all 3 bridge verification steps green"
echo "  - Step 1/3: read-only bridge check"
echo "  - Step 2/3: bridge regression test (5 scenarios / 9 sub-checks)"
echo "  - Step 3/3: amend helper regression test (6 scenarios / 21 sub-checks)"
echo "==================================================================="
echo "  The Constitution<->Makefile triple-budget alignment is verified"
echo "  + both regression suites are green. Safe to open a PR."
echo "==================================================================="

exit 0
