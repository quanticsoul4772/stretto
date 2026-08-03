//! Single static bump allocator, ported from arena.c.
//!
//! arena.h is unchanged and remains the seam.
//!
//! WHY THIS MODULE WAS INITIALLY OUT OF SCOPE, AND WHY IT NO LONGER IS
//!
//! `specs/005-zig-port/spec.md` excluded arena on the grounds that
//! porting it removes the ASan redzone gcc puts around `pool`, and
//! that the writes that redzone catches come from `effects.c` and
//! `voice.c` — "still C, still instrumented".
//!
//! Both are Zig now. gcc cannot instrument a `zig build-obj` output,
//! so every module that allocates from this pool is already
//! uninstrumented and the redzone protects nothing it used to. The
//! exclusion outlived its reason, and leaving arena as the one C
//! module on the synth side would have been arbitrary rather than
//! principled.
//!
//! Translation notes:
//!
//!   * `pool` carries an explicit all-zero initializer and 64-byte
//!     alignment. C zero-fills an uninitialized `static` into .bss;
//!     `undefined` here would move 128 KB into .data, which is the
//!     single largest .bss/.data hazard in the tree (FR-004).
//!
//!   * `bump` likewise: uninitialized `static size_t` in C, explicit 0
//!     here.
//!
//!   * `(n + 7) & ~(size_t)7` rounds up to 8. Zig's `~` on an unsigned
//!     type is the same bitwise complement, so the idiom translates
//!     directly.
//!
//!   * The OOM path MUST keep both `fprintf` to stderr and `exit(1)`:
//!     tests/unit/test_arena.c forks, over-allocates, and asserts
//!     `WEXITSTATUS(status) == 1`. A Zig `@panic` would abort with a
//!     different status and fail that test. Both are declared
//!     `extern` against libc rather than @cImport'd — `zig build-obj`
//!     does not link libc, so <stdio.h> and <stdlib.h> are not on the
//!     include path.
//!
//!   * `arena_alloc` returns `?*anyopaque` to match `void *`. It never
//!     actually returns null: the OOM branch exits the process, which
//!     is the C behaviour and what Principle-level callers assume.

const builtin = @import("builtin");

const HEAP_BYTES: usize = 131072;

/// 8-byte-aligned bump allocator over one static pool. Per the
/// Constitution's Memory model: no `free`, no `malloc`, no dynamic
/// resizing; OOM is a programmer error and exits the process.
var pool: [HEAP_BYTES]u8 align(64) = .{0} ** HEAP_BYTES;
var bump: usize = 0;

// From <stdio.h> / <stdlib.h>. Declared rather than @cInclude'd: see
// the module note. `fprintf` is variadic, which Zig expresses as
// `...` on an extern declaration.
extern fn fprintf(stream: *anyopaque, fmt: [*:0]const u8, ...) c_int;
extern fn exit(status: c_int) noreturn;

/// `stderr` is a linkable object on glibc but a MACRO on mingw-w64,
/// expanding to `__acrt_iob_func(2)`. A plain `extern const stderr`
/// builds for Linux and fails `make win` at link with
/// "undefined reference to `stderr'", so the stream needs a per-target
/// accessor. This is the only place in the port where a translation
/// differs by target.
const stderrStream = if (builtin.target.os.tag == .windows)
    struct {
        extern fn __acrt_iob_func(i: c_uint) *anyopaque;
        fn get() *anyopaque {
            return __acrt_iob_func(2);
        }
    }.get
else
    struct {
        extern const stderr: *anyopaque;
        fn get() *anyopaque {
            return stderr;
        }
    }.get;

export fn arena_alloc(n: usize) ?*anyopaque {
    const aligned = (n + 7) & ~@as(usize, 7);
    if (bump + aligned > HEAP_BYTES) {
        // Format string byte-identical to the C, including the %d on
        // HEAP_BYTES, so anything scraping stderr sees no change.
        _ = fprintf(stderrStream(), "arena: oom %zu+%zu > %d\n", bump, aligned, @as(c_int, HEAP_BYTES));
        exit(1);
    }
    const p: *anyopaque = @ptrCast(&pool[bump]);
    bump += aligned;
    return p;
}

export fn arena_used() usize {
    return bump;
}
