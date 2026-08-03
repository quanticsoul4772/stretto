//! Song-section state machine, ported from section.c.
//!
//! Exposes the section.h contract via `export fn`; section.h is
//! unchanged and remains the seam.
//!
//! THE ONE CROSS-MODULE DEPENDENCY, AND WHY IT IS SAFE HERE
//!
//! section.c did `#include "voice.h"` for three enum values. A Zig
//! module cannot include a C header, so they are hand-declared below
//! -- exactly the silent-ABI-drift hazard Constitution Principle V
//! names as the cost of the `extern fn`/`export fn` exception.
//!
//! It is covered, and by a test that already existed rather than one
//! added to justify the port: tests/unit/test_section.c includes
//! ../../voice.h and asserts section_chord_voice_type() against
//! VOICE_WT, VOICE_ADD and VOICE_FM for all four sections
//! (test_section.c:80-85). If voice.h's enum is ever reordered, that
//! test fails immediately with the real values. The hand-declaration
//! cannot drift silently.
//!
//! Translation notes:
//!
//!   * `current_bar` carries an explicit 0, matching the C. Every
//!     table is `const`, so all land in .rodata as the C `static const`
//!     did.
//!
//!   * lerp_bias does signed `>>` on a value that IS negative in
//!     normal operation (BIAS_GATE[INTRO] is -64). C's right shift of
//!     a negative signed int is implementation-defined; gcc emits an
//!     arithmetic shift, and Zig's `>>` on a signed type is defined to
//!     be arithmetic. They agree on this target, which is what the
//!     golden hash checks.
//!
//!   * The narrowing to i8 uses @truncate, matching gcc's behaviour
//!     for an out-of-range `(int8_t)` cast. In practice the weighted
//!     average of two i8 values cannot leave i8 range, so the cast is
//!     exact either way.
//!
//!   * Division uses @divTrunc to match C's truncation. Both operands
//!     are positive at every call site, so the distinction does not
//!     bite here -- it is written this way so a future edit that makes
//!     a numerator negative does not silently change meaning.
//!
//!   * The VF_* masks are comptime shifts of 1, so every shift amount
//!     is known at compile time.

const SECTION_PERIOD_BARS: u32 = 96; // one full cycle
const SECTION_LEN_BARS: u32 = 24; // per section
const SECTION_FADE_BARS: u32 = 8; // 4 entering + 4 leaving
const SECTION_COUNT: usize = 4;

const SEC_INTRO: u8 = 0;

// Voice-family bitmask, mirroring section.h.
const VF_KICK: u8 = 1 << 0;
const VF_SNARE: u8 = 1 << 1;
const VF_HAT: u8 = 1 << 2;
const VF_BASS: u8 = 1 << 3;
const VF_CHORD: u8 = 1 << 4;
const VF_MELODY: u8 = 1 << 5;
const VF_COUNTER: u8 = 1 << 6;
const VF_ALL: u8 = 0x7F;

// From voice.h's enum:
//   VOICE_OFF=0, VOICE_KS=1, VOICE_FM=2, VOICE_DRUM=3,
//   VOICE_WT=4,  VOICE_ADD=5, VOICE_SUB=6
// Pinned by tests/unit/test_section.c:80-85 -- see the module comment.
const VOICE_FM: u8 = 2;
const VOICE_WT: u8 = 4;
const VOICE_ADD: u8 = 5;

// --- Per-section parameter biases -----------------------------------
// Continuous biases are signed deltas applied on top of the user's
// live-tunable parameter values.

/// gate_prob delta (-64 = sparser, +32 = denser)
const BIAS_GATE: [SECTION_COUNT]i8 = .{ -64, 0, 32, -16 };
/// SVF cutoff delta in cutoff units
const BIAS_CUTOFF: [SECTION_COUNT]i8 = .{ -40, 0, 30, -10 };
/// Reverb wet delta, applied on top of user wet
const BIAS_REVERB: [SECTION_COUNT]i8 = .{ 40, 0, -20, 20 };
/// Bars-until-mutate delta (positive = stiller, negative = busier)
const BIAS_MUT: [SECTION_COUNT]i8 = .{ 8, 0, -4, 4 };

/// Discrete: kick pattern index per section.
const KICK_PATTERN: [SECTION_COUNT]u8 = .{ 0, 0, 2, 0 };
/// Discrete: L-system character index per section.
/// INTRO -> sparse (2), BODY -> stepwise (0), TENSION -> leaping (1),
/// RESOLVE -> stepwise (0).
const LSYS_CHARACTER: [SECTION_COUNT]u8 = .{ 2, 0, 1, 0 };

const SECTION_NAMES: [SECTION_COUNT][*:0]const u8 = .{
    "intro", "body", "tens", "res",
};

/// Chord voice type per section. Each section gets a distinct chord
/// timbre; no section is silent.
///   INTRO   wavetable (animated pad, position swept by LFO)
///   BODY    additive  (organ-y Hammond pad, steady)
///   TENSION FM        (cutting glassy, percussive attack)
///   RESOLVE wavetable (returns to the animated pad)
const SECTION_CHORD_VOICE: [SECTION_COUNT]u8 = .{
    VOICE_WT, VOICE_ADD, VOICE_FM, VOICE_WT,
};

