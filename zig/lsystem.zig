//! Lindenmayer-system melodic phrase generator, ported from lsystem.c.
//!
//! lsystem.h is unchanged and remains the seam.
//!
//! Translation notes:
//!
//!   * `characters` and `axiom` are MUTABLE (lsystem_mutate rewrites
//!     them), so they are `var` and land in .data exactly as the C
//!     non-const statics did. `output_buf` has no C initializer, so it
//!     carries an explicit zero and stays in .bss.
//!
//!   * C partial array initializers zero-fill the tail. The comptime
//!     `rhs()` helper reproduces that exactly rather than relying on
//!     the reader to count trailing zeros.
//!
//!   * `scratch` was a FUNCTION-LOCAL static in C. Zig has no such
//!     thing, so it is a file-scope var; same storage, same lifetime,
//!     same .bss placement. It is written before it is read on every
//!     generation, so the initial value never matters.
//!
//!   * THE VARIABLE SHIFT (lsystem.c:180, named in the spec as a
//!     hazard). `rng >> (16 + 3*i)` has a computed shift count. i is
//!     bounded by new_len <= 5, so the count reaches 28 and stays
//!     under 32 -- legal in C and legal here. Zig needs the amount as
//!     a u5 for a u32, so the cast is explicit; in ReleaseSafe an
//!     out-of-range count would panic rather than being C's undefined
//!     behaviour, which is a strictly better failure if new_len is
//!     ever widened.
//!
//!   * All other shifts are `1 << deg` with deg wrapped into 0..6
//!     first, so every shift amount is provably in range.
//!
//!   * `pointer` is i8 and is re-anchored to a snapped 0..6 degree on
//!     every call, so the +/-2 moves cannot leave i8 range.

const SYM_UP: u8 = 1;
const SYM_UP2: u8 = 2;
const SYM_DN: u8 = 3;
const SYM_DN2: u8 = 4;
const SYM_REP: u8 = 5;
const SYM_REST: u8 = 6;
const N_SYMBOLS: u32 = 6; // SYM_UP..SYM_REST

const N_CHARACTERS: usize = 3;
const N_RULES: usize = 6;
const RULE_RHS_MAX: usize = 6;
const AXIOM_MAX: usize = 8;
const OUTPUT_BUF_LEN: u16 = 256;
const EXPAND_GENS: u32 = 3;

const LSYSTEM_REST: u8 = 0xFF;

/// One RHS per symbol; each RHS is up to RULE_RHS_MAX symbols,
/// NUL-terminated.
const Character = struct {
    rhs: [N_RULES][RULE_RHS_MAX + 1]u8,
};

/// Pad a rule to the full slot with NUL, reproducing C's partial
/// array initializer.
fn rhs(comptime syms: []const u8) [RULE_RHS_MAX + 1]u8 {
    var out: [RULE_RHS_MAX + 1]u8 = .{0} ** (RULE_RHS_MAX + 1);
    for (syms, 0..) |s, i| out[i] = s;
    return out;
}

/// Hand-tuned starter characters. Indexed as rhs[symbol - 1] since
/// symbols are 1-based.
var characters: [N_CHARACTERS]Character = .{
    // Character 0: stepwise. Lots of step movement, occasional rest,
    // leaps resolve back stepwise.
    .{ .rhs = .{
        rhs(&.{ SYM_UP, SYM_UP, SYM_DN }), // UP
        rhs(&.{ SYM_UP, SYM_DN }), // UP2
        rhs(&.{ SYM_DN, SYM_DN, SYM_UP }), // DN
        rhs(&.{ SYM_DN, SYM_UP }), // DN2
        rhs(&.{ SYM_UP, SYM_REP, SYM_DN }), // REP
        rhs(&.{SYM_REP}), // REST
    } },
    // Character 1: leaping. Leaps beget more leaps, with stepwise
    // resolutions.
    .{ .rhs = .{
        rhs(&.{ SYM_UP2, SYM_DN }), // UP
        rhs(&.{ SYM_UP2, SYM_UP, SYM_DN }), // UP2
        rhs(&.{ SYM_DN2, SYM_UP }), // DN
        rhs(&.{ SYM_DN2, SYM_DN, SYM_UP }), // DN2
        rhs(&.{ SYM_UP2, SYM_DN2 }), // REP
        rhs(&.{ SYM_REP, SYM_REST }), // REST
    } },
    // Character 2: sparse. Lots of rests interleaved with motion.
    .{ .rhs = .{
        rhs(&.{ SYM_UP, SYM_REST, SYM_DN }), // UP
        rhs(&.{ SYM_REST, SYM_UP, SYM_REST }), // UP2
        rhs(&.{ SYM_DN, SYM_REST, SYM_UP }), // DN
        rhs(&.{ SYM_REST, SYM_DN, SYM_REST }), // DN2
        rhs(&.{ SYM_REST, SYM_UP, SYM_REST }), // REP
        rhs(&.{ SYM_REST, SYM_REST }), // REST
    } },
};

var axiom: [AXIOM_MAX + 1]u8 = blk: {
    var a: [AXIOM_MAX + 1]u8 = .{0} ** (AXIOM_MAX + 1);
    a[0] = SYM_UP;
    a[1] = SYM_REST;
    a[2] = SYM_DN;
    a[3] = SYM_UP;
    break :blk a;
};

