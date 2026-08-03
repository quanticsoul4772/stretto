# Research: Zig Port (005-zig-port)

**Status**: OPEN — measured, viable, scope undetermined

**Date**: 2026-08-02

**Supersedes**: the earlier 2026-08-02 revision of this file, which
recorded `CLOSED — NO-GO` and stated that "stretto will not be ported
to Zig." That conclusion was wrong. Both the reasoning and the
correction are recorded below rather than deleted, because the failure
mode is reusable and worth not repeating.

## Question

When a module moves from C to Zig, what does the binary pay, and does
the port fit Principle I's budgets?

The packed Linux cap binds at 30 720 B against a measured 30 048 B,
leaving 672 B (2.2 %) of headroom — the tightest of the three budgets,
and therefore the one whose realignment the port forces first.

It does **not** decide the port's scope. An earlier revision of this
file said it did; that framing is withdrawn (`spec.md` Q3, `plan.md`
Non-Goals). The caps keep the C synth small and were set by measuring
what it shipped. They are realigned as the port proceeds, the same way
v1.1.0 and v1.2.0 realigned them.

## Two prior answers, both wrong

### Attempt 1 — measured the wrong strategy

CI run [30753655081](https://github.com/quanticsoul4772/stretto/actions/runs/30753655081)
compiled **all seventeen `.c` files** with `zig cc` and measured
34 432 B packed at best effort — 3 712 B (12.1 %) over cap. That was
written up as a NO-GO on the port.

The measurement is real and is retained below, but it answers a
question nobody asked: it prices **replacing gcc with clang for the
existing C codebase**. It contains zero lines of Zig. The conclusion
drawn from it — that the language does not fit — does not follow from
what was measured.

```
BINARY                     STRIPPED     PACKED PACKED vs CAP
synth.gcc                     49128      30040         -680
synth.gcc.nosep               52520      30208         -512
synth.zig.os-nolto            60800      35524        +4804
synth.zig.os-lto              60144      34432        +3712
synth.zig.oz-lto              60144      34432        +3712
```

Method notes that remain valid for that question: `-Oz` and `-Os` were
byte-identical on CI's LLVM; `-Wl,--icf=all` is rejected
(`unsupported linker arg`); `-fno-pic` is rejected under `zig cc`
(`the selected target requires position independent code`);
`-Wl,-z,noseparate-code` is the one gcc flag `zig cc` will not take,
and `synth.gcc.nosep` isolates its cost at **168 B packed**, so flag
parity does not explain the gap. Under `zig cc`, LLVM codegen does.

**None of this constrains the hybrid design**, where gcc still links
and therefore every linker flag applies normally.

### Attempt 2 — measured the right strategy, misattributed the cost

Hybrid linking was then measured on a VPS: gcc compiles the C,
`zig build-obj` compiles ported modules, gcc links. Two modules came
in at +88 B and +68 B packed, and that was published as the per-module
cost of Zig, with the open question framed as *flat per module* vs
*proportional to lines*.

Both framings ignore the mechanism, and the numbers came from a
machine this document elsewhere declares non-authoritative for size.

## The mechanism

`CFLAGS` carries `-Os -flto -fuse-linker-plugin`. Every `.c` compiles
to GIMPLE bitcode, and gcc inlines across module boundaries at link
time. A `zig build-obj` output is a plain ELF object that the GCC LTO
plugin does not claim.

LTO does not fail and does not degrade globally. **It degrades exactly
at the ported module's boundary**, in both directions: the module
stops being inlined into its callers, and its callers stop being
inlined into it.

That cost is a property of leaving the LTO unit. It has nothing to do
with Zig, and it is measurable **without writing any Zig** — compile
the same `.c`, with the same gcc, with `-fno-lto`.

## Method

CI run [30756190233](https://github.com/quanticsoul4772/stretto/actions/runs/30756190233),
`ubuntu-24.04`, gcc 13.3.0, zig 0.16.0, upx-ucl. Branch
`089-m0-size-probe`, PR #183. Script: `tools/m0-size-probe.sh`.

Five binaries, **one job, one commit**, and — new in this round —
**`STRETTO_VERSION` pinned on the command line for every variant**.

That pin is load-bearing, not hygiene. `version.h` embeds
`git describe --tags --always --dirty` and is compiled into `main.o`.
Swapping a `.c` for a `.zig` dirties the tree by construction, so a
clean control and a dirty variant embed strings of different length.
The one-commit rule does not cover clean-vs-dirty; only the pin does.
`STRETTO_VERSION` is a `:=` variable, so a command-line assignment
wins.

Every variant renders 16 s at seed 0 and is checked against the
committed golden. A size number from a binary that renders different
audio measures nothing.

Variant 1's compile line is lifted from `make -n` rather than
transcribed, so it is `CFLAGS` parity by construction and differs from
the control by exactly one flag.

## Result

```
                                  stripped   packed   Δstripped  Δpacked   cliff
0  control (all C, gcc -flto)        49096    30036         —        —      3428
1  density.c -fno-lto (still C)      49136    30048       +40      +12      3396
2  density.zig untuned               49328    30140      +232     +104      3204
3  density.zig TUNED                 49136    30032       +40       -4      3396
4  density + chord .zig TUNED        49136    30096       +40      +60      3396

budget_linux_synth_stripped=51200
budget_linux_synth_packed=30720
```

All five **match the golden hash**.

### Zig codegen costs zero

Variant 3 stripped is 49 136. Variant 1 stripped is 49 136.
Byte-for-byte identical — a tuned Zig module and the same module in
C-without-LTO produce the same binary size.

The entire per-module cost is LTO exit: **+40 B stripped / +12 B
packed**, measured in C.

### The +88 B figure was an untuned-rule artifact

Variant 2 minus variant 3 is **192 B stripped / 108 B packed**, and
`readelf -SW` on the objects gives the cause:

- Untuned `zig build-obj` emits `.eh_frame` and `.rela.eh_frame`.
  **`strip -s` does not remove `.eh_frame`**, so those bytes reach the
  binary. `CFLAGS` carries `-fno-asynchronous-unwind-tables` for
  exactly this reason, and it never transferred to the Zig rule.
- Untuned output is a single `.text` blob, so `-Wl,--gc-sections` has
  nothing to work with. Tuned output splits per function
  (`.text..density_bias_gate`, `.data..tension`).

Recovered by: `-ffunction-sections -fdata-sections -fno-unwind-tables
-fomit-frame-pointer -fno-PIC -fno-PIE`.

The general lesson is that **`zig build-obj` inherits none of
`CFLAGS`**. gcc still links, so every *linker* flag applies — but the
compile-side flag stack must be restated for each Zig rule.

### The second module cost zero stripped bytes

Variant 4 is also 49 136. `chord_progression` is table-driven and
`-Os` kept it out-of-line already, so it had no inlining to lose.

This kills "cost is proportional to module size": `chord_progression`
is 80 source lines against `density`'s 42, and cost the larger module
*nothing*. **Cost is bounded by how much cross-module inlining a
module was receiving.** For leaves that is near zero. The per-sample
path — `mixer.c:16-22` calls `gen_step()` and `voice_pool_mix()` once
per sample; `voice.c:305,644,648` call `sat16()` from `effects.c`
inside it — is where a real number would come from.

### Headroom

Two modules cost **60 B packed**. Headroom against the 30 720 cap is
**624 B**.

Page-cliff headroom is 3 396 B across all tuned variants; the advisory
threshold in `tools/size-budget-gate.sh` is 256 B, so no cliff risk at
this size.

### On the packed column

Variant 3 lands 4 B *below* the all-C control at identical stripped
size, and variant 4 adds 60 B packed at identical stripped size. UPX
compresses different content differently. **Stripped is the clean
signal; packed carries tens of bytes of compression noise** and should
not be read at finer resolution than that.

## Measurement hazard: `version.h`

Retained from the earlier revision, now mitigated rather than only
documented.

`version.h` is compiled into the binary and generated from
`git describe --tags --always --dirty`. The embedded string carries
the commit SHA and a `-dirty` suffix; different content compresses
differently while the length stays constant. Observed packed sizes for
**identical source**:

| tree state | describe | packed |
|---|---|---|
| clean, `c7db9fc` | `…-gc7db9fc` | 30 048 |
| dirty, `9e4a8c2` | `…-g9e4a8c2-dirty` | 30 056 |
| dirty, `272751b` | `…-g272751b-dirty` | 30 040 |
| clean, `33d7ff5` | `…-g33d7ff5` | 30 048 |
| clean, `dfae0cd` | `…-gdfae0cd` | 30 040 |

Stripped size is pinned at 49 128 B throughout. UPX itself is
deterministic: packing the same ELF three times produced byte-
identical output.

The M0 control landing at 30 036 against the repo's recorded 30 048 for
the same code is this term, removed from all five variants equally by
the pin.

**Rule**: any size comparison must build every variant in one job at
one commit **with `STRETTO_VERSION` pinned on the command line**.

## Determinism result (independent of the port)

All LLVM builds render byte-identical audio to the golden, across
optimization levels and both toolchain arrangements, on machines other
than the one that produced the golden:

```
golden      : 11dcb3a19ffafa22e29bff26fe78fed38d0208f967506b20d97492499593db69
zig cc ×3   : MATCHES golden   (attempt 1, -Os/-Oz, LTO on and off)
hybrid ×3   : MATCHES golden   (M0 variants 2, 3, 4)
```

Principle III states the engine is integer-only (int16/int32/int64, no
`double` or `float` in any synth/voice/mixer/effects module), which
makes bit-exactness compiler-independent by construction. That is now
an empirical cross-compiler, cross-machine result rather than an
argument from the text.

Note this also means `-ffast-math` in `CFLAGS` is a dead flag for the
engine — it is applied to every runtime object but there is no
floating-point arithmetic for it to affect. Under hybrid it is doubly
moot, since gcc still compiles fifteen of seventeen modules untouched.
(An earlier draft had this backwards and claimed `-ffast-math` reached
only the build-time generators. It does not; the generators are built
with a bare `gcc -O2 -Wall -Wextra`.)

## The cost model, revised by the ports themselves

M0 concluded that Zig codegen costs zero and the whole per-module cost
is LTO exit — which implied that compiling a `.c` with `-fno-lto`
prices its port without writing any Zig. That probe was run for every
remaining module before the ports proceeded.

**It is a floor, not a prediction.** Measured against matched controls,
same commit, `STRETTO_VERSION` pinned:

| module | `-fno-lto` probe | actual | note |
|---|---|---|---|
| `density` | +40 | +40 | exact |
| `chord_progression` | +0 | +0 | exact |
| `motif` | not probed | +256 | |
| `section` | +96 | **+192** | 2× |
| `lsystem` | **−40** | **+376** | sign flip |

M0's "codegen costs zero" was established on `density`: four functions,
each a load or a subtract-shift. The probe stays exact for modules of
that shape and degrades as the module computes. `section`'s `lerp_bias`
does two multiplies, a division and an arithmetic shift; `lsystem`
expands a grammar through three generations with a nested rewrite loop,
and LLVM's codegen for that is substantially larger than gcc's — enough
to invert a predicted saving into a +376 B cost.

**Revised model**: cost = LTO exit + a codegen delta that is ~0 for
trivial accessors, ~1× the LTO term for arithmetic, and unbounded by
the probe for control-flow-heavy code. The `-fno-lto` figure is a lower
bound and should be quoted as one.

The combined five-module probe figure (+984 stripped / +280 packed for
all of `section`, `lsystem`, `effects`, `voice`, `gen`) inherits the
same floor status. `section` and `lsystem` alone came to +568 stripped
against a +56 prediction for the pair.

## Open questions

1. **What do the coupled modules cost?** Every measurement so far is a
   leaf. `effects`, `voice`, and `gen` sit in the per-sample path and
   are where LTO exit could be expensive. No number exists.
2. **How are ported modules coverage-gated?** `ci.yml` keys a bash
   associative array by `.c` filename over fifteen files, and both
   `make coverage` and `make test-asan` derive their object lists from
   `COV_SRCS` — a `.c` filename list with only `.c` pattern rules. The
   first port PR does not fail a gate; **it fails to build**, in two
   required checks. See `plan.md` Phase 1.
3. **`@cImport` or hand-declared `extern`?** Forced at `voice`, and by
   the tables rather than the struct: `voice.c:4-7` includes four
   generated headers holding ~2 500 `static const` entries that cannot
   be hand-transcribed.

## What is settled

- Hybrid linking works and is bit-exact.
- Zig codegen is free at leaf scale; the cost is LTO exit.
- The Zig rule must restate the `CFLAGS` codegen stack; it inherits
  nothing.
- Cost tracks inlining received, not source size.
- No cliff risk at current size.

## What survives from the earlier revision

- **PR #181** (`087-commit-generated-tables`) — the five generated
  table headers are committed, closing the host-libm reproducibility
  hole that the `ubuntu-24.04` CI pin only narrowed. Merged on its own
  merit, independent of the port. It also makes Principle III's
  "committed to headers" claim true for the first time.
- The determinism result above.
- The `version.h` hazard, now with a mitigation.

## Constraints recorded so a revisit does not rediscover them

1. **The Makefile cannot be deleted.** `ci.yml`'s `sanitizers` job runs
   `make test-asan` and is a required status check (ruleset
   `main-protection`, id 18729764). Zig ships no AddressSanitizer
   runtime. This does not constrain hybrid, which keeps GNU Make as
   the build system — it constrained the abandoned `build.zig` design,
   and it is why "contributors need Zig and nothing else" was never
   achievable.
2. **The coverage gate is keyed by `.c` filename** in a bash
   associative array in `ci.yml`, over **fifteen** files with
   `keys.c=100` and `main.c=99`. Several measured sets are small —
   `density.c` gates at 95 with zero slack — and gcov counts
   instrumented arcs while kcov/llvm-cov count source lines or
   regions. On files this size the methodologies disagree by more than
   the slack.
3. **`make clean` deleted `$(HEADERS)`**, fixed in `33d7ff5` as part of
   PR #181.
4. **Raising a budget is not one command.**
   `tools/spec-budget-amend.sh` automates the Constitution literal and
   the Makefile variable. It does not touch
   `tests/test_spec_budget_amend.sh:194-217`, whose Case 2 invokes
   `--lin-upx 31` expecting exit 0 — any raise of the Linux UPX cap
   turns that into a no-growth or shrink call, fails the case, and
   reddens a required check with no warning from the tool.
