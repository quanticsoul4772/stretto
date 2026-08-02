# Implementation Plan: Incremental Zig Port

**Branch**: `090-zig-port-spec` | **Date**: 2026-08-02 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/005-zig-port/spec.md`

## Summary

Move stretto's modules from C to Zig one at a time using **hybrid
linking**: gcc compiles the remaining C exactly as today,
`zig build-obj` compiles each ported module to an object, and gcc
links everything. GNU Make stays. The `.h` file stays and remains the
seam.

The size question that governs scope is measured and answered for
leaves: **Zig codegen costs zero.** A tuned Zig module and the same
module compiled as C with `-fno-lto` produce byte-identical stripped
binaries (49 136 both). The entire per-module cost is leaving gcc's
LTO unit, at +40 B stripped / +12 B packed. See
[research.md](./research.md).

Unmeasured: the coupled modules (`effects`, `voice`, `gen`) in the
per-sample path, where LTO exit could be expensive.

## Technical Context

**Language/Version**: C99 (gcc 13) + Zig 0.16.0, pinned
**Build**: GNU Make; gcc links every target
**Primary Dependencies**: libc, libpulse, libasound (Linux); winmm (Windows)
**Storage**: 128 KB static arena, bump allocator, no `malloc`
**Testing**: `make test` (bit-exact golden), `make test-unit`, `make test-multiseed`, `make coverage`, `make test-asan`
**Target Platform**: Linux x86-64 (glibc), Windows x86-64 (MinGW cross)
**Performance Goals**: unchanged — 48 kHz render, realtime PulseAudio callback in live mode
**Constraints**: `STRIP_TARGET = 51200`, `PACK_TARGET = 30720`, `WIN_PACK_BUDGET = 49152`; currently 49 096 / 30 036 / 43 520 measured
**Scale/Scope**: 8 candidate modules of 17; ~2 800 of ~6 000 lines

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Note |
|---|---|---|
| **I. Tiny Native Binary (NON-NEGOTIABLE)** | ✅ PASS (measured) | CI run 30756190233, `ubuntu-24.04`, one job one commit, `STRETTO_VERSION` pinned. Two ported leaves: **49 136 stripped / 30 096 packed**, against caps of 51 200 / 30 720 — **2 064 B and 624 B of headroom**. Per-module cost is +40 B stripped / +12 B packed and is **LTO exit, not Zig**: variant 1 (`density.c` compiled `-fno-lto`, still C) and variant 3 (`density.zig` tuned) are both 49 136 stripped, byte-identical. Page-cliff headroom 3 396 B against a 256 B advisory threshold. Coupled modules unmeasured; each PR reports its own number (FR-013) and scope decisions belong to the repo owner. |
| **II. C99 and Zig** | ✅ PASS (as of v1.3.0) | Retitled from "C99 Only" and amended to make C99 and Zig both first-class. **"No build system beyond GNU Make" stays true** — hybrid keeps Make and gcc-as-linker. `nm -u` on the produced objects is empty, so the runtime-dependency clause (libc + libpulse / winmm) holds unchanged. Was a standing violation from PR #183 until the amendment landed. |
| **III. Deterministic (NON-NEGOTIABLE)** | ✅ PASS | Six independent LLVM builds across two toolchain arrangements and three optimization levels all render byte-identical to the golden, on machines other than the one that produced it. The integer-only engine makes this compiler-independent by construction, and it is now an empirical cross-compiler result rather than an argument from the text. "Committed to headers" became true with PR #181. FR-004 through FR-007 pin the translation hazards that could break it (initializer matching, `@divTrunc`, `<<%`, promotion sites). |
| **IV. Ambient + Algorithmic Aesthetic** | ✅ N/A | An implementation-language change with a bit-exactness gate cannot alter the aesthetic. |
| **V. Cleanly Modular** | ✅ PASS (as of v1.3.0) | The one-way dependency graph is untouched; no module gains or loses an edge, and it was always a module graph rather than a file graph. Amended on two clauses: "one `.c`/`.h` pair" now admits `.zig`/`.h`, and the **"no `extern` declarations across module boundaries"** sentence carries a bounded exception for Zig `export fn`/`extern fn` **against the module's own `.h`**. What is genuinely lost — compile-time signature checking — is named in the principle, with the bit-exact golden as the stated backstop. Still unavoidable at `voice`, by the generated tables rather than the struct. |
| **VI. Test Discipline (NON-NEGOTIABLE)** | ✅ PASS, with a named reduction (v1.3.0) | Every unit test survives unmodified — verified, not assumed: every file in `tests/unit/` includes only `test.h` and `../../<module>.h`, none `#include`s a `.c` or names a file-scope static, and `test_density.c` passes 7/7 against `density.zig`. **Coverage is not lost**: Phase 1 established that kcov measures Zig modules (`density.zig: 100.00% of 14`, read by the existing gate parser), so the per-file gate still applies and no exclusion was needed. Two real reductions are recorded in the principle rather than absorbed: ported modules get no gcc sanitizer instrumentation — mitigated by building them `-OReleaseSafe` in that tree, keeping Zig's overflow/bounds checks — and they leave the `-Werror` warning gate, which lives in `SAN_FLAGS` and nowhere else. |
| **VII. No Partial Features** | ✅ PASS | Each PR ships one complete module: implementation, unchanged `.h`, passing tests, coverage gate, both targets. No `TODO`, no stub, no half-ported module. A port that cannot complete is not merged. |
| **VIII. Document Why, Not What** | ✅ PASS | `research.md` records the mechanism (LTO-unit exit) and, deliberately, two wrong prior conclusions with their reasoning. Each ported file documents its translation choices at the site. Phase 6 is a mandatory docs refresh per Workflow §5. |
| **IX. Cross-Platform From Day One** | ✅ PASS | FR-009 requires every module to build for `x86_64-windows-gnu` in the same PR as its Linux port; `WIN_OBJS` derives from `COMMON_OBJS`, so a module that does not cross-compile breaks `make win` immediately. Windows packed headroom is 5 632 B, far from binding. `windows-smoke.yml` carries the only automated cross-target byte-identity check and must gain Zig in Phase 2. |
| **X. Generative > Random** | ✅ N/A | No change to generative behavior; the golden hash gates it. |

