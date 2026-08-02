# Research: Zig Port (005-zig-port)

**Status**: CLOSED — NO-GO
**Date**: 2026-08-02
**Decision**: stretto will not be ported to Zig. Principle I cannot be met.

## Question

Can an LLVM-compiled stretto fit Principle I's three size budgets? The packed
Linux cap binds at 30 720 B against a measured 30 048 B, leaving 672 B (2.2 %)
of headroom, and no published measurement compared Zig `ReleaseSmall` against
GCC `-Os -flto` for an equivalent program.

Everything else about the port — comptime table generation, single-toolchain
cross-compilation, `FixedBufferAllocator` for the arena — was contingent on
this one number.

## Method

CI run [30753655081](https://github.com/quanticsoul4772/stretto/actions/runs/30753655081),
`ubuntu-24.04`, gcc 13.3.0, zig 0.16.0, upx-ucl. Throwaway branch
`m1-zig-size-probe`, never merged, deleted after the run.

Five binaries built in **one job at one commit**, so `version.h` was identical
across all of them (`#define STRETTO_VERSION "1.5.0-7-gdfae0cd"`). This is not
incidental: see *Measurement hazard* below.

`zig cc` was used rather than `zig build`. It is a clang driver and accepts the
gcc flag stack directly, so the codegen question is answered without
confounding it with whether `std.Build` can express those flags.

## Result

```
BINARY                     STRIPPED     PACKED PACKED vs CAP
synth.gcc                     49128      30040         -680
synth.gcc.nosep               52520      30208         -512
synth.zig.os-nolto            60800      35524        +4804
synth.zig.os-lto              60144      34432        +3712
synth.zig.oz-lto              60144      34432        +3712

budget_linux_synth_stripped=51200
budget_linux_synth_packed=30720
VERDICT=NO-GO
```

Best-effort Zig (`-Oz` + LTO) is **3 712 B over the packed cap (12.1 %)** and
**8 944 B over stripped (17.5 %)**.

## Why this is codegen, not tuning

**Every size lever was tried.** `-Oz` (clang's most aggressive size mode),
LTO on and off, `-fno-unwind-tables`, `-fomit-frame-pointer`, on top of the
existing flag stack. `-Wl,--icf=all` — LLD's identical-code-folding, which GNU
ld does not have and which was the most promising Zig-only advantage — is
rejected: `error: unsupported linker arg: --icf`.

**`-Oz` bought nothing on this toolchain.** `os-lto` and `oz-lto` are
byte-identical at 60 144 / 34 432. On a local dev box (zig 0.16.0, gcc 13)
`-Oz` did save ~1 KB, so the lever exists in principle; on CI's LLVM it is
already saturated.

**Flag parity is not the cause.** `zig cc` rejects exactly one gcc flag:

```
-Wl,-z,noseparate-code -> error: unsupported linker extension flag
```

`synth.gcc.nosep` isolates its cost — gcc denied that flag still lands at
30 208 B packed, comfortably under cap. The flag is worth **168 B packed**
(3 392 B stripped). The remaining ~3 500 B is LLVM codegen.

Flags initially assumed unavailable that in fact work under `zig cc`:
`-Wl,--hash-style=sysv`, `-Wl,--build-id=none`, `-Wl,-z,norelro`, `-no-pie`,
`-Qn`, `-latomic`, `-fuse-linker-plugin` (accepted, no effect).

`-fno-pic` is not usable: `error: unable to create module 'arena': the selected
target requires position independent code`.

## Measurement hazard found while probing

`version.h` is compiled into the binary and generated from
`git describe --tags --always --dirty`. The embedded string carries the commit
SHA and a `-dirty` suffix; different content compresses differently while the
length stays constant. Observed packed sizes for **identical source**:

| tree state | describe | packed |
|---|---|---|
| clean, `c7db9fc` | `…-gc7db9fc` | 30 048 |
| dirty, `9e4a8c2` | `…-g9e4a8c2-dirty` | 30 056 |
| dirty, `272751b` | `…-g272751b-dirty` | 30 040 |
| clean, `33d7ff5` | `…-g33d7ff5` | 30 048 |
| clean, `dfae0cd` (CI) | `…-gdfae0cd` | 30 040 |

Stripped size is pinned at 49 128 B throughout. UPX itself is deterministic:
packing the same ELF three times produced byte-identical output.

**Consequence**: any size comparison is valid only between binaries embedding
the same version string. A dirty tree or a different commit injects up to
~16 B of noise into a budget with 672 B of headroom. Future size work must
build every variant in one job at one commit.

## Determinism result (independent of the port)

All three LLVM builds render byte-identical audio to the golden, at three
optimization levels, on a different machine from the one that produced it:

```
golden   : 11dcb3a19ffafa22e29bff26fe78fed38d0208f967506b20d97492499593db69
gcc      : 11dcb3a19ffafa22e29bff26fe78fed38d0208f967506b20d97492499593db69
os-nolto : MATCHES golden
os-lto   : MATCHES golden
oz-lto   : MATCHES golden
```

Principle III states the engine is integer-only (int16/int32/int64, no `double`
or `float` in any synth/voice/mixer/effects module), which makes bit-exactness
compiler-independent by construction. That is now an empirical cross-compiler,
cross-machine result rather than an argument from the text.

Note this also means `-ffast-math` in `CFLAGS` is a dead flag for the engine —
it is applied to every runtime object but there is no floating-point arithmetic
for it to affect. (An earlier draft of this analysis had this backwards, and
claimed `-ffast-math` reached only the build-time generators. It does not; the
generators are built with a bare `gcc -O2 -Wall -Wextra`.)

## Decision

**NO-GO.** Principle I is NON-NEGOTIABLE and the gap is 12.1 % on the binding
budget after best-effort tuning.

Raising `PACK_TARGET` was considered and rejected. Both prior cap amendments
(v1.1.0, v1.2.0) realigned *unenforced, aspirational* targets to measured
reality — figures the shipped synth had never met, gated as warnings only —
and each attributed the growth to specific principles being served
(III Deterministic + IX Cross-Platform + X Generative). There is no precedent
for raising an enforced, currently-met cap, and a toolchain migration can cite
only Principle IX, weakly.

## What survives

- **PR #181** (`087-commit-generated-tables`) — the five generated table
  headers are now committed, closing the host-libm reproducibility hole that
  the `ubuntu-24.04` CI pin only narrowed. Valuable independently of the port
  and merged on its own merit.
- The determinism result above.
- The `version.h` measurement hazard, which constrains any future size work.

## Blockers that would have applied had the size gate passed

Recorded so a future revisit does not rediscover them:

1. **The Makefile cannot be deleted.** `ci.yml`'s `sanitizers` job runs
   `make test-asan` and is a required status check (ruleset `main-protection`,
   id 18729764). Zig ships no AddressSanitizer runtime. The end state would
   have been a permanent dual build, so "contributors need Zig and nothing
   else" was never achievable.
2. **The coverage gate is keyed by `.c` filename** in a bash associative array
   in `ci.yml`, over **fifteen** files with `keys.c=100` and `main.c=99`.
   Several measured sets are tiny — `arena.c` 10 lines, `mixer.c` 12,
   `density.c` 19, `chord_progression.c` 27 — so at 27 lines one line is 3.7
   percentage points. gcov counts instrumented arcs, kcov counts DWARF lines;
   on files this small they disagree by more than the slack.
3. **`make clean` deleted `$(HEADERS)`**, fixed in `33d7ff5` as part of
   PR #181.