/// Chord arpeggio toggle per section. TENSION arpeggiates for energy;
/// the others play blocks. Discrete - switches instantly at the
/// boundary like the voice-type selection above.
const SECTION_CHORD_ARP: [SECTION_COUNT]u8 = .{ 0, 0, 1, 0 };

/// Per-section voice-family mask. INTRO's entry is a placeholder;
/// section_voice_mask() substitutes the live combo.
const SECTION_VOICE_MASK: [SECTION_COUNT]u8 = .{
    VF_ALL, // INTRO (overridden)
    VF_ALL, // BODY
    VF_ALL, // TENSION
    VF_ALL & ~@as(u8, VF_KICK | VF_SNARE | VF_HAT), // RESOLVE: no drums
};

/// Curated INTRO combos: 1-3 voice families each, all musically
/// coherent. gen.c picks one per 96-bar cycle via PRNG.
const INTRO_COMBOS: [8]u8 = .{
    VF_CHORD, // solo pad
    VF_CHORD | VF_COUNTER, // pad + counter line
    VF_CHORD | VF_HAT, // pad + tick
    VF_BASS | VF_CHORD, // deep pad foundation
    VF_BASS | VF_HAT, // pulse + tick
    VF_BASS | VF_COUNTER, // bass + sparse line
    VF_CHORD | VF_MELODY | VF_HAT, // melody-led trio
    VF_BASS | VF_CHORD | VF_HAT, // minimal full-stack trio
};

var current_bar: u32 = 0;
var intro_combo_idx: u8 = 0;

export fn section_init() void {
    current_bar = 0;
}

export fn section_step(bar: u32) void {
    current_bar = bar;
}

export fn section_current() u8 {
    const cycle_bar = current_bar % SECTION_PERIOD_BARS;
    return @intCast(cycle_bar / SECTION_LEN_BARS);
}

export fn section_name() [*:0]const u8 {
    return SECTION_NAMES[section_current()];
}

/// Linear interpolation for signed biases. The crossfade window is
/// centered on the section boundary: the last fade_half bars of a
/// section blend toward the next, and the first fade_half bars of the
/// next finish the blend. At the boundary exactly the bias is halfway
/// between adjacent sections. Outside the window the unblended value
/// is returned.
fn lerpBias(table: *const [SECTION_COUNT]i8) i8 {
    const fade_half: u32 = SECTION_FADE_BARS / 2;
    const cycle_bar = current_bar % SECTION_PERIOD_BARS;
    const section = cycle_bar / SECTION_LEN_BARS;
    const pos = cycle_bar % SECTION_LEN_BARS;

    const prev_sec: usize = @intCast((section + SECTION_COUNT - 1) % SECTION_COUNT);
    const next_sec: usize = @intCast((section + 1) % SECTION_COUNT);
    const cur: i32 = table[@intCast(section)];

    // Denominator of both blends: the full crossfade width. Comptime-
    // known and nonzero.
    const span: i32 = @intCast(2 * fade_half);

    if (pos < fade_half) {
        // Entering: blend prev -> cur, half complete at pos=0 (w=128),
        // reaching full cur at pos=fade_half (w=256).
        const w: i32 = @divTrunc(@as(i32, @intCast(pos + fade_half)) * 256, span);
        return @truncate((@as(i32, table[prev_sec]) * (256 - w) + cur * w) >> 8);
    }
    if (pos >= SECTION_LEN_BARS - fade_half) {
        // Leaving: blend cur -> next, w=0 at the start of the leave
        // window, w=128 at the boundary (pos = SECTION_LEN_BARS - 1).
        const k: i32 = @intCast(pos - (SECTION_LEN_BARS - fade_half));
        const w: i32 = @divTrunc(k * 256, span);
        return @truncate((cur * (256 - w) + @as(i32, table[next_sec]) * w) >> 8);
    }
    return @truncate(cur);
}

export fn section_bias_gate() i8 {
    return lerpBias(&BIAS_GATE);
}
export fn section_bias_cutoff() i8 {
    return lerpBias(&BIAS_CUTOFF);
}
export fn section_bias_reverb() i8 {
    return lerpBias(&BIAS_REVERB);
}
export fn section_bias_mutation_interval() i8 {
    return lerpBias(&BIAS_MUT);
}

export fn section_kick_pattern() u8 {
    return KICK_PATTERN[section_current()];
}
export fn section_lsystem_character() u8 {
    return LSYS_CHARACTER[section_current()];
}
export fn section_chord_voice_type() u8 {
    return SECTION_CHORD_VOICE[section_current()];
}
export fn section_chord_arpeggio() u8 {
    return SECTION_CHORD_ARP[section_current()];
}

export fn section_set_intro_combo(idx: u8) void {
    intro_combo_idx = idx & 7;
}

export fn section_voice_mask() u8 {
    const sec = section_current();
    if (sec == SEC_INTRO) return INTRO_COMBOS[intro_combo_idx];
    return SECTION_VOICE_MASK[sec];
}