**Gate result**: ✅ **PASS as of Constitution v1.3.0.** Principles II and V amended; Principle VI resolved by Phase 1 (kcov measures Zig modules, so the per-file gate survives) with its two real reductions named in the principle. All ten principles pass; the Complexity Tracking rows below record what each amendment cost.

## Complexity Tracking

> Filled because the Constitution Check has violations that must be justified.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|--------------------------------------|
| **II. C99 Only** — Zig source in the tree | The port is the feature. There is no version of it that keeps the tree C99. | *Keeping stretto in C* is the simpler alternative and remains available at every PR boundary — the port is incremental by construction and can stop at any module. It is rejected only insofar as the repo owner wants Zig; this plan asserts no technical necessity, because measurement shows there is none (see Non-Goals). |
| **V. no cross-module `extern`** — Zig `export fn`/`extern fn` | A Zig module cannot `#include` a C header, so its exported signatures are hand-declared against the `.h` rather than checked by a compiler. | `@cImport` avoids hand-declaration but produces distinct incompatible types per importing module, forcing a shared `c.zig` and flattening the per-module `.h` seam that Principle V exists to protect. The trade is real: hand-declaration risks silent ABI drift, `@cImport` guarantees seam loss. Mitigation is a C-side static-assert TU or `zig translate-c` diffed in CI. Forced at `voice` regardless, by the generated tables rather than the struct. |
| **VI. sanitizer instrumentation + warning gate** | `-fsanitize=address` cannot instrument a `zig build-obj` output, and `-Wall -Wextra -Werror` has no `zig build-obj` equivalent covering the project's rules. | *Keeping a C copy of each ported module solely for coverage* was rejected outright — the gate would measure code that is not in the shipped binary and the copies would drift silently. *Excluding ported modules from the coverage gate* was prepared and proved unnecessary: kcov measures them. Building the Zig object at `-OReleaseSafe` in the `build_san` tree only (that tree never touches the release binary, so zero size risk) recovers overflow and bounds checking, which is strictly more than ASan gave an uninstrumented object. The `-Werror` loss has no mitigation today; a Zig lint step would close it. |

