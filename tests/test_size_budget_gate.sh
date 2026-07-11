#!/bin/bash
# Regression suite for tools/size-budget-gate.sh (066).
#
# Pure fixture tests: synthetic binary-sizes.txt files exercise every
# gate path - PASS, per-key FAIL, "missing" (toolchain absent) skip,
# absent budget_* keys, absent measurement key, missing file, and the
# page-cliff ADVISORY added in 066 - with no build required.
#
# The gate is always invoked with GITHUB_ACTIONS unset: on Actions the
# script emits a `::notice::` workflow command for the advisory, and a
# fixture-driven suite must never inject annotation directives into a
# real CI run's output.

set -e
cd "$(dirname "$0")/.."

GATE=tools/size-budget-gate.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

fail=0

# run <fixture-file>; captures combined output and exit code into
# $out / $rc without tripping set -e.
run() {
    set +e
    out=$(env -u GITHUB_ACTIONS bash "$GATE" "$1" 2>&1)
    rc=$?
    set -e
}

budgets() {
    printf 'budget_linux_synth_stripped=51200\n'
    printf 'budget_linux_synth_packed=30720\n'
    printf 'budget_windows_stretto_exe_packed=49152\n'
}

check() {  # check <case-name> <expected-rc> <must-contain|-> <must-NOT-contain|->
    name=$1; want_rc=$2; want=$3; ban=$4
    ok=1
    [ "$rc" -eq "$want_rc" ] || { echo "  $name: FAIL - exit $rc (expected $want_rc)"; ok=0; }
    if [ "$want" != "-" ] && ! printf '%s' "$out" | grep -q -- "$want"; then
        echo "  $name: FAIL - output missing \"$want\""; ok=0
    fi
    if [ "$ban" != "-" ] && printf '%s' "$out" | grep -q -- "$ban"; then
        echo "  $name: FAIL - output contains banned \"$ban\""; ok=0
    fi
    if [ "$ok" -eq 1 ]; then echo "  $name: ok"; else fail=1; fi
}

echo "=== size-budget-gate fixture suite ==="

# 1. Everything within budget: PASS, exit 0, no advisory.
{ budgets
  printf 'linux_synth_stripped=48512\n'
  printf 'linux_synth_packed=25460\n'
  printf 'windows_stretto_exe_packed=41984\n'
  printf 'linux_synth_page_cliff_headroom=784\n'
} > "$T/ok.txt"
run "$T/ok.txt"
check "all-within-budget" 0 "Binary size budget gate PASSED" "ADVISORY"

# 2. Stripped over budget: FAIL, exit 1.
{ budgets
  printf 'linux_synth_stripped=52744\n'
  printf 'linux_synth_packed=25460\n'
  printf 'windows_stretto_exe_packed=41984\n'
} > "$T/over.txt"
run "$T/over.txt"
check "stripped-over-budget" 1 "FAIL: linux_synth_stripped" -

# 3. "missing" measurement rows (toolchain absent): skipped, exit 0.
{ budgets
  printf 'linux_synth_stripped=48512\n'
  printf 'linux_synth_packed=missing\n'
  printf 'windows_stretto_exe_packed=missing\n'
} > "$T/skip.txt"
run "$T/skip.txt"
check "missing-rows-skipped" 0 "skipped" -

# 4. budget_* keys absent entirely: FAIL (not FATAL - that token is
#    the missing-file path), exit 1.
{ printf 'linux_synth_stripped=48512\n'
  printf 'linux_synth_packed=25460\n'
  printf 'windows_stretto_exe_packed=41984\n'
} > "$T/nobudget.txt"
run "$T/nobudget.txt"
check "absent-budget-keys" 1 "FAIL: budget_\\*" -

# 5. Missing file: FATAL, exit 1.
run "$T/does-not-exist.txt"
check "missing-file" 1 "FATAL" -

# 6. Measurement key absent entirely (not "missing"): MISSING key, exit 1.
{ budgets
  printf 'linux_synth_packed=25460\n'
  printf 'windows_stretto_exe_packed=41984\n'
} > "$T/nokey.txt"
run "$T/nokey.txt"
check "absent-measurement-key" 1 "MISSING key" -

# 7. Low headroom: ADVISORY printed, gate still exits 0.
{ budgets
  printf 'linux_synth_stripped=48512\n'
  printf 'linux_synth_packed=25460\n'
  printf 'windows_stretto_exe_packed=41984\n'
  printf 'linux_synth_page_cliff_headroom=100\n'
} > "$T/lowroom.txt"
run "$T/lowroom.txt"
check "low-headroom-advisory" 0 "ADVISORY" -
# 7b. Exactly page-aligned: the zero-case wording, still exit 0.
sed 's/headroom=100/headroom=0/' "$T/lowroom.txt" > "$T/zeroroom.txt"
run "$T/zeroroom.txt"
check "zero-headroom-advisory" 0 "the next byte of code" -
# 7c. GITHUB_ACTIONS unset must suppress the workflow command even
#     when the advisory fires (the whole point of the env -u).
run "$T/lowroom.txt"
check "no-workflow-command" 0 - "::notice"

# 8. Comfortable / missing / garbage headroom: silent, exit 0.
for v in 4000 missing abc; do
    sed "s/headroom=100/headroom=$v/" "$T/lowroom.txt" > "$T/quiet.txt"
    run "$T/quiet.txt"
    check "headroom-$v-silent" 0 - "ADVISORY"
done

echo
if [ "$fail" -ne 0 ]; then
    echo "FAIL: size-budget-gate fixture suite"
    exit 1
fi
echo "PASS: size-budget-gate fixture suite"
