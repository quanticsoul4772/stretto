#!/bin/bash
#
# tools/spec-budget-amend.sh
#
# Helper for future Constitution<->Makefile size-budget amendments
# (v1.3.0+). Bumps 1-3 of the 3 size budgets (Windows UPX / Linux
# UPX / Linux stripped) in BOTH .specify/memory/constitution.md
# Principle I paragraph AND the Makefile size-budget variables in
# lockstep, then prints a git diff for review. REFUSES to commit --
# the developer reviews + commits manually with a v1.X.0 amendment
# rationale in the commit message + PR body.
#
# Why this helper exists: post-#117, the size-budget triple lives
# in 2 separate sources (Constitution Principle I paragraph + 3
# Makefile variables). Forgetting to bump one of them in a v1.X.0
# amendment is what caused the post-#117 spec<->build drift
# cascade (PRs #118-#124). This helper makes a v1.X.0 amendment a
# 1-invocation edit: supply the new KB values, get a diff, commit
# manually. The bridge (tools/spec-budget-check.sh + ci.yml step
# + regression test) is the safety net for any drift that slips
# past this helper.
#
# Usage:
#   tools/spec-budget-amend.sh --win 49                        # bump Windows UPX to 49 KB
#   tools/spec-budget-amend.sh --lin-upx 32                    # bump Linux UPX to 32 KB
#   tools/spec-budget-amend.sh --lin-str 60                    # bump Linux stripped to 60 KB
#   tools/spec-budget-amend.sh --win 49 --lin-upx 32 --lin-str 60  # bump all 3 atomically
#   tools/spec-budget-amend.sh --dry-run --lin-str 60          # preview only; no file changes
#
# Budget-grow policy: per the post-#117 amendment policy (cf PR
# #118 rationale), budgets can only GROW, not shrink. To shrink a
# budget (e.g. if a major refactor reduces binary size), do it
# manually with explicit Constitution + Makefile edits + an
# accompanying PR-body rationale, not via this helper. The helper
# rejects shrink attempts with FATAL.
#
# Exit codes:
#   0  amend complete (changes left in working tree, NOT committed)
#   1  invalid input or amend failed (working tree restored to pre-amend state)
#   2  FATAL setup failure (script bug / missing files)
#
# Rollback safety: Constitution + Makefile are backed up to /tmp
# before any edit; if the script aborts with non-zero exit, the
# trap restores from the backups. On success, the .bak files are
# explicitly removed so the trap is a no-op on subsequent exits.

set -e

# --- 1. Parse CLI args ---
dry_run=0
win_kb=""
lin_upx_kb=""
lin_str_kb=""

usage() {
    cat <<EOF
Usage: tools/spec-budget-amend.sh [OPTIONS]

Options:
  --win KB       Set Windows UPX-packed budget to KB (positive integer, can only grow)
  --lin-upx KB   Set Linux UPX-packed budget to KB
  --lin-str KB   Set Linux stripped budget to KB
  --dry-run      Preview the changes without modifying any files
  --help, -h     Show this help

At least one --{win,lin-upx,lin-str} flag is required.

This script does NOT commit. After running, review the diff and run:
  git add .specify/memory/constitution.md Makefile
  git commit -m "v1.X.0: bump <budget> from N to M KB per Principle I amendment"

If the Makefile rationale paragraph (comment block above the 3
budget variables) needs updating to reflect the new measurements,
edit it in the SAME commit; this script leaves the rationale
unchanged since it's editorial content.

EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --win)     win_kb="$2";     shift 2 ;;
        --lin-upx) lin_upx_kb="$2"; shift 2 ;;
        --lin-str) lin_str_kb="$2"; shift 2 ;;
        --dry-run) dry_run=1;       shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "FATAL: unknown flag: $1" >&2; usage >&2; exit 1 ;;
    esac
done

# Validate: at least 1 flag supplied
if [ -z "${win_kb}" ] && [ -z "${lin_upx_kb}" ] && [ -z "${lin_str_kb}" ]; then
    echo "FATAL: at least one of --win / --lin-upx / --lin-str is required" >&2
    usage >&2
    exit 1
fi

# --- 2. Validate KB values (positive integer >= 1) ---
validate_kb() {
    local name="$1" kb="$2"
    if [ -z "${kb}" ]; then return 0; fi  # not supplied; skip
    case "${kb}" in
        ''|*[!0-9]*) echo "FATAL: ${name} KB value '${kb}' is not a positive integer" >&2; exit 1 ;;
    esac
    if [ "${kb}" -lt 1 ]; then
        echo "FATAL: ${name} KB value ${kb} is less than 1 KB" >&2
        exit 1
    fi
}
validate_kb "WIN"     "${win_kb}"
validate_kb "LIN_UPX" "${lin_upx_kb}"
validate_kb "LIN_STR" "${lin_str_kb}"

