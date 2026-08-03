//! Long-term motif memory, ported from motif.c.
//!
//! Exposes the motif.h contract via `export fn`; motif.h is unchanged
//! and remains the seam.
//!
//! Translation notes, in the order the spec pins them
//! (specs/005-zig-port/spec.md FR-004..FR-007):
//!
//!   * `ring` carries an explicit all-zero initializer. The C is
//!     `static uint8_t ring[8][64]` with NO initializer, which C
//!     zero-fills and places in .bss. Zig's `undefined` would be
//!     0xAA-poisoned outside ReleaseSmall/ReleaseFast AND would move
//!     512 bytes from .bss into .data. Every other file-scope variable
//!     here matches its C initializer, all of which are 0.
//!
//!   * No `/` on signed operands, so no @divTrunc site.
//!
//!   * No `<<` anywhere. The two shifts are `rng >> 8` and `rng >> 16`,
//!     right shifts by constants well under 32, so the computed-shift-
//!     amount hazard that applies elsewhere does not arise here.
//!
//!   * Integer promotion, the one place it matters: C computes
//!     `(int)d + (int)replay_transpose` by promoting both to `int`.
//!     Zig does not promote, so the widening is explicit. i32 is used
//!     rather than i16 to match C's `int` exactly; the value is
//!     normalized into 0..6 before it narrows back to u8, so the
//!     @intCast cannot trap.
//!
//!   * `bars_since_replay` uses `+%=`. It is `uint32_t` in C, where
//!     overflow is DEFINED to wrap; a plain `+=` would panic in
//!     ReleaseSafe instead. This is faithfulness, not an expectation
//!     that it wraps — at ~2 s/bar the counter needs ~272 years.
//!     The u8 counters use plain `+=` because both are bounded by a
//!     guard on the following line, so a panic there would be a real
//!     bug surfacing in the sanitizer build rather than noise.
//!
//!   * `motif_in_replay` returns `c_int`, not `bool` or `u8`. The
//!     header declares `int` and gen.c uses it in a C conditional.

const MOTIF_PHRASE_BARS: u8 = 4;
const MOTIF_STEPS_PER_BAR: u8 = 16;
const MOTIF_PHRASE_SLOTS: usize = 64;
const MOTIF_RING_SIZE: u8 = 8;
const MOTIF_NO_NOTE: u8 = 0xFF;

/// A new replay can only start after this many bars since the last
/// one. With the per-bar chance below, average cadence lands near once
/// every 30-50 bars.
const MOTIF_REPLAY_MIN_GAP: u32 = 30;
/// 64/256 = 25% per bar, once the gap has elapsed.
const MOTIF_REPLAY_CHANCE_256: u32 = 64;

/// Ring of captured phrases. Each slot is a 64-element array of degrees
/// with MOTIF_NO_NOTE marking positions the gate suppressed.
var ring: [MOTIF_RING_SIZE][MOTIF_PHRASE_SLOTS]u8 =
    .{.{0} ** MOTIF_PHRASE_SLOTS} ** MOTIF_RING_SIZE;

// Capture state.
var capture_phrase_idx: u8 = 0;
var bar_in_phrase: u8 = 0;
var bars_since_replay: u32 = 0;

// Replay state.
var in_replay: u8 = 0;
var replay_phrase: u8 = 0;
var replay_transpose: i8 = 0;
var replay_bar: u8 = 0;

fn clearPhrase(idx: u8) void {
    @memset(&ring[idx], MOTIF_NO_NOTE);
}

export fn motif_init() void {
    var i: u8 = 0;
    while (i < MOTIF_RING_SIZE) : (i += 1) clearPhrase(i);
    capture_phrase_idx = 0;
    bar_in_phrase = 0;
    bars_since_replay = 0;
    in_replay = 0;
    replay_phrase = 0;
    replay_transpose = 0;
    replay_bar = 0;
}

export fn motif_record(step_in_bar: u8, degree: u8) void {
    if (in_replay != 0) return;
    if (step_in_bar >= MOTIF_STEPS_PER_BAR) return;
    if (bar_in_phrase >= MOTIF_PHRASE_BARS) return;
    // Both operands are bounded by the guards above: bar_in_phrase < 4
    // and step_in_bar < 16, so the product is at most 63 and u8
    // arithmetic cannot overflow.
    const slot: u8 = bar_in_phrase * MOTIF_STEPS_PER_BAR + step_in_bar;
    ring[capture_phrase_idx][slot] = degree;
}

export fn motif_replay_at(step_in_bar: u8) u8 {
    if (in_replay == 0) return MOTIF_NO_NOTE;
    if (step_in_bar >= MOTIF_STEPS_PER_BAR) return MOTIF_NO_NOTE;
    if (replay_bar >= MOTIF_PHRASE_BARS) return MOTIF_NO_NOTE;
    const slot: u8 = replay_bar * MOTIF_STEPS_PER_BAR + step_in_bar;
    const d = ring[replay_phrase][slot];
    if (d == MOTIF_NO_NOTE) return MOTIF_NO_NOTE;

    // Transposition wraps mod 7 (degrees wrap within the scale). C
    // promotes both operands to `int`; i32 matches that width exactly.
    var td: i32 = @as(i32, d) + @as(i32, replay_transpose);
    while (td < 0) td += 7;
    while (td >= 7) td -= 7;
    return @intCast(td);
}

export fn motif_in_replay() c_int {
    return in_replay;
}

/// Weighted transposition palette: verbatim 50%, +/-2 at 25% each. The
/// modest range keeps a replay recognizable as the original phrase.
fn pickTranspose(rng: u32) i8 {
    const r = rng & 3;
    if (r == 0 or r == 1) return 0;
    if (r == 2) return 2;
    return -2;
}

export fn motif_bar_step(bar: u32, rng: u32) void {
    _ = bar; // state is internally maintained; bar is informational

    if (in_replay != 0) {
        replay_bar += 1;
        if (replay_bar >= MOTIF_PHRASE_BARS) {
            // Replay complete - return to capture in a fresh phrase
            // slot, overwriting the oldest entry next time around.
            in_replay = 0;
            capture_phrase_idx = (capture_phrase_idx + 1) % MOTIF_RING_SIZE;
            clearPhrase(capture_phrase_idx);
            bar_in_phrase = 0;
            bars_since_replay = 0;
        }
        return;
    }

    // Capture mode: tick into the next bar of the current phrase.
    bar_in_phrase += 1;
    if (bar_in_phrase >= MOTIF_PHRASE_BARS) {
        capture_phrase_idx = (capture_phrase_idx + 1) % MOTIF_RING_SIZE;
        clearPhrase(capture_phrase_idx);
        bar_in_phrase = 0;
    }
    bars_since_replay +%= 1;

    // Replay-trigger gate.
    if (bars_since_replay >= MOTIF_REPLAY_MIN_GAP and
        (rng & 0xFF) < MOTIF_REPLAY_CHANCE_256)
    {
        var pick: u8 = @intCast((rng >> 8) % MOTIF_RING_SIZE);
        // Avoid replaying the currently-capturing slot: it may be
        // partially filled, or about to be overwritten.
        if (pick == capture_phrase_idx) {
            pick = (pick + MOTIF_RING_SIZE - 1) % MOTIF_RING_SIZE;
        }
        replay_phrase = pick;
        replay_transpose = pickTranspose(rng >> 16);
        replay_bar = 0;
        in_replay = 1;
    }
}
