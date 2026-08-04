# Feature Specification: Incremental Zig Port

**Feature Branch**: `090-zig-port-spec`

**Created**: 2026-08-02

**Last Updated**: 2026-08-03

**Status**: Complete — all nine in-scope modules ported and merged.

**Input**: User description: "I've been playing around with zig and
wondered if mcplint would be better if written in zig instead" →
scoped down to stretto as the better candidate → "research and
brainstorm the best way to port stretto to zig."

## Overview

stretto is ~6 000 lines of C99 with a 128 KB static arena, an
integer-only audio engine, and three CI-gated binary size budgets.
This spec covers moving individual modules from C to Zig **one at a
time**, keeping GNU Make as the build system and gcc as the linker.

The arrangement is **hybrid linking**: gcc compiles the remaining C
exactly as today, `zig build-obj` compiles each ported module to an
object file, and gcc links everything. The module's `.h` file stays
unchanged and remains the seam.

*Granularity changed after completion (PR #215).* The release, Windows
and debug links now take **one** Zig object built from a root that
`@import`s all nine modules, because nine separate `zig build-obj`
invocations were nine separate optimization units — worth −144 B of
`.text`. Coverage and sanitizer builds still compile the nine
separately so the per-file coverage gate keeps working. Hybrid linking
itself is unchanged: gcc still compiles every `.c` and still links
every target, and the `.h` seam is untouched.

This is deliberately not a rewrite. There is no `build.zig`, no
comptime table generation, no cross-compilation consolidation, and no
end state in which the project is "a Zig project." Those were all
goals of an earlier design that measured 12 % over the packed budget;
see `research.md`.

**The motivation is Zig itself.** This spec does not claim a build
simplification, a size win, or a portability win, because measurement
shows none of those are available (`plan.md`, Non-Goals). Post-port
tooling is strictly larger than today's.

## Clarifications

### Session 2026-08-02

- **Q1: Rewrite in place or start a new project?** → **In place, one
  module per PR.** The `.h` seam means a ported module is a drop-in
  object; a parallel project would duplicate the golden-hash
  infrastructure and the size gates that make the port verifiable at
  all.
- **Q2: `build.zig` or keep GNU Make?** → **Keep GNU Make.**
  `ci.yml`'s `sanitizers` job runs `make test-asan` and is a required
  status check; Zig ships no AddressSanitizer runtime, so the Makefile
  could never be deleted. Given a permanent dual build, `build.zig`
  buys nothing and costs a translation of ten build graphs.
- **Q3: What decides how far the port goes?** → **Nothing. Every
  in-scope module is ported.** The size budgets do not scope this work
  and never should have been read as scoping it.

  Principle I's caps exist to keep the *C synth* small — that was the
  original project's goal, and the caps were set by measuring what it
  actually shipped. This is a port to Zig. Its purpose is that the
  code is in Zig. Letting a constraint inherited from the thing being
  ported decide how much of the port happens is a category error, and
  two earlier revisions of this document made it: first as a
  self-authored GO/NO-GO gate that closed the port outright, then as
  "the measured packed size decides how far it goes," which is the
  same mistake with the trigger moved.

  **The caps move with the work.** When a port PR would exceed one,
  the cap is realigned to measured reality in an amendment — exactly
  what v1.1.0 and v1.2.0 did, and what `tools/spec-budget-amend.sh`
  exists for. Size is measured and reported on every port PR because
  the numbers are worth knowing, not because they gate anything.
- **Q4: Which modules?** → **Leaves first, coupled modules last, and
  the audio backends / UI / entry point never.** See FR-010.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — A ported module is indistinguishable at runtime (Priority: P1)

A module is reimplemented in Zig. The synth renders byte-identical
audio, every unit test passes unchanged, and the binary still fits its
budgets.

**Why this priority**: The whole port is only viable if the `.h` seam
holds. If a ported module requires touching its callers or its tests,
the per-module PR model collapses.

**Independent Test**: `make && ./synth --render 16 out.wav --seed 0`
and compare against `golden/regression_16s.sha256`.

**Acceptance Scenarios**:

1. **Given** a module ported to Zig, **When** the synth renders 16 s at
   seed 0, **Then** the SHA-256 matches the committed golden byte for
   byte.
2. **Given** a module ported to Zig, **When** `make test-unit` runs,
   **Then** the module's existing C test file passes **without
   modification**.
3. **Given** a module ported to Zig, **When** `make win` runs, **Then**
   the Windows cross-build still links and packs.

### User Story 2 — Size cost is known before it is spent (Priority: P1)

Every port PR reports what it cost, measured in CI, against the budget.

**Why this priority**: The packed Linux budget has ~624 B of headroom.
Two prior attempts at this measurement produced wrong numbers, once by
measuring the wrong strategy and once by measuring on a machine that
cannot produce a valid absolute. A port that cannot report its cost
accurately cannot be scoped.

**Independent Test**: `make size` output in the PR body, from a CI run
on `ubuntu-24.04`.

**Acceptance Scenarios**:

1. **Given** a port PR, **When** CI completes, **Then** the PR body
   carries `linux_synth_stripped`, `linux_synth_packed`, and
   `linux_synth_page_cliff_headroom`.
2. **Given** a size comparison between variants, **When** it is taken,
   **Then** all variants were built in one job at one commit with
   `STRETTO_VERSION` pinned on the command line.

### User Story 3 — Test discipline survives the language boundary (Priority: P1)

A ported module is still coverage-gated and still built under
sanitizers.

**Why this priority**: Principle VI is NON-NEGOTIABLE. Both
`make coverage` and `make test-asan` derive their object lists from
`COV_SRCS`, a hardcoded `.c` filename list served by `.c`-only pattern
rules — so the first port PR does not merely lose a gate, it **fails
to build** in two required checks.

**Acceptance Scenarios**:

1. **Given** a ported module, **When** `make coverage` runs, **Then** it
   builds and emits a per-file percentage the CI gate can parse.
2. **Given** a ported module, **When** `make test-asan` runs, **Then**
   it builds, links, and the suite passes.

## Requirements *(mandatory)*

### Functional

- **FR-001**: A ported module MUST expose the same symbols with the
  same C ABI signatures as the `.c` it replaces, via `export fn`.
- **FR-002**: The module's `.h` file MUST NOT change. C callers
  continue to `#include` it unmodified.
- **FR-003**: Rendered audio MUST be byte-identical to the committed
  golden at every seed the multi-seed test covers.
- **FR-004**: Every file-scope variable MUST carry an initializer
  matching the C declaration's initializer. `undefined` is prohibited:
  it is 0xAA-poisoned outside ReleaseSmall/ReleaseFast and moves the
  symbol out of `.bss`.
- **FR-005**: Signed integer division MUST use `@divTrunc` to match
  C's truncate-toward-zero. `@divFloor` differs for negative
  numerators.
- **FR-006**: Left shifts that discard a nonzero bit need **no special
  operator**. Zig's `<<` on an unsigned type discards high bits exactly
  as C's does; the illegal case is a shift *amount* at or above the bit
  width. The sites this requirement was written for — the xorshift PRNG
  at `voice.c:93,95` and `gen.c:49,51,343,345`, and the FM modulation
  index at `voice.c:355` which reaches ~1.97e8 before shifting — all
  translate as plain `<<`.

  *Corrected 2026-08-03 during the `voice` port.* This requirement
  previously mandated `<<%`. **There is no `<<%` operator in Zig** — it
  does not parse. Computed shift *amounts* still need care (see
  `lsystem.c:180`), which is the real hazard in this family.
- **FR-007**: Integer promotion sites MUST make an explicit
  `@truncate` (wrap) vs `@intCast` (trap) choice, recorded in the PR.
  C widens `int16_t` to `int` automatically and Zig does not; affected
  sites include `voice.c:285`, `voice.c:322-323`, `effects.c:271,273`,
  and `section.c:69,77`.
- **FR-008**: Each Zig compile rule MUST restate the `CFLAGS` codegen
  stack. `zig build-obj` inherits nothing, and an untuned invocation
  measured 192 B stripped worse than a tuned one.
- **FR-009**: A ported module MUST build for both `x86_64-linux-gnu`
  and `x86_64-windows-gnu`. `WIN_OBJS` is derived from `COMMON_OBJS`,
  so a module that does not cross-compile breaks `make win`.
- **FR-010**: Port order is leaves first:
  `density` → `chord_progression` → `motif` → `section` → `lsystem` →
  `effects` → `voice` → `gen` → `arena`, subject to revision once a
  coupled module has been measured. (`arena` was appended once its
  exclusion rationale expired — see Out of Scope.)
- **FR-011**: A ported module MUST remain coverage-gated at a
  documented threshold, and MUST remain in the sanitizer build.
- **FR-012**: The Zig toolchain version MUST be pinned in CI, for the
  same reason `ci.yml` pins the runner image: a codegen change in a
  0.x compiler can alter the render hash with no source change.
- **FR-013**: Every port PR MUST report CI-measured stripped, packed,
  and page-cliff headroom.

### Non-Functional

- **NFR-001**: No runtime dependency beyond libc + libpulse (Linux) /
  winmm (Windows). Verified per module with `nm -u` on the produced
  object.
- **NFR-002**: No change to the 128 KB static arena, its alignment
  contract, or the no-`malloc` rule.
- **NFR-003**: GNU Make remains the build system; gcc remains the
  linker for every target.

## Out of Scope

- `build.zig` or any replacement of GNU Make.
- Deleting the Makefile.
- Porting `main.c`, `ui.c`, `keys.c`, `wav.c`, `mixer.c`, or any
  `audio_*.c`. These stay C permanently.
- ~~**Porting `arena.c`.**~~ **Ported after all — the exclusion
  outlived its reason.** It was excluded because moving arena to Zig
  removes the ASan redzone gcc puts around `pool`, and the writes that
  redzone catches came from `effects.c` and `voice.c`, *"which remain C
  and remain instrumented."*

  Both are Zig now. gcc cannot instrument a `zig build-obj` output, so
  every module allocating from the pool is already uninstrumented and
  the redzone protects nothing it used to. Leaving arena as the one C
  module on the synth side would have been arbitrary rather than
  principled.
- comptime generation of the five lookup tables. PR #181 committed
  them; regenerating them in Zig would reopen a reproducibility hole
  that was just closed.
- Any claim of toolchain simplification. See `plan.md` Non-Goals.

## Success Criteria

- **SC-001**: Golden hash byte-identical after every port.
- **SC-002**: Every existing unit test passes unmodified.
- **SC-003**: All required status checks green on every port PR.
- **SC-004**: Cumulative packed size reported per PR, with headroom
  against `PACK_TARGET` stated explicitly.
- **SC-005**: The Constitution accurately describes the tree at every
  merge point.

## Assumptions

- The port completes: every in-scope module ends up in Zig. Budgets are
  realigned to measured reality as the work requires them to be. This
  spec measures and reports; it does not gate.
- Zig 0.16.0 is the pinned baseline. A version bump is treated as a
  golden-regeneration-class change requiring its own PR.

## Current state

*Rewritten 2026-08-03, at completion. The original text is preserved
below because it was the record for the whole port and because what it
got wrong is the point.*

- **All nine in-scope modules are ported and merged**: `density`,
  `chord_progression`, `motif`, `section`, `lsystem`, `effects`,
  `voice`, `gen`, `arena`. Every one is referenced by the `Makefile`
  through the five Zig pattern rules; the corresponding `.c` files are
  deleted.
- Every `.h` is unchanged, every unit test passes without
  modification, and the golden hash is byte-identical throughout —
  SC-001, SC-002 and SC-003 met.
- Size at completion, CI-measured on run 30784977189: **52 600 B
  stripped / 31 728 B packed / 45 056 B Windows packed**, leaving
  8.3 % / 9.7 % / 9.1 %. The binding constraint is no longer any cap
  but the **page cliff**: 324 B of headroom against a 256 B advisory.
- Principle II was amended to "C99 and Zig" at v1.3.0 (PR #187), and
  the Constitution's *preamble* was caught up at v1.4.3 (PR #211),
  29 merges later. **SC-005 is not met and cannot become met** — it
  requires accuracy at every merge point, and those are past. See
  `research.md`, which records both windows.

### Original text, as written 2026-08-02

> - `zig/density.zig` and `zig/chord_progression.zig` are on `main`
>   (PR #183) as probe inputs. **Neither is referenced by the
>   `Makefile`**; the build is unaffected by their presence.
> - Their presence means `main` carries Zig source while Principle II
>   reads "C99 Only". Principle II is not machine-enforced — only
>   Principle I is bridged — so nothing fails, but the Constitution is
>   currently inaccurate about the tree. See `plan.md` Phase 0.

This section stayed on `main` unchanged through all nine module PRs.
It says the build is unaffected by Zig and that two modules are
unreferenced, and by the end both clauses were false in every
particular. Nothing pointed at it, because each PR updated the record
it was changing and no PR asked which records its change had
falsified.
