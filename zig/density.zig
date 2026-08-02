//! Adaptive density, ported from density.c.
//!
//! Exposes the exact density.h contract via `export fn`, so the
//! remaining C links against it unchanged (Principle V: the seam stays
//! the .h file).
//!
//! Two translation hazards handled explicitly:
//!
//!   * `tension` carries the SAME initializer the C had --
//!     DENSITY_TENSION_MID (128), not zero and not `undefined`. C
//!     guarantees zero-fill for uninitialized file-scope statics, but
//!     density.c:4 is `static uint8_t tension = DENSITY_TENSION_MID;`
//!     which is not zero. Getting this wrong would pass every gate the
//!     repo has -- gen_init() calls density_update() before any
//!     consequential read (see main.c:283-288) -- while silently
//!     changing the UI status row density_get_tension() feeds.
//!     Zig's `undefined` would be worse still: 0xAA-poisoned outside
//!     ReleaseSmall/ReleaseFast, and it moves the byte out of .bss.
//!
//!   * C's `/` on signed ints truncates toward zero. Zig rejects `/`
//!     on signed operands unless exactness is provable, so the bias
//!     divisions use @divTrunc to match C exactly. @divFloor would
//!     differ for negative numerators, which is the common case here
//!     (tension > 128).

const DENSITY_TENSION_MID: u8 = 128;

var tension: u8 = DENSITY_TENSION_MID;

/// popcount of the low 7 bits.
fn popcount7(v: u8) u8 {
    return @popCount(v & 0x7F);
}

export fn density_update(active_mask: u8, gate_prob: u8) void {
    // Tension = active-degree count weighted high + gate weighted
    // moderate. Widened to u16 before the multiply exactly as the C
    // does, so the clamp sees the same value.
    var t: u16 = @as(u16, popcount7(active_mask)) * 18 + @as(u16, gate_prob >> 2);
    if (t > 255) t = 255;
    tension = @intCast(t);
}

export fn density_get_tension() u8 {
    return tension;
}

export fn density_bias_gate() i8 {
    const delta = @divTrunc(@as(i32, DENSITY_TENSION_MID) - @as(i32, tension), 8);
    return @truncate(delta);
}

export fn density_bias_reverb() i8 {
    const delta = @divTrunc(@as(i32, DENSITY_TENSION_MID) - @as(i32, tension), 4);
    return @truncate(delta);
}
