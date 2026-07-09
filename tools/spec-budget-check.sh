#!/bin/bash
#
# tools/spec-budget-check.sh
#
# Asserts that Makefile size-budget variables (WIN_PACK_BUDGET /
# PACK_TARGET / STRIP_TARGET) equal the Constitution Principle I
# triple enumerated in .specify/memory/constitution.md:
#   - ≤48 KB UPX-packed Windows .exe       (Principle I; v1.1.0 baseline)
#   - ≤30 KB UPX-packed Linux binary        (Principle I; v1.2.0 amendment)
#   - ≤50 KB stripped Linux binary          (Principle I; v1.1.0 amendment)
#
# This is the spec<>build bridge that runs BEFORE the inline
# `Binary size budget gate` step in .github/workflows/ci.yml. If a
# future PR amends .specify/memory/constitution.md Principle I
# paragraph without bumping the corresponding Makefile variable
# (or vice versa), this script fails CI immediately with a
# remediation hint, before the same drift would have been caught
# via the post-#117 binary-sizes.txt artifact cascade PRs
# #118-#124 had to clean up.
#
# Exit codes:
#   0  all 3 budgets match (PASS)
#   1  at least one budget drifts (FAIL; per-budget remediation hint emitted)
#   2  malformed inputs (missing files, regex no-match, non-integer Makefile values)
#
# Usage:
#   bash tools/spec-budget-check.sh         # from repo root
#   bash tools/spec-budget-check.sh         # from any cwd; resolves repo via git
#
# Unit-conversion convention: Constitution budgets in KB (binary),
# Makefile budgets in bytes. 1 KB = 1024 B. Both 48 KB -> 49152 B
# and 50 KB -> 51200 B and 30 KB -> 30720 B resolved cleanly to
# the existing Makefile variable values as of post-#118+#121, so
# the 1024 factor is the principle not coincidence.

set -e

# --- 1. Resolve repo root + required file paths ---
repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "FATAL: not inside a git working tree" >&2
    exit 2
}
cd "${repo_root}"

constitution="${repo_root}/.specify/memory/constitution.md"
makefile="${repo_root}/Makefile"

for f in "${constitution}" "${makefile}"; do
    [ -f "${f}" ] || { echo "FATAL: missing required file: ${f}" >&2; exit 2; }
done

# --- 2. Extract 3 budgets from Constitution Principle I paragraph ---
#
# The Principle I paragraph enumerates the 3 budgets in a fixed order:
#   ≤48 KB UPX-packed Windows .exe (...) ,
#   ≤30 KB UPX-packed Linux binary (...) ,
#   ≤50 KB stripped Linux binary (...) .
# Each budget is matched by a unique keyword regex so the parser
# doesn't pull out the parenthetical measurement mentions (e.g.
# "current 38 KB post-#117", "25 460 B / ~25 KB"). Using keyword
# regex (rather than positional) means an added 4th budget later
# (e.g. for a 5th platform) can be matched with the same pattern
# style without breaking the 3-way assertion logic.
#
# Pattern keys used to identify each budget:
#   WIN      = '≤\d+ KB UPX-packed Windows .exe'            (1st match = Windows budget)
#   LIN_UPX  = '≤\d+ KB UPX-packed Linux binary'           (1st match = Linux UPX budget)
#   LIN_STR  = '≤\d+ KB stripped Linux binary'             (1st match = Linux stripped budget)
#
# Each Constitution line mentions the KB budget exactly once
# (the parenthetical measurement mentions use '~XX KB' or 'XX B'
# forms, neither of which match the authoritative '≤X KB' pattern;
# sed-extraction of the '≤X KB' literal after keyword-grep is
# sufficient to disambiguate). The '≤' character is U+2264; the
# script relies on grep matching UTF-8 literal.

extract_kb() {
    # $1 = regex pattern (POSIX extended, including the '≤' literal).
    #
    # Important: this function uses `grep -oE` (only-matching) rather
    # than `grep -E` (whole-line). Principle I is a single paragraph
    # with all 3 budgets on one line, so a whole-line match followed by
    # `head -1 | grep '≤X KB' | head -1` would yield the FIRST budget
    # in the line (Windows 48 KB) for ALL 3 calls, because each
    # pattern's whole-line match spans the same line. Using `-oE`
    # restricts output to the matched substring only, so the KB literal
    # that's NEAREST each budget's keyword is uniquely identified.
    local pattern="$1"
    local match kb
    match=$(grep -m1 -oE "${pattern}" "${constitution}") || {
        echo "FATAL: Constitution Principle I budget regex '${pattern}' has no match in ${constitution}" >&2
        echo "       (file may be missing the line; check Principle I paragraph is intact per Governance)" >&2
        exit 2
    }
    kb=$(echo "${match}" | grep -oE '≤[0-9]+ KB' | head -1 | tr -d '≤ KB')
    if [ -z "${kb}" ]; then
        echo "FATAL: could not parse '≤X KB' literal from match: ${match}" >&2
        exit 2
    fi
    # Validate that kb is a positive integer ≥ 1 (defensive; reject 0 KB, negative, etc.)
    case "${kb}" in
        ''|*[!0-9]*|0) echo "FATAL: parsed KB value '${kb}' is not a positive integer ≥ 1 from match: ${match}" >&2; exit 2 ;;
    esac
    echo "${kb}"
}

win_kb=$(extract_kb '≤[0-9]+ KB UPX-packed Windows')
linux_upx_kb=$(extract_kb '≤[0-9]+ KB UPX-packed Linux binary')
linux_strip_kb=$(extract_kb '≤[0-9]+ KB stripped Linux binary')

