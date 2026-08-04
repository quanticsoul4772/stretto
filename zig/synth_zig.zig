//! Single translation unit for the nine ported synth modules.
//!
//! WHY
//!
//! Each module was compiled by its own `zig build-obj` invocation, so
//! each was its own optimization unit -- nine of them. Zig could not
//! inline across those boundaries any more than gcc can inline across
//! a plain ELF object boundary, which is the same mechanism that makes
//! LTO exit the port's dominant cost. Building all nine from one root
//! restores intra-Zig inlining.
//!
//! `comptime { _ = @import(...) }` forces semantic analysis of each
//! module, which is what causes its `export fn` declarations to be
//! emitted. Import order is irrelevant. The exported C ABI surface is
//! unchanged: 108 symbols before and after, identical sets, so every
//! `.h`, every C caller and every unit test is untouched.
//!
//! MEASURED
//!
//!   .text   25 394 -> 25 250 B   (-144)
//!   render hash unchanged, 108 exported symbols unchanged
//!
//! MEASURE .text, NOT FILE SIZE
//!
//! The stripped file stayed at exactly 52 600 B across this change and
//! four other structural variants, because the code segment is padded
//! to page boundaries and absorbs anything under ~4 KB. Reading file
//! size alone reports every one of those experiments as a flat zero.
//! `size -A synth` is the instrument that resolves them.
//!
//! WHAT THIS DOES NOT FIX
//!
//! The remaining cost is the C -> Zig boundary, and it is structural
//! rather than an optimizer setting. In the all-C build gcc inlined
//! the audio pipeline into `render_chunk` (6 921 B of fused code) and
//! then DELETED the standalone bodies, because they were `static` and
//! nothing else referenced them. Every ported function is `export fn`
//! with external C-ABI linkage, so its body must be emitted whether or
//! not it is also inlined. Measured dead ends, all on .text against
//! this baseline: porting `mixer.c` into this unit, +0; rewiring
//! voice -> effects.sat16 as a native Zig call instead of through
//! @cImport, +0; gating test-only exports behind a build option, +80
//! (worse -- inlining a 1 685 B body into its single call site costs
//! more than the body saves); unified clang LTO across both languages,
//! +4 345; ReleaseFast, +12 624.
//!
//! COVERAGE AND SANITIZERS ARE DELIBERATELY NOT MERGED
//!
//! `COV_SRCS_MEASURED_ZIG` still names the nine modules and both
//! instrumented trees still build them separately, because `ci.yml`
//! keys per-file coverage thresholds off those nine filenames. Neither
//! tree contributes to the release binary, so nine objects there costs
//! nothing and keeps the gate measuring what it has always measured.

comptime {
    _ = @import("arena.zig");
    _ = @import("effects.zig");
    _ = @import("voice.zig");
    _ = @import("gen.zig");
    _ = @import("lsystem.zig");
    _ = @import("chord_progression.zig");
    _ = @import("section.zig");
    _ = @import("density.zig");
    _ = @import("motif.zig");
}
