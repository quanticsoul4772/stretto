//! Chord progression Markov walk, ported from chord_progression.c.
//!
//! Exposes the chord_progression.h contract via `export fn`.
//!
//! Translation notes:
//!   * Both weight tables are `const`, so they land in .rodata exactly
//!     as the C `static const` did.
//!   * `current_root` / `prev_root` carry explicit initializers
//!     matching the C (both 0). Zig's `undefined` would be
//!     0xAA-poisoned outside ReleaseSmall/ReleaseFast and would also
//!     move them out of .bss.
//!   * `rng % sum` and `current_root % CHORD_N_DEGREES` are unsigned
//!     in C, so plain `%` matches; no @rem/@mod distinction applies.
//!   * No `<<` appears here. Where one does (the xorshift PRNG in
//!     voice.c / gen.c), the translation is `<<%` -- C wraps by
//!     definition, and a Zig `<<` that discards a nonzero bit is
//!     illegal behavior, unchecked at ReleaseSmall.

// u8, not usize: `current_root % CHORD_N_DEGREES` must be a same-type
// binary op. The u8 result then coerces to usize for the array index,
// which is a widening Zig does implicitly.
const CHORD_N_DEGREES: u8 = 7;

const CHORD_MARKOV_MAJOR: [7][7]u8 = .{
    //       I   ii  iii IV  V   vi  vii
    .{ 2, 1, 1, 4, 4, 3, 1 }, // I
    .{ 3, 0, 0, 1, 5, 0, 0 }, // ii
    .{ 2, 0, 0, 2, 1, 4, 0 }, // iii
    .{ 4, 1, 0, 1, 4, 2, 0 }, // IV
    .{ 5, 0, 0, 1, 0, 3, 0 }, // V
    .{ 2, 4, 0, 4, 2, 0, 0 }, // vi
    .{ 4, 0, 0, 0, 2, 0, 0 }, // vii
};

const CHORD_MARKOV_MINOR: [7][7]u8 = .{
    //       i   iio III iv  v   VI  VII
    .{ 3, 1, 2, 3, 3, 2, 3 }, // i
    .{ 3, 0, 0, 0, 2, 0, 0 }, // iio
    .{ 3, 0, 0, 2, 1, 2, 1 }, // III
    .{ 3, 1, 0, 1, 2, 0, 2 }, // iv
    .{ 4, 0, 0, 1, 0, 1, 1 }, // v
    .{ 2, 0, 1, 3, 1, 0, 2 }, // VI
    .{ 4, 0, 1, 0, 1, 0, 0 }, // VII
};

var current_root: u8 = 0;
var prev_root: u8 = 0;

/// Lydian (1) and Mixolydian (5) get the major table.
fn selectTable(scale: u8) *const [7][7]u8 {
    if (scale == 1 or scale == 5) return &CHORD_MARKOV_MAJOR;
    return &CHORD_MARKOV_MINOR;
}

export fn chord_progression_init() void {
    current_root = 0;
    prev_root = 0;
}

export fn chord_progression_step(rng: u32, scale: u8) void {
    // Captured before walking so the bass scheduler can read the
    // direction of motion at the first bass event of the new chord.
    prev_root = current_root;

    const table = selectTable(scale);
    const row = &table[current_root % CHORD_N_DEGREES];

    var sum: u32 = 0;
    for (row) |w| sum += w;
    if (sum == 0) { // defensive: degenerate row
        current_root = 0;
        return;
    }

    const pick = rng % sum;
    var acc: u32 = 0;
    for (row, 0..) |w, i| {
        acc += w;
        if (pick < acc) {
            current_root = @intCast(i);
            return;
        }
    }
    // unreachable in practice; C falls through leaving current_root as-is
}

export fn chord_progression_get_root() u8 {
    return current_root;
}

export fn chord_progression_get_prev_root() u8 {
    return prev_root;
}