# KB → bytes (binary KB = 1024 B per Constitution convention)
win_bytes=$((win_kb * 1024))
linux_upx_bytes=$((linux_upx_kb * 1024))
linux_strip_bytes=$((linux_strip_kb * 1024))

# --- 3. Extract byte-budget variables from Makefile ---
#
# Convention: each size-budget variable is assigned at the START
# of a line followed by '=' then a positive-integer byte value.
# The bridge lints that pattern; if the Makefile ever drifts to
# an arithmetic expression (e.g. 'STRIP_TARGET = 50 * 1024') the
# awk + case-fallthrough numeric-validate will fail loudly here.
#
# WIN_PACK_BUDGET (introduced by PR #122 / 025-spec-to-make-bridge)
# lives NEXT TO STRIP_TARGET / PACK_TARGET in the Makefile size
# target comment block; the bridge reads all 3 from the same lines.

extract_make_var() {
    # $1 = variable name (must be at line start per Makefile convention)
    local var="$1"
    local val
    val=$(grep -E "^${var}[[:space:]]*=" "${makefile}" | head -1 | sed -E 's/^[^=]*=[[:space:]]*//' | tr -d ' \t')
    if [ -z "${val}" ]; then
        echo "FATAL: Makefile does not define ${var} = ... (per PR #122 the variable should live next to STRIP_TARGET / PACK_TARGET)" >&2
        exit 2
    fi
    case "${val}" in
        *[!0-9]*) echo "FATAL: Makefile ${var} = ${val} is not a positive integer (must be a byte count expressed as a literal)" >&2; exit 2 ;;
    esac
    echo "${val}"
}

# Intentionally align with Makefile variable names so failure-mode
# diagnostics can COPY the script's Makefile-side extract verbatim
# into a Makefile amendment.
make_win_bytes=$(extract_make_var 'WIN_PACK_BUDGET')
make_strip_bytes=$(extract_make_var 'STRIP_TARGET')
make_pack_bytes=$(extract_make_var 'PACK_TARGET')

# --- 4. Compare and emit verdict ---
#
# Output format mirrors the existing 'Binary size budget gate' step's
# verbose diagnostic style so a developer scanning CI logs recognizes
# the pattern. The 'MATCH' / 'MISMATCH' token at line-end is greppable.

fail=0
printf '\n=== Spec<>Build size-budget bridge (Constitution Principle I vs Makefile) ===\n'
printf '  convention: Constitution budgets in KB (binary 1024), Makefile budgets in bytes\n'
printf '  convention: 1 KB = 1024 B (matches current 48 KB -> 49152 B / 50 KB -> 51200 B / 30 KB -> 30720 B precedent)\n\n'

check_budget() {
    # $1 label; $2 const_kb; $3 const_bytes; $4 make_var; $5 make_bytes; $6 pattern
    local label="$1" const_kb="$2" const_bytes="$3" make_var="$4" make_bytes="$5" pattern="$6"
    local status token
    if [ "${const_bytes}" = "${make_bytes}" ]; then
        token="MATCH"
        status=0
    else
        token="MISMATCH"
        status=1
        # Emit a remediation hint specifically for this budget.
        printf '    Hint: %s drift detected. To resolve, amend EITHER:\n' "${label}"
        printf '             (a) Makefile: set %s = %s   (the resolved byte value the Constitution implies for %s KB * 1024)\n' "${make_var}" "${const_bytes}" "${const_kb}"
        printf '             (b) Constitution Principle I paragraph: change ≈≤%s KB≤ to the KB value implied by %s = %s B\n' "${const_kb}" "${make_var}" "${make_bytes}"
        printf '          Future PRs should keep BOTH in lockstep per the Governance clause ("Amendments are PRs that modify this file and bump the version below").\n'
    fi
    printf '  %-10s  constitution=%s B (≤%s KB)  makefile=%s B (%s = %s)  %s\n' \
        "${label}" "${const_bytes}" "${const_kb}" "${make_bytes}" "${make_var}" "${make_bytes}" "${token}"
    if [ "${status}" -gt 0 ]; then
        fail=1
    fi
}

check_budget "WIN"      "${win_kb}"         "${win_bytes}"         "WIN_PACK_BUDGET" "${make_win_bytes}"  "'≤\d+ KB UPX-packed Windows'"
check_budget "LIN_UPX"  "${linux_upx_kb}"   "${linux_upx_bytes}"   "PACK_TARGET"     "${make_pack_bytes}" "'≤\d+ KB UPX-packed Linux binary'"
check_budget "LIN_STR"  "${linux_strip_kb}" "${linux_strip_bytes}" "STRIP_TARGET"    "${make_strip_bytes}" "'≤\d+ KB stripped Linux binary'"

printf '\n'
if [ "${fail}" -gt 0 ]; then
    printf '=== FAIL: Spec<>Build size-budget bridge drift detected ===\n'
    printf 'A future Constitution Principle I amendment that updates KB values in .specify/memory/constitution.md\n'
    printf 'must update the corresponding Makefile size-budget variable(s) (or vice versa) in the SAME PR. The\n'
    printf 'inline Binary size budget gate ci.yml step enforces the resulting budgets against binary-sizes.txt;\n'
    printf 'this script enforces the consistency between Constitution and Makefile BEFORE that measurement is\n'
    printf 'taken, so a drift is caught at PR-merge time rather than as a post-mortem cascade (cf PRs #118-#124).\n'
    exit 1
fi

printf '=== PASS: all 3 budgets aligned (Constitution Principle I = Makefile = byte counts; KB * 1024 = bytes match within each row) ===\n'
exit 0