# --- 3. Resolve paths + backup files ---
repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "FATAL: not in a git working tree" >&2; exit 2; }
cd "${repo_root}"

constitution="${repo_root}/.specify/memory/constitution.md"
makefile="${repo_root}/Makefile"
bridge="${repo_root}/tools/spec-budget-check.sh"

for f in "${constitution}" "${makefile}" "${bridge}"; do
    [ -f "${f}" ] || { echo "FATAL: missing required file: ${f}" >&2; exit 2; }
done

constitution_bak="/tmp/.spec_budget_amend_constitution.bak.$$"
makefile_bak="/tmp/.spec_budget_amend_makefile.bak.$$"

cp "${constitution}" "${constitution_bak}"
cp "${makefile}" "${makefile_bak}"

# Cleanup trap: on any non-zero exit (signal or error), restore from
# backups so the working tree is consistent with HEAD. On success
# (exit 0), the post-amend section explicitly removes the .bak files
# so the trap is a no-op.
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

# --- 4. Extract current values ---
# Constitution: get the current KB value before a given keyword
get_current_kb() {
    local keyword="$1"
    local kb
    kb=$(grep -m1 -oE "≤[0-9]+ KB ${keyword}" "${constitution}" | head -1 | grep -oE '≤[0-9]+ KB' | head -1 | tr -d '≤ KB')
    if [ -z "${kb}" ]; then
        echo "FATAL: could not extract current Constitution KB for keyword '${keyword}'" >&2
        echo "       (Constitution Principle I paragraph may be missing or malformed)" >&2
        exit 2
    fi
    echo "${kb}"
}

# Makefile: get the current byte value for a given variable
get_current_bytes() {
    local var="$1"
    local bytes
    bytes=$(grep -E "^${var}[[:space:]]*=" "${makefile}" | head -1 | sed -E 's/^[^=]+=[[:space:]]*//' | tr -d ' \t')
    if [ -z "${bytes}" ]; then
        echo "FATAL: could not extract current Makefile value for ${var}" >&2
        exit 2
    fi
    case "${bytes}" in
        *[!0-9]*) echo "FATAL: Makefile ${var} = ${bytes} is not a positive integer" >&2; exit 2 ;;
    esac
    echo "${bytes}"
}

cur_win_kb=$(get_current_kb     "UPX-packed Windows")
cur_lin_upx_kb=$(get_current_kb "UPX-packed Linux binary")
cur_lin_str_kb=$(get_current_kb "stripped Linux binary")
cur_win_bytes=$(get_current_bytes     "WIN_PACK_BUDGET")
cur_lin_upx_bytes=$(get_current_bytes "PACK_TARGET")
cur_lin_str_bytes=$(get_current_bytes "STRIP_TARGET")

# --- 5. Check budget-grow policy ---
# Per post-#117 amendment policy (cf PR #118 rationale), budgets
# can only grow, not shrink. This is a defensive check; if a
# future PR legitimately needs to shrink (e.g. a major refactor
# reduces binary size), the dev can bypass this check by editing
# the files manually with explicit Constitution + Makefile edits.
check_growth() {
    local name="$1" new_kb="$2" cur_kb="$3" cur_bytes="$4"
    if [ -n "${new_kb}" ] && [ "${new_kb}" -le "${cur_kb}" ]; then
        echo "FATAL: ${name} budget can only grow per post-#117 amendment policy" >&2
        echo "       current: ${cur_kb} KB (${cur_bytes} B)" >&2
        echo "       new:     ${new_kb} KB" >&2
        echo "       To shrink a budget, do it manually with explicit edits + PR-body rationale" >&2
        exit 1
    fi
}
check_growth "WIN"     "${win_kb}"     "${cur_win_kb}"     "${cur_win_bytes}"
check_growth "LIN_UPX" "${lin_upx_kb}" "${cur_lin_upx_kb}" "${cur_lin_upx_bytes}"
check_growth "LIN_STR" "${lin_str_kb}" "${cur_lin_str_kb}" "${cur_lin_str_bytes}"

# --- 6. Print preview ---
echo "=== Spec<>Build size-budget amendment ==="
echo "  Current:"
printf '    %-10s  constitution=%3s KB  makefile=%6s B  (variable=%s)\n' \
    "WIN"     "${cur_win_kb}"     "${cur_win_bytes}"     "WIN_PACK_BUDGET"
