#!/usr/bin/env python3
"""Print kcov per-file coverage in gcov's output shape.

Why this exists: gcov cannot instrument Zig. `zig build-obj` rejects
-fprofile-arcs, -ftest-coverage, -fprofile-instr-generate and
-fcoverage-mapping alike, so a module ported to Zig has no path to a
gcov percentage. kcov needs no instrumentation -- it reads DWARF -- but
it emits JSON, and ci.yml's coverage gate parses `make coverage` output
by FIELD POSITION:

    grep -E "^${f}:" coverage.log | sort -k4 -n | tail -1 | awk '{print $2}'

  field 1  <file>:
  field 2  NN.NN%          <- the percentage the gate compares
  field 3  of
  field 4  N               <- line count, used to break duplicate ties

So the row must read exactly:

    density.zig: 100.00% of 14

Emitting anything else silently fails the gate's `[ -z "$pct" ]` branch
rather than the threshold comparison, which reads as "no coverage data"
and is much harder to diagnose than a low number.

Usage:
    kcov-report.py <kcov-merged/coverage.json> [module.zig ...]

With module names given, only those files are printed and a missing one
is an error -- that is what makes a silently-dropped module fail the
build loudly instead of vanishing from the report.
"""
import json
import os
import sys


def main(argv):
    if len(argv) < 2:
        print("usage: kcov-report.py <coverage.json> [module.zig ...]",
              file=sys.stderr)
        return 2

    path = argv[1]
    wanted = set(argv[2:])

    try:
        with open(path) as fh:
            data = json.load(fh)
    except FileNotFoundError:
        print(f"kcov-report: no such file: {path}", file=sys.stderr)
        return 1
    except (OSError, ValueError) as exc:
        print(f"kcov-report: cannot read {path}: {exc}", file=sys.stderr)
        return 1

    seen = set()
    rows = []
    for entry in data.get("files", []):
        full = entry.get("file") or ""
        name = os.path.basename(full)
        if wanted and name not in wanted:
            continue

        total = entry.get("total_lines")
        pct = entry.get("percent_covered")
        if total is None or pct is None:
            print(f"kcov-report: {name}: entry missing line counts",
                  file=sys.stderr)
            return 1

        # kcov reports percent_covered as a string in some versions and a
        # number in others; normalise before formatting so the field is
        # always NN.NN.
        try:
            pct = float(pct)
        except (TypeError, ValueError):
            print(f"kcov-report: {name}: unparseable percentage {pct!r}",
                  file=sys.stderr)
            return 1

        seen.add(name)
        rows.append(f"{name}: {pct:.2f}% of {total}")

    missing = wanted - seen
    if missing:
        # Loud on purpose. A module that produced no kcov data would
        # otherwise reach the gate as "no coverage data" with no clue
        # which step lost it.
        print("kcov-report: no coverage data for: " + " ".join(sorted(missing)),
              file=sys.stderr)
        return 1

    for row in sorted(rows):
        print(row)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