var output_buf: [OUTPUT_BUF_LEN]u8 = .{0} ** OUTPUT_BUF_LEN;
/// Was a function-local static in lsystem_reset; see the module note.
var scratch: [OUTPUT_BUF_LEN]u8 = .{0} ** OUTPUT_BUF_LEN;

var output_len: u16 = 0;
var pos: u16 = 0;
/// Current scale degree, 0..6.
var pointer: i8 = 0;
var cur_character: u8 = 0;

fn appendRhs(dst: []u8, dst_pos_in: u16, r: *const [RULE_RHS_MAX + 1]u8, dst_max: u16) u16 {
    var dst_pos = dst_pos_in;
    var i: usize = 0;
    while (r[i] != 0) : (i += 1) {
        if (dst_pos >= dst_max) return dst_pos;
        dst[dst_pos] = r[i];
        dst_pos += 1;
    }
    return dst_pos;
}

/// Expand the axiom EXPAND_GENS generations using the current
/// character. Capped at OUTPUT_BUF_LEN.
export fn lsystem_reset() void {
    // Generation 0: copy axiom into output_buf.
    var len: u16 = 0;
    var i: usize = 0;
    while (axiom[i] != 0 and len < OUTPUT_BUF_LEN) : (i += 1) {
        output_buf[len] = axiom[i];
        len += 1;
    }

    const c = &characters[cur_character % N_CHARACTERS];

    var g: u32 = 0;
    while (g < EXPAND_GENS) : (g += 1) {
        var scratch_pos: u16 = 0;
        var k: u16 = 0;
        while (k < len) : (k += 1) {
            const s = output_buf[k];
            if (s < SYM_UP or s > SYM_REST) {
                // unknown symbol; copy verbatim
                if (scratch_pos < OUTPUT_BUF_LEN) {
                    scratch[scratch_pos] = s;
                    scratch_pos += 1;
                }
                continue;
            }
            scratch_pos = appendRhs(&scratch, scratch_pos, &c.rhs[s - 1], OUTPUT_BUF_LEN);
            if (scratch_pos >= OUTPUT_BUF_LEN) break;
        }
        len = scratch_pos;
        @memcpy(output_buf[0..len], scratch[0..len]);
        if (len >= OUTPUT_BUF_LEN - RULE_RHS_MAX) break;
    }

    output_len = len;
    pos = 0;
    pointer = 0;
}

fn snapToMask(deg_in: i8, active_mask: u8) u8 {
    var deg = deg_in;
    // Wrap to [0, 6].
    while (deg < 0) deg += 7;
    while (deg > 6) deg -= 7;
    if (active_mask & (@as(u8, 1) << @intCast(deg)) != 0) return @intCast(deg);

    // Alternating outward search for the nearest in-mask degree.
    var off: i8 = 1;
    while (off <= 6) : (off += 1) {
        var up = deg + off;
        var dn = deg - off;
        while (up > 6) up -= 7;
        while (dn < 0) dn += 7;
        if (active_mask & (@as(u8, 1) << @intCast(up)) != 0) return @intCast(up);
        if (active_mask & (@as(u8, 1) << @intCast(dn)) != 0) return @intCast(dn);
    }
    // mask was zero (shouldn't happen; gen.c forces the 0x01 fallback)
    return 0;
}

export fn lsystem_next(active_mask: u8) u8 {
    if (output_len == 0) lsystem_reset();
    if (pos >= output_len) lsystem_reset();

    const s = output_buf[pos];
    pos += 1;

    if (s == SYM_REST) return LSYSTEM_REST;

    switch (s) {
        SYM_UP => pointer += 1,
        SYM_UP2 => pointer += 2,
        SYM_DN => pointer -= 1,
        SYM_DN2 => pointer -= 2,
        else => {}, // SYM_REP: no move; anything else: unknown
    }

    const snapped = snapToMask(pointer, active_mask);
    // Anchor the pointer to whatever degree we settled on.
    pointer = @intCast(snapped);
    return snapped;
}

/// ~50% chance to re-roll one rule's RHS, ~25% to cycle character,
/// ~25% to swap an axiom symbol. Calls lsystem_reset() so the next
/// phrase reflects the new rules without extra work in the caller.
export fn lsystem_mutate(rng: u32) void {
    const pick = (rng >> 24) & 3;

    if (pick <= 1) {
        const rule_idx: usize = @intCast((rng >> 4) % N_RULES);
        const c = &characters[cur_character % N_CHARACTERS];
        const r = &c.rhs[rule_idx];
        // New RHS length 2..5, from a different region of the rng.
        const new_len: u8 = @intCast(2 + ((rng >> 12) % 4));
        var i: u8 = 0;
        while (i < new_len) : (i += 1) {
            // Computed shift count, 16..28 -- see the module note.
            const sh: u5 = @intCast(16 + 3 * @as(u32, i));
            r[i] = @intCast(SYM_UP + ((rng >> sh) % N_SYMBOLS));
        }
        r[new_len] = 0;
    } else if (pick == 2) {
        cur_character = @intCast((@as(usize, cur_character) + 1) % N_CHARACTERS);
    } else {
        const i: usize = @intCast((rng >> 20) & 3); // axiom slot 0..3
        if (i < AXIOM_MAX) {
            axiom[i] = @intCast(SYM_UP + ((rng >> 28) % N_SYMBOLS));
        }
    }

    lsystem_reset();
}

export fn lsystem_set_character(idx: u8) void {
    const target: u8 = @intCast(idx % N_CHARACTERS);
    if (target == cur_character) return;
    cur_character = target;
    lsystem_reset();
}