## Project Structure

```
zig/                        # ported modules
  density.zig               # on main (PR #183), not yet referenced by Makefile
  chord_progression.zig     # on main (PR #183), not yet referenced by Makefile
<module>.h                  # unchanged; still the seam
tools/m0-size-probe.sh      # M0 measurement harness
specs/005-zig-port/
  spec.md  plan.md  research.md
```

## Phases

### Phase 0 — Correct the record *(this PR)*

`research.md` recorded `CLOSED — NO-GO` on `main`. That is false and is
replaced with the M0 measurements and the LTO-exit cost model. `spec.md`
and this `plan.md` bring the directory to the shape every other
`specs/` directory has.

Also recorded here rather than left implicit: PR #183 put Zig source on
`main` ahead of the Principle II amendment, so Phase 3 is now
retroactive rather than prospective.

### Phase 1 — Instrumented-build resolution *(blocking)*

The first port PR fails to build in two required checks, before any
gate logic runs:

- `Makefile:462` — `COV_SRCS_MEASURED` is a literal `.c` filename list
- `Makefile:472-473` — `COV_OBJS` derives from it
- `Makefile:545` — `SAN_OBJS` reuses the same list
- `Makefile:491`, `:559` — only `%.o: %.c` rules exist

Delete `density.c` and both die with `No rule to make target`.

**Five pattern-rule classes need Zig counterparts, not two**: `%.o`
(`:168`), `%.win.o` (`:171`, different flag set), `%.dbg.o` (`:626`),
`$(BUILD_COV)/%.o` (`:491`), `$(BUILD_SAN)/%.o` (`:559`). All five need
`$(HEADERS)` as an explicit prerequisite — Zig emits no `.d` files, so
ported modules leave `-MMD -MP` tracking.

Spike, in order: whether `-fprofile-instr-generate -fcoverage-mapping`
work through `zig build-obj` (these are documented `zig cc` flags, not
Zig-source-compiler flags — expect no); **kcov** as the likely answer,
being DWARF-based and needing no instrumentation; and the fact that
`Makefile:519` ends the coverage recipe with `| grep "\.c"`, so a
`.zig` row cannot reach `coverage.log` until that filter changes.

### Phase 2 — CI toolchain availability

No workflow installs Zig. Four need it: `ci.yml:42-45` (`sanitizers`,
required), `ci.yml:65-71` (`build-test-coverage`, required),
`release.yml:36-42` (builds published artifacts; breaks at the next
tag with nothing catching it earlier), `windows-smoke.yml:56-59`.

Pinned, per FR-012. **Do not add a CI job** — `ci.yml:27-32` warns
that required contexts match by rendered job name and that adding one
requires editing ruleset 18729764 in the same change.

Downstream build deps also need Zig: `packaging/aur/PKGBUILD:17`,
`Formula/stretto.rb:23-25`, and `install.sh:81-83`'s build-from-source
instruction.

### Phase 3 — Constitution amendment (v1.3.0)

Principles II and V per the Complexity Tracking table; Principle VI per
Phase 1's outcome; plus **Memory model** (`:65`, names `arena.c` — now
resolved by putting arena out of scope), **Build infrastructure**
(`:68`, enumerates four pattern-rule classes where there are five),
**Tooling required** (`:70-75`, add pinned Zig and whatever Phase 1
concludes), and **Development Workflow §3** (`:81-88`, seven local
commands now need Zig installed). Principle VI's exclusion list is
already stale — `ui.c`/`keys.c` graduated into `COV_SRCS_MEASURED` —
and should be fixed while the file is open.

**Do not reflow Principle I's paragraph.** Three suites parse it by
literal string including the U+2264 character:
`tools/spec-budget-check.sh:107-109`,
`tests/test_spec_budget_check.sh:185,240`,
`tests/test_spec_budget_amend.sh:168,199`.

