#!/bin/bash
#
# tools/size-budget-gate.sh [binary-sizes.txt]
#
# The 3-key binary size budget gate, extracted from the inline awk in
# .github/workflows/ci.yml so the release workflow
# (.github/workflows/release.yml) enforces the identical gate on the
# binaries it publishes - previously tag builds were the one set of
# binaries never gated (make pack only WARNS; ci.yml does not run on
# tag pushes).
#
# Reads measurements AND budgets from one artifact: the key=value rows
# of `make size` output (linux_synth_stripped / linux_synth_packed /
# windows_stretto_exe_packed + the budget_* rows echoed from the
# Makefile's STRIP_TARGET / PACK_TARGET / WIN_PACK_BUDGET). The
# Makefile stays the single measurement-side source of truth; the
# Constitution<->Makefile bridge (tools/spec-budget-check.sh) ties it
# to Principle I upstream.
#
# Exit 0 = all present keys within budget; 1 = overrun or missing
# budget keys. "missing" measurement rows (toolchain absent locally)
# are skipped, matching the previous inline behavior.

set -e

file="${1:-binary-sizes.txt}"
if [ ! -f "${file}" ]; then
    echo "FATAL: ${file} not found - run 'make size | tee binary-sizes.txt' first" >&2
    exit 1
fi

awk -F= '
  $1 == "linux_synth_stripped"                { stripped = $2 }
  $1 == "linux_synth_packed"                  { packed = $2 }
  $1 == "windows_stretto_exe_packed"          { win_packed = $2 }
  $1 == "budget_linux_synth_stripped"         { STRIP_BUDGET = $2 }
  $1 == "budget_linux_synth_packed"           { PACK_BUDGET = $2 }
  $1 == "budget_windows_stretto_exe_packed"   { WIN_PACK_BUDGET = $2 }
  END {
    fail = 0

    if (STRIP_BUDGET + 0 <= 0 || PACK_BUDGET + 0 <= 0 || WIN_PACK_BUDGET + 0 <= 0) {
      print "FAIL: budget_* keys missing or non-numeric in binary-sizes.txt -- `make size` must echo STRIP_TARGET / PACK_TARGET / WIN_PACK_BUDGET as budget_* rows"
      exit 1
    }

    if (stripped == "") {
      print "  linux_synth_stripped: MISSING key in binary-sizes.txt -- Constitution v1.1.0 enforcement required"
      fail = 1
    } else if (stripped == "missing") {
      print "  linux_synth_stripped: not built locally (toolchain row missing per Makefile size target) -- skipped"
    } else if (stripped + 0 > STRIP_BUDGET) {
      printf("FAIL: linux_synth_stripped=%s > STRIP_BUDGET=%d -- Constitution v1.1.0 (Principle I Linux stripped cap)\n", stripped, STRIP_BUDGET)
      print "      Amend Principle I in .specify/memory/constitution.md, OR amend STRIP_TARGET in Makefile, OR trim the trigger feature."
      fail = 1
    } else {
      printf("  linux_synth_stripped=%s bytes  ok (<= %d budget, Constitution v1.1.0)\n", stripped, STRIP_BUDGET)
    }

    if (packed == "") {
      print "  linux_synth_packed: MISSING key in binary-sizes.txt -- Constitution v1.2.0 enforcement required"
      fail = 1
    } else if (packed == "missing") {
      print "  linux_synth_packed: not built locally (UPX unavailable) -- skipped"
    } else if (packed + 0 > PACK_BUDGET) {
      printf("FAIL: linux_synth_packed=%s > PACK_BUDGET=%d -- Constitution v1.2.0 (Principle I Linux UPX cap)\n", packed, PACK_BUDGET)
      print "      Amend Principle I in .specify/memory/constitution.md, OR amend PACK_TARGET in Makefile, OR trim the trigger feature."
      fail = 1
    } else {
      printf("  linux_synth_packed=%s bytes  ok (<= %d budget, Constitution v1.2.0)\n", packed, PACK_BUDGET)
    }

    if (win_packed == "") {
      print "  windows_stretto_exe_packed: MISSING key in binary-sizes.txt -- Constitution v1.1.0 enforcement required"
      fail = 1
    } else if (win_packed == "missing") {
      print "  windows_stretto_exe_packed: not built locally (mingw unavailable) -- skipped"
    } else if (win_packed + 0 > WIN_PACK_BUDGET) {
      printf("FAIL: windows_stretto_exe_packed=%s > WIN_PACK_BUDGET=%d -- Constitution v1.1.0 (Principle I Windows UPX cap)\n", win_packed, WIN_PACK_BUDGET)
      print "      Amend Principle I in .specify/memory/constitution.md, OR amend WIN_PACK_BUDGET in Makefile, OR trim the trigger feature."
      fail = 1
    } else {
      printf("  windows_stretto_exe_packed=%s bytes  ok (<= %d budget, Constitution v1.1.0)\n", win_packed, WIN_PACK_BUDGET)
    }

    if (fail > 0) {
      print "Binary size budget gate FAILED. See .specify/memory/constitution.md Principle I for principled defense of any future budget changes."
    } else {
      print "Binary size budget gate PASSED."
    }
    exit fail
  }
' "${file}"