printf '    %-10s  constitution=%3s KB  makefile=%6s B  (variable=%s)\n' \
    "LIN_UPX" "${cur_lin_upx_kb}" "${cur_lin_upx_bytes}" "PACK_TARGET"
printf '    %-10s  constitution=%3s KB  makefile=%6s B  (variable=%s)\n' \
    "LIN_STR" "${cur_lin_str_kb}" "${cur_lin_str_bytes}" "STRIP_TARGET"
echo "  Amendments:"
[ -n "${win_kb}"     ] && printf '    %-10s  %3s KB -> %3s KB  (%6s B -> %6s B)\n' \
    "WIN"     "${cur_win_kb}"     "${win_kb}"     "${cur_win_bytes}"     "$((win_kb * 1024))"
[ -n "${lin_upx_kb}" ] && printf '    %-10s  %3s KB -> %3s KB  (%6s B -> %6s B)\n' \
    "LIN_UPX" "${cur_lin_upx_kb}" "${lin_upx_kb}" "${cur_lin_upx_bytes}" "$((lin_upx_kb * 1024))"
[ -n "${lin_str_kb}" ] && printf '    %-10s  %3s KB -> %3s KB  (%6s B -> %6s B)\n' \
    "LIN_STR" "${cur_lin_str_kb}" "${lin_str_kb}" "${cur_lin_str_bytes}" "$((lin_str_kb * 1024))"

if [ "${dry_run}" -eq 1 ]; then
    echo
    echo "DRY-RUN: no files were modified. To apply, re-run without --dry-run."
    rm -f "${constitution_bak}" "${makefile_bak}"
    trap - EXIT INT TERM
    exit 0
fi

# --- 7. Apply amendments ---
# Constitution: replace the ≤XX KB literal before the keyword. The
# targeted-regex approach (vs full-paragraph rewrite) preserves the
# surrounding text + cross-references + measurement mentions.
apply_const() {
    local new_kb="$1" keyword="$2"
    if [ -n "${new_kb}" ]; then
        sed -i -E "s/≤[0-9]+ KB ${keyword}/≤${new_kb} KB ${keyword}/" "${constitution}"
    fi
}
apply_const "${win_kb}"     "UPX-packed Windows"
apply_const "${lin_upx_kb}" "UPX-packed Linux binary"
apply_const "${lin_str_kb}" "stripped Linux binary"

# Makefile: replace the byte value, preserving the variable name
# + spacing (column-aligned at '=' via Makefile column-17 convention).
apply_make() {
    local var="$1" new_kb="$2" cur_bytes="$3"
    if [ -n "${new_kb}" ]; then
        local new_bytes=$((new_kb * 1024))
        sed -i -E "s/^(${var}[[:space:]]*=[[:space:]]*)${cur_bytes}/\1${new_bytes}/" "${makefile}"
    fi
}
apply_make "WIN_PACK_BUDGET" "${win_kb}"     "${cur_win_bytes}"
apply_make "PACK_TARGET"     "${lin_upx_kb}" "${cur_lin_upx_bytes}"
apply_make "STRIP_TARGET"    "${lin_str_kb}" "${cur_lin_str_bytes}"

# --- 8. Verify: run the bridge script to confirm new values align ---
echo
echo "=== Verifying via tools/spec-budget-check.sh ==="
if ! bash "${bridge}"; then
    echo "FATAL: bridge script failed post-amendment; rolling back via cleanup trap" >&2
    exit 1
fi

# --- 9. Print diff + refuse-to-commit message ---
echo
echo "=== git diff (Constitution + Makefile) ==="
git diff --no-color -- "${constitution}" "${makefile}" || true
echo
echo "=== Post-amendment: refuse-to-commit notice ==="
echo "This script does NOT commit. Review the diff above, then:"
echo "  git add .specify/memory/constitution.md Makefile"
echo "  git commit -m 'v1.X.0: bump <budget> from N to M KB per Principle I amendment'"
echo
echo "If the Makefile rationale paragraph (comment block above the 3 budget"
echo "variables) needs updating to reflect the new measurements, edit it"
echo "in the SAME commit; this script leaves the rationale unchanged since"
echo "it's editorial content (manually curated post-amendment per the"
echo "Principle I paragraph's own precedent: PR #116 / #118 / #121 each"
echo "refreshed the rationale in their amend-PR, not via tooling)."
echo
echo "The Constitution footer (Last Amended date + Version line) should"
echo "also be bumped in the SAME commit per Governance: 'Amendments are"
echo "PRs that modify this file and bump the version below.'"

# --- 10. Cleanup: remove .bak files so the trap doesn't restore ---
rm -f "${constitution_bak}" "${makefile_bak}"
trap - EXIT INT TERM

exit 0