**Practical**: `tests/test_spec_budget_amend.sh:69` aborts FATAL on a
dirty `constitution.md` or `Makefile`, and runs inside `make test` and
`make verify`. Commit before verifying locally.

### Phase 4 — Language-agnostic coverage gate — **WITHDRAWN**

Planned: re-key `ci.yml:226-242` and `Makefile:462-519` from filename to
module name, with a Makefile-emitted manifest resolving the extension.
The stated motivation was to keep a port PR from touching `ci.yml`, on
the grounds that Workflow §2 forbids mixed-purpose PRs.

Withdrawn on evidence from Phase 1, which shipped before it. Phase 1's
kcov rows are keyed by the real filename (`density.zig: 100.00% of 14`)
in gcov's exact shape, and the **unmodified** gate parses them — its own
pipeline returned `100` for `density.zig` while `chord_progression.c`
still returned `92`. No re-key is required for correctness.

The mixed-purpose argument was also wrong. Flipping a module's own
threshold from `[density.c]=95` to `[density.zig]=95` is part of porting
that module, not a second purpose, and the one-line diff is
self-documenting: the threshold row records the language change right
next to the per-file rationale comment explaining the number.

The manifest would have added an indirection layer between the gate and
the filename it actually measures, to solve a problem that does not
exist. Each port PR flips its own entry instead — seven one-line edits
over the life of the port.

### Phase 5 — Build rules, then modules

Rules first, inert (no `zig/*.zig` is referenced yet, so nothing
changes and every `make` in CI parses them). Then one module per PR.

**Two pattern rules for one target is safe**: with `density.c` deleted,
`%.o: %.c $(HEADERS)` cannot apply and make falls through to
`%.o: zig/%.zig`, deterministically.

**Stale `.d` files are not.** `Makefile:650` is
`-include $(OBJS:.o=.d)` and `-MP` emits a phony for the header but
not the source, so a built tree holds `density.o: density.c density.h`.
Deleting `density.c` gives `No rule to make target 'density.c'` on
every tree that has built before. **CI passes on a fresh checkout while
every dev box breaks** — the PR body must say `make clean`, or the rule
needs a `density.c:` phony guard.

Take one **coupled**-module measurement before committing to the rest
of the order. Every measurement so far is a leaf, and leaves are the
cheap case by construction.

### Phase 6 — Docs refresh *(mandatory)*

Workflow §5 requires a `docs/*` PR after 5+ accumulated changes; this
arc is ten-plus. Stale surface: `ARCHITECTURE.md:9-12` (size table),
`:37-125` (module layout, every entry `foo.c/.h`), `:127-141`
(dependency graph), `:660-678` (coverage table), `:714-743` (CI steps),
`:745-749` (build flags — ported modules are not in the gcc LTO unit),
`:16-35` (spec-kit pipeline, needs an 005 entry). Plus `README.md` and
`CHANGELOG.md`.

## Non-Goals *(stated because two prior plan revisions claimed them)*

- **Toolchain simplification.** Post-port tooling is gcc + MinGW +
  pinned Zig + UPX + bash + python3/numpy + gcov + kcov — strictly
  more than today, and the burden reaches downstream source builders,
  not just CI. The earlier cross-compilation and comptime-tables
  arguments both evaporate under hybrid.
- **A size win.** Measured cost is zero-to-positive, never negative.
- **`build.zig`.** Deleted from the plan, not deferred.
- **A stopping rule authored here.** An earlier revision wrote a
  GO/NO-GO gate into this plan and then executed it, closing the port
  on a measurement of the wrong strategy. Scope decisions and budget
  changes belong to the repo owner; `tools/spec-budget-amend.sh` is
  the supported mechanism for the latter, with the caveat recorded in
  `research.md` that it is not a one-command operation.

## Amendment history

- **2026-08-02** — Initial. Supersedes an unversioned plan that
  proposed `build.zig` compiling all C via `addCSourceFiles`, measured
  12 % over the packed cap, and concluded the port was closed. Both
  the measurement and the conclusion are retained in `research.md`;
  only the conclusion was wrong.
