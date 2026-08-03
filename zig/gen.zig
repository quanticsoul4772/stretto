//! Generative scheduler, ported from gen.c. The last module of the
//! 005 port.
//!
//! gen.h is unchanged and remains the seam.
//!
//! This is the LTO hub: gen.c called into voice, lsystem,
//! chord_progression, section, density, motif and effects, and every
//! one of those is now Zig. The calls go out through @cImport'd
//! prototypes — the same C ABI they always used, since gcc still links
//! every target.
//!
//! Translation notes:
//!
//!   * Shifts by a computed amount are the real hazard in this file,
//!     not shifts that discard bits (see the FR-006 correction in
//!     spec.md). `ca_step` shifts a u32 by `i` and by `(i+1)&31`;
//!     `schedule_drums` shifts a u64 by a substep index; the Euclidean
//!     tests shift a u16 by `15 - step_in_bar`. Each is cast to the
//!     exact Log2Int width so an out-of-range amount is a compile-time
//!     type error rather than C's undefined behaviour.
//!
//!   * The xorshift PRNG uses plain `<<`. Zig's `<<` on an unsigned
//!     type discards high bits exactly as C's does.
//!
//!   * `gen_step`'s tick guard is `(int32_t)(sample_clock - next_fire)
//!     >= 0` — a SIGNED comparison of an unsigned difference, which is
//!     what survives the ~24.8 h u32 wrap. Translated as an explicit
//!     wrapping subtract reinterpreted through @bitCast; a plain
//!     comparison would burst thousands of ticks at the wrap.
//!
//!   * Every `int` in gen.h is `c_int` here — main.c's uniform flag
//!     table passes these directly.

const c = @cImport({
    @cInclude("voice.h");
    @cInclude("config.h");
    @cInclude("euclid_table.h");
    @cInclude("lsystem.h");
    @cInclude("chord_progression.h");
    @cInclude("section.h");
    @cInclude("density.h");
    @cInclude("motif.h");
    @cInclude("effects.h");
});

/// From <time.h>. Declared here rather than @cInclude'd: `zig
/// build-obj` does not link libc, so system headers are not on the
/// include path, and adding -lc to the rule would change every ported
/// module's object. time_t is i64 on x86_64 Linux.
extern fn time(tloc: ?*i64) i64;

// Constants derived from the headers rather than restated, so a
// reordering there cannot silently disagree with this file. @cImport
// gives them C's `int` type; the callee signatures take u8.
const VOICE_KS: u8 = @intCast(c.VOICE_KS);
const VOICE_FM: u8 = @intCast(c.VOICE_FM);
const VOICE_SUB: u8 = @intCast(c.VOICE_SUB);
const ROLE_BASS: u8 = @intCast(c.ROLE_BASS);
const ROLE_CHORD: u8 = @intCast(c.ROLE_CHORD);
const ROLE_MELODY: u8 = @intCast(c.ROLE_MELODY);
const DRUM_KICK: u8 = @intCast(c.DRUM_KICK);
const DRUM_SNARE: u8 = @intCast(c.DRUM_SNARE);
const DRUM_HIHAT: u8 = @intCast(c.DRUM_HIHAT);
const VF_KICK: u8 = @intCast(c.VF_KICK);
const VF_SNARE: u8 = @intCast(c.VF_SNARE);
const VF_HAT: u8 = @intCast(c.VF_HAT);
const VF_BASS: u8 = @intCast(c.VF_BASS);
const VF_CHORD: u8 = @intCast(c.VF_CHORD);
const VF_MELODY: u8 = @intCast(c.VF_MELODY);
const VF_COUNTER: u8 = @intCast(c.VF_COUNTER);
const LSYSTEM_REST: u8 = @intCast(c.LSYSTEM_REST);
const MOTIF_NO_NOTE: u8 = @intCast(c.MOTIF_NO_NOTE);
const SECTION_PERIOD_BARS: u32 = @intCast(c.SECTION_PERIOD_BARS);
const SAMPLE_RATE: i64 = @intCast(c.SAMPLE_RATE);

/// 48 = LCM(3, 4, 16): bass fires at 3 evenly-spaced positions,
/// chord at 4, melody on a 16-step Euclidean grid. All three lock at
/// substep 0 of each bar, then diverge — the 3:4 cross-rhythm is heard
/// as the bass and chord events interlock.
const BAR_SUBSTEPS: u32 = 48;
/// one melody slot every 3 substeps
const MELODY_SUBSTRIDE: u32 = 3;

const MUTATE_MIN: u32 = 1;
const MUTATE_MAX: u32 = 16;
const MUTATE_DEFAULT: u8 = 4;
/// 65536 / 512 = 128-bar period
const MUTATE_LFO_INC: u16 = 512;

/// 48 substeps * 2000 samples = 96000 = 2.00 s per bar at 48 kHz.
const DEFAULT_SAMPLES_PER_SUBSTEP: u32 = 2000;
var samples_per_substep: u32 = DEFAULT_SAMPLES_PER_SUBSTEP;

/// 0 = straight (byte-inert default); the precomputed fire sample for
/// the upcoming substep. Semantics at gen_step.
var swing_amount: u32 = 0;
var next_fire: u32 = 0;

var gen_prng_state: u32 = 0xDEADBEEF;

fn prng() u32 {
    var x = gen_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gen_prng_state = x;
    return x;
}

/// Six modes rooted on D, all 7 degrees in one octave from D4 (MIDI
/// 62). Markov runs on degree indices so the same matrix works for any
/// 7-note scale; only the degree-to-MIDI mapping changes.
const N_SCALES: usize = 6;
const SCALES = [N_SCALES][7]u8{
    .{ 62, 64, 65, 67, 69, 71, 72 }, // Dorian
    .{ 62, 64, 66, 68, 69, 71, 73 }, // Lydian
    .{ 62, 63, 65, 67, 69, 70, 72 }, // Phrygian
    .{ 62, 63, 65, 67, 68, 70, 72 }, // Locrian
    .{ 62, 64, 65, 67, 69, 70, 73 }, // Harmonic Minor
    .{ 62, 64, 66, 67, 69, 71, 72 }, // Mixolydian
};
var cur_scale: u8 = 0;

const ChordNote = struct { degree: i8, octave: i8 };

const N_CHORD_PATTERNS: u32 = 6;
const CHORD_PATTERNS = [N_CHORD_PATTERNS][3]ChordNote{
    .{ .{ .degree = 0, .octave = 0 }, .{ .degree = 2, .octave = 0 }, .{ .degree = 4, .octave = 0 } }, // triad
    .{ .{ .degree = 0, .octave = 0 }, .{ .degree = 2, .octave = 0 }, .{ .degree = 6, .octave = 0 } }, // seventh
    .{ .{ .degree = 0, .octave = 0 }, .{ .degree = 3, .octave = 0 }, .{ .degree = 4, .octave = 0 } }, // sus4
    .{ .{ .degree = 0, .octave = 0 }, .{ .degree = 1, .octave = 0 }, .{ .degree = 4, .octave = 0 } }, // sus2
    .{ .{ .degree = 2, .octave = 0 }, .{ .degree = 4, .octave = 0 }, .{ .degree = 0, .octave = 1 } }, // inv1
    .{ .{ .degree = 4, .octave = 0 }, .{ .degree = 0, .octave = 1 }, .{ .degree = 2, .octave = 1 } }, // inv2
};

/// 1st-order seed for D Dorian, replicated across the prev_prev axis
/// at gen_init. Stepwise motion gets moderate weight; leading tone
/// (6) and dominant (4) bias toward tonic; no self-transitions.
const MARKOV_SEED = [7][7]u8{
    .{ 0, 4, 3, 2, 4, 1, 2 },
    .{ 5, 0, 3, 2, 1, 1, 0 },
    .{ 3, 4, 0, 4, 2, 1, 0 },
    .{ 2, 2, 3, 0, 4, 2, 1 },
    .{ 5, 1, 2, 3, 0, 3, 2 },
    .{ 1, 2, 1, 2, 3, 0, 3 },
    .{ 5, 1, 0, 1, 2, 2, 0 },
};

/// Runtime 2nd-order table, [prev_prev][prev][next]. The extra context
/// lets stepwise motifs and cadential figures persist longer.
var markov2: [7][7][7]u8 = .{.{.{0} ** 7} ** 7} ** 7;

var mutate_lfo_phase: u16 = 0;
var bars_until_mutate: u8 = MUTATE_DEFAULT;
var cur_degree: u8 = 0;
/// start on the dominant for harmonic interest
var cur_degree_counter: u8 = 4;
/// 2nd-order context: degree before cur
var cur_degree_counter_prev: u8 = 0;
var eucl_k_counter: u8 = 4;
/// Voice-leading anchor: rough pitch center of the previous chord
/// trigger, anchored mid-range to prevent long-term drift.
var prev_chord_center: u8 = 67;
var ca_row: u32 = 0x12345678;
var ca_harm: u32 = 0x87654321;
var eucl_k_a: u8 = 3;
var eucl_k_b: u8 = 5;
var bar_count: u32 = 0;
var substep_count: u32 = 0;
var sample_clock: u32 = 0;
var next_step: u32 = 0;
/// A Euclidean step only fires if (prng() % 256) < gate_prob.
var gate_prob: u8 = 200;

/// Rule 110 (class IV) supplies recurring structure; Rule 30 (class
/// III) supplies variation that stops it repeating verbatim.
const RULE_110: u8 = 0x6E;
const RULE_30: u8 = 0x1E;

fn caStep(row: u32, rule: u8) u32 {
    var next: u32 = 0;
    var i: u5 = 0;
    while (true) : (i += 1) {
        const left: u32 = (row >> @intCast((@as(u32, i) + 1) & 31)) & 1;
        const center: u32 = (row >> i) & 1;
        const right: u32 = (row >> @intCast((@as(u32, i) + 31) & 31)) & 1;
        const p: u3 = @intCast((left << 2) | (center << 1) | right);
        if ((rule >> p) & 1 != 0) next |= (@as(u32, 1) << i);
        if (i == 31) break;
    }
    return next;
}

/// Snap an arbitrary degree to the nearest in-mask degree. Motif
/// replay needs it: the captured phrase's degrees may not match the
/// current bar's mask, since the CA has moved on since capture.
fn snapToActiveMask(deg: u8, active_mask: u8) u8 {
    if (active_mask & (@as(u8, 1) << @intCast(deg)) != 0) return deg;
    var off: u8 = 1;
    while (off <= 6) : (off += 1) {
        const up: u8 = (deg + off) % 7;
        const dn: u8 = (deg + 7 - off) % 7;
        if (active_mask & (@as(u8, 1) << @intCast(up)) != 0) return up;
        if (active_mask & (@as(u8, 1) << @intCast(dn)) != 0) return dn;
    }
    return 0;
}

fn markov2Init() void {
    var pp: usize = 0;
    while (pp < 7) : (pp += 1) {
        var p: usize = 0;
        while (p < 7) : (p += 1) {
            var n: usize = 0;
            while (n < 7) : (n += 1) markov2[pp][p][n] = MARKOV_SEED[p][n];
        }
    }
}

/// 2nd-order walk with interval bias against the main melody's most
/// recent degree. Degree-distance, not semitone-distance: the counter
/// is pitched +12, so same-degree is pitch-class unison in any mode.
///   0 (unison)  x0    avoid landing on the main's note
///   1 (2nd/7th) x64   mild dissonance
///   2 (3rd/6th) x192  preferred consonance
///   3 (4th/5th) x128  neutral
/// If the row sums to zero after bias, falls back to prev unchanged —
/// same safety net as the unbiased walk, no extra prng() draws.
fn markov2NextVoicedVoiced(prev_prev: u8, prev: u8, active_mask: u8, main_deg: u8) u8 {
    const bias_by_interval = [4]u16{ 0, 64, 192, 128 };
    const row = &markov2[prev_prev % 7][prev % 7];
    var weights: [7]u16 = .{0} ** 7;
    var sum: u16 = 0;
    var i: usize = 0;
    while (i < 7) : (i += 1) {
        if (active_mask & (@as(u8, 1) << @intCast(i)) == 0) {
            weights[i] = 0;
            continue;
        }
        var interval: i32 = @as(i32, @intCast(i)) - @as(i32, main_deg);
        if (interval < 0) interval = -interval;
        if (interval > 3) interval = 7 - interval;
        weights[i] = (@as(u16, row[i]) * bias_by_interval[@intCast(interval)]) >> 8;
        sum += weights[i];
    }
    if (sum == 0) return prev;
    var pick: u16 = @intCast(prng() % sum);
    i = 0;
    while (i < 7) : (i += 1) {
        if (pick < weights[i]) return @intCast(i);
        pick -= weights[i];
    }
    return prev;
}

fn mutate() void {
    const r = prng();
    // Drift one cell of the 2nd-order table, so distinct
    // (prev_prev, prev) contexts evolve different tendencies.
    const pp: usize = @intCast((r >> 24) % 7);
    const pv: usize = @intCast((r >> 0) % 7);
    const to: usize = @intCast((r >> 4) % 7);
    markov2[pp][pv][to] = @intCast((r >> 8) & 0x0F);

    const bit: u5 = @intCast((r >> 12) & 31);
    ca_row ^= (@as(u32, 1) << bit);
    if (ca_row == 0) ca_row = 0x12345678;

    if ((bar_count >> 4) & 1 != 0) {
        eucl_k_a = @intCast(1 + ((r >> 17) % 7));
    } else {
        eucl_k_b = @intCast(2 + ((r >> 17) % 7));
    }

    // ~25% chance per mutate: drift gate_prob +/-16, clamped to the
    // musical range [64, 240] (25% .. 94%).
    if (((r >> 24) & 3) == 0) {
        const delta: i32 = @as(i32, @intCast((r >> 20) & 0x1F)) - 16;
        var p: i32 = @as(i32, gate_prob) + delta;
        if (p < 64) p = 64;
        if (p > 240) p = 240;
        gate_prob = @intCast(p);
    }

    // Re-roll the counter-melody Euclidean k so its pattern shifts
    // independently of the main melody.
    if (((r >> 28) & 1) == 0) {
        eucl_k_counter = @intCast(2 + ((r >> 18) % 7));
    }

    if (((r >> 30) & 1) == 0) {
        c.voice_mutate_filter(prng());
    }

    // ~1 in 3 mutate events drift the L-system grammar.
    if (((r >> 26) & 3) == 0) {
        c.lsystem_mutate(prng());
    }
}

/// Triangle LFO between MUTATE_MIN (busy) and MUTATE_MAX (calm) bars.
/// Period is 128 bars, ~4.3 min at default tempo.
fn dynamicMutateInterval() u8 {
    const tri: u32 = if (mutate_lfo_phase < 32768)
        mutate_lfo_phase
    else
        @as(u32, 65535) - mutate_lfo_phase;
    var v: u32 = MUTATE_MIN + @divTrunc(tri * (MUTATE_MAX - MUTATE_MIN + 1), 32768);
    if (v < MUTATE_MIN) v = MUTATE_MIN;
    if (v > MUTATE_MAX) v = MUTATE_MAX;
    return @intCast(v);
}

/// Dynamic LFO value + section bias, clamped to [1, 32]. Single source
/// of truth so new bias sources extend this rather than the call site.
fn effectiveMutateInterval() u8 {
    var v: i32 = @as(i32, dynamicMutateInterval()) +
        @as(i32, c.section_bias_mutation_interval());
    if (v < 1) v = 1;
    if (v > 32) v = 32;
    return @intCast(v);
}

var gen_seeded_explicitly: bool = false;
var seed_input: u32 = 0;

/// xorshift32, so PRNG / ca_row / ca_harm all start from independent
/// points. gen_seed XORs the input with a constant first so seed=0
/// maps somewhere meaningful and misses xorshift's zero fixed point.
fn hash32(x_in: u32) u32 {
    var x = x_in;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

export fn gen_seed(seed: u32) void {
    var s = hash32(seed ^ 0xDEADBEEF);
    if (s == 0) s = 0x12345678;
    gen_prng_state = s;
    s = hash32(s);
    ca_row = if (s != 0) s else 0x12345678;
    s = hash32(s);
    ca_harm = if (s != 0) s else 0x87654321;
    gen_seeded_explicitly = true;
    seed_input = seed;
}

export fn gen_get_seed_input() u32 {
    return seed_input;
}

export fn gen_init() void {
    cur_degree = 0;
    cur_degree_counter = 4;
    cur_degree_counter_prev = 0;
    markov2Init();
    eucl_k_counter = 4;
    prev_chord_center = 67;
    cur_scale = 0;
    eucl_k_a = 3;
    eucl_k_b = 5;
    bar_count = 0;
    substep_count = 0;
    sample_clock = 0;
    next_step = 0;
    // A stale value from a prior run would delay (pre-069: deadlock)
    // the first tick of repeated-init tests.
    next_fire = 0;
    swing_amount = 0;
    samples_per_substep = DEFAULT_SAMPLES_PER_SUBSTEP;
    gate_prob = 200;
    mutate_lfo_phase = 0;
    bars_until_mutate = MUTATE_DEFAULT;

    // Unseeded callers derive from the wall clock so each launch is a
    // different generative output.
    if (!gen_seeded_explicitly) {
        gen_seed(@truncate(@as(u64, @bitCast(time(null)))));
    }

    c.lsystem_reset();
    c.motif_init();
    c.chord_progression_init();
    c.section_init();
    // The cycle-wrap draw in schedule_bar_boundary only fires at bar
    // 96/192/..., so the very first INTRO needs its combo seeded here.
    c.section_set_intro_combo(@intCast(prng() & 7));
    c.voice_set_cutoff_bias(c.section_bias_cutoff());
    c.lsystem_set_character(c.section_lsystem_character());

    // Prime tension conservatively; the first bar boundary recomputes
    // from the live active mask + gate.
    c.density_update(0x01, gate_prob);
    c.reverb_set_wet_bias(c.section_bias_reverb() + c.density_bias_reverb());
}

/// Advance the CA, fire scheduled mutations, advance the chord Markov
/// (every 2 bars), step the section LFO and push its biases, recompute
/// density. Once per bar, at substep 0.
fn scheduleBarBoundary() void {
    ca_row = caStep(ca_row, RULE_110);
    if (ca_row == 0) ca_row = 0x12345678;
    bar_count +%= 1;
    mutate_lfo_phase +%= MUTATE_LFO_INC;
    bars_until_mutate -= 1;
    if (bars_until_mutate == 0) {
        mutate();
        bars_until_mutate = effectiveMutateInterval();
    }
    if ((bar_count % 2) == 0) {
        c.chord_progression_step(prng(), cur_scale);
    }
    c.section_step(bar_count);
    // Each cycle boundary (entry into INTRO) picks a fresh sparse
    // combo, so every intro is a different minimal palette.
    if ((bar_count % SECTION_PERIOD_BARS) == 0) {
        c.section_set_intro_combo(@intCast(prng() & 7));
    }
    c.voice_set_cutoff_bias(c.section_bias_cutoff());
    c.lsystem_set_character(c.section_lsystem_character());
    // ca_harm has not advanced for this bar yet (that runs after this
    // block in gen_step), so density uses ca_row alone as the
    // bar-stable active-degree measure.
    c.density_update(@intCast(ca_row & 0x7F), gate_prob);
    c.reverb_set_wet_bias(c.section_bias_reverb() + c.density_bias_reverb());

    // One prng() draw decides entering or exiting replay.
    c.motif_bar_step(bar_count, prng());
}

/// Bar-scale ca_row (Rule 110) combined with the faster ca_harm
/// (Rule 30, every 12 substeps), forced non-empty. (harm | 0x11) keeps
/// degrees 0 and 4 available so the active set never starves the chord
/// progression.
fn computeActiveMask() u8 {
    const am: u8 = @intCast(ca_row & 0x7F);
    const hm: u8 = @intCast((ca_harm >> 8) & 0x7F);
    const m = am & (hm | 0x11);
    return if (m != 0) m else 0x01;
}

fn bit64(n: u6) u64 {
    return @as(u64, 1) << n;
}

/// Each drum has its own rotating bank. Bit N means "trigger at
/// substep N". Coprime bank sizes (4, 3, 5) make the kit cycle through
/// LCM = 60 bars before repeating exactly. Kick is pinned by section;
/// snare and hihat advance per bar.
fn scheduleDrums(substep_in_bar: u32) void {
    const kick_patterns = [4]u64{
        bit64(0) | bit64(24), // basic 1+3
        bit64(0) | bit64(24) | bit64(30), // syncopated 1+3+
        bit64(0) | bit64(12) | bit64(24) | bit64(36), // four-on-the-floor
        bit64(0) | bit64(18) | bit64(24), // off-kilter w/ 2+
    };
    const snare_patterns = [3]u64{
        bit64(12) | bit64(36), // classic 2+4
        bit64(9) | bit64(12) | bit64(33) | bit64(36), // with ghost 2e 4e
        bit64(24), // half-time 3 only
    };
    const hihat_patterns = [5]u64{
        bit64(0) | bit64(6) | bit64(12) | bit64(18) | bit64(24) | bit64(30) | bit64(36) | bit64(42),
        bit64(0) | bit64(3) | bit64(6) | bit64(9) | bit64(12) | bit64(15) | bit64(18) | bit64(21) |
            bit64(24) | bit64(27) | bit64(30) | bit64(33) | bit64(36) | bit64(39) | bit64(42) | bit64(45),
        bit64(0) | bit64(12) | bit64(24) | bit64(36), // quarters
        bit64(6) | bit64(18) | bit64(30) | bit64(42), // offbeats
        bit64(0) | bit64(8) | bit64(16) | bit64(24) | bit64(32) | bit64(40), // triplet feel
    };
    const vm = c.section_voice_mask();
    const kbits = kick_patterns[c.section_kick_pattern() % 4];
    const sbits = snare_patterns[bar_count % 3];
    const hbits = hihat_patterns[bar_count % 5];
    const sh: u6 = @intCast(substep_in_bar);
    if ((vm & VF_KICK) != 0 and ((kbits >> sh) & 1) != 0) c.voice_pool_trigger_drum(DRUM_KICK);
    if ((vm & VF_SNARE) != 0 and ((sbits >> sh) & 1) != 0) c.voice_pool_trigger_drum(DRUM_SNARE);
    if ((vm & VF_HAT) != 0 and ((hbits >> sh) & 1) != 0) c.voice_pool_trigger_drum(DRUM_HIHAT);
}

/// 4 events per bar at unequal spacing. Beats 1 and 3 (substeps 0, 24)
/// anchor the tempo; offbeats at 18 and 42 anticipate beat 3 and the
/// next bar 1 — a dub/reggae groove. Pitch alternates root/fifth, and
/// bar parity swaps the order so consecutive bars differ.
fn scheduleBass(substep_in_bar: u32) void {
    if ((c.section_voice_mask() & VF_BASS) == 0) return;
    const bass_substeps = [4]u32{ 0, 18, 24, 42 };
    const bass_deg_a = [4]u8{ 0, 4, 0, 4 };
    const bass_deg_b = [4]u8{ 4, 0, 4, 0 };
    var i: usize = 0;
    while (i < 4) : (i += 1) {
        if (substep_in_bar != bass_substeps[i]) continue;
        const degs = if ((bar_count & 1) != 0) &bass_deg_b else &bass_deg_a;
        const chord_root = c.chord_progression_get_root();
        var deg: u8 = (chord_root + degs[i]) % 7;

        // At the first bass event of a new chord, play a one-step
        // diatonic approach in the direction of the previous root
        // instead of jumping to the new one. The next event (substep
        // 18) plays the actual root: approach -> resolution.
        if (substep_in_bar == 0 and (bar_count % 2) == 0) {
            const prev = c.chord_progression_get_prev_root();
            if (prev != chord_root) {
                var delta: i32 = @as(i32, prev) - @as(i32, chord_root);
                while (delta > 3) delta -= 7;
                while (delta < -3) delta += 7;
                if (delta > 0) {
                    deg = (chord_root + 1) % 7;
                } else if (delta < 0) {
                    deg = (chord_root + 6) % 7;
                }
            }
        }

        const bass_note: u8 = SCALES[cur_scale][deg] - 12;
        c.voice_pool_trigger_role(bass_note, VOICE_SUB, ROLE_BASS);
        return;
    }
}

/// Block mode: 4 evenly-spaced events per bar (substeps 0, 12, 24, 36)
/// — the "4" of the 3-against-4 polyrhythm, all 3 voicing notes at
/// once. Arpeggio mode (TENSION only): 8 events per bar, one voicing
/// note each, cycled up. Both rotate voicing per bar, rebase onto the
/// current chord function, and octave-shift toward the previous
/// chord's centroid for voice leading.
fn scheduleChord(substep_in_bar: u32, active_mask: u8) void {
    if ((c.section_voice_mask() & VF_CHORD) == 0) return;
    const arp = c.section_chord_arpeggio();
    if (arp != 0) {
        if (substep_in_bar % 6 != 0) return;
    } else {
        if (substep_in_bar != 0 and substep_in_bar != 12 and
            substep_in_bar != 24 and substep_in_bar != 36) return;
    }

    const pat = &CHORD_PATTERNS[bar_count % N_CHORD_PATTERNS];
    const chord_root = c.chord_progression_get_root();
    const voice_type = c.section_chord_voice_type();

    if (arp != 0) {
        // Cycle the voicing-note index across bars so the arp keeps
        // moving: 8 steps/bar against 3 notes phases against the bar.
        const arp_step: u32 = bar_count *% 8 + substep_in_bar / 6;
        const i: usize = @intCast(arp_step % 3);
        const d: u8 = @intCast((@as(i32, pat[i].degree) + @as(i32, chord_root)) & 0xFF);
        const dd: u8 = d % 7;
        if ((active_mask & (@as(u8, 1) << @intCast(dd))) == 0) return;
        var note: i32 = @as(i32, SCALES[cur_scale][dd]) + @as(i32, pat[i].octave) * 12;
        while (note > @as(i32, prev_chord_center) + 8) note -= 12;
        while (note < @as(i32, prev_chord_center) - 8) note += 12;
        if (note < 24) note += 12;
        if (note > 96) note -= 12;
        c.voice_pool_trigger_role(@intCast(note), voice_type, ROLE_CHORD);
        // Single-note centroid update, same 3:1 smoothing as block.
        prev_chord_center = @intCast((@as(u16, prev_chord_center) * 3 +
            @as(u16, @intCast(note))) >> 2);
        if (prev_chord_center < 55) prev_chord_center = 55;
        if (prev_chord_center > 79) prev_chord_center = 79;
        return;
    }

    var sum: u16 = 0;
    var count: u8 = 0;
    var i: usize = 0;
    while (i < 3) : (i += 1) {
        const d: u8 = @intCast((@as(i32, pat[i].degree) + @as(i32, chord_root)) & 0xFF);
        const dd: u8 = d % 7;
        if ((active_mask & (@as(u8, 1) << @intCast(dd))) == 0) continue;
        var note: i32 = @as(i32, SCALES[cur_scale][dd]) + @as(i32, pat[i].octave) * 12;
        while (note > @as(i32, prev_chord_center) + 8) note -= 12;
        while (note < @as(i32, prev_chord_center) - 8) note += 12;
        if (note < 24) note += 12;
        if (note > 96) note -= 12;
        c.voice_pool_trigger_role(@intCast(note), voice_type, ROLE_CHORD);
        sum += @as(u16, @intCast(note));
        count += 1;
    }
    if (count > 0) {
        const new_center: u8 = @intCast(@divTrunc(sum, @as(u16, count)));
        prev_chord_center = @intCast((@as(u16, prev_chord_center) * 3 +
            @as(u16, new_center)) >> 2);
        if (prev_chord_center < 55) prev_chord_center = 55;
        if (prev_chord_center > 79) prev_chord_center = 79;
    }
}

/// Main + counter melody, on Euclidean grid substeps only. Main is
/// L-system phrased, counter is Markov walked, both pitched into the
/// active mask and probability-gated by user + section + density.
fn scheduleMelody(substep_in_bar: u32, active_mask: u8) void {
    if (substep_in_bar % MELODY_SUBSTRIDE != 0) return;
    const step_in_bar: u32 = substep_in_bar / MELODY_SUBSTRIDE;
    // The voice-family gate applies ONLY to the trigger calls, not to
    // the PRNG draws / L-system / Markov / motif updates. A muted
    // section silences the line; it does not rewrite the composition.
    const vm = c.section_voice_mask();

    var eff_gate_i: i32 = @as(i32, gate_prob) +
        @as(i32, c.section_bias_gate()) +
        @as(i32, c.density_bias_gate());
    if (eff_gate_i < 32) eff_gate_i = 32;
    if (eff_gate_i > 255) eff_gate_i = 255;
    const eff_gate: u32 = @intCast(eff_gate_i);

    const hits: u16 = c.euclid_table[eucl_k_a] | c.euclid_table[eucl_k_b];
    const sh: u4 = @intCast(15 - step_in_bar);
    if (((hits >> sh) & 1) != 0 and ((prng() & 0xFF) < eff_gate)) {
        // In replay, pull degrees from the motif ring (snapped to the
        // current mask so CA-suppressed degrees stay suppressed). In
        // capture, run the L-system and record each fired note.
        if (c.motif_in_replay() != 0) {
            var d = c.motif_replay_at(@intCast(step_in_bar));
            if (d != MOTIF_NO_NOTE) {
                d = snapToActiveMask(d, active_mask);
                cur_degree = d;
                const note = SCALES[cur_scale][cur_degree];
                const vtype = if ((step_in_bar & 1) != 0) VOICE_FM else VOICE_KS;
                if ((vm & VF_MELODY) != 0) c.voice_pool_trigger_role(note, vtype, ROLE_MELODY);
            }
        } else {
            const deg = c.lsystem_next(active_mask);
            if (deg != LSYSTEM_REST) {
                cur_degree = deg;
                const note = SCALES[cur_scale][cur_degree];
                const vtype = if ((step_in_bar & 1) != 0) VOICE_FM else VOICE_KS;
                if ((vm & VF_MELODY) != 0) c.voice_pool_trigger_role(note, vtype, ROLE_MELODY);
                c.motif_record(@intCast(step_in_bar), deg);
            }
        }
    }

    const cnt_hits: u16 = c.euclid_table[eucl_k_counter];
    if (((cnt_hits >> sh) & 1) != 0 and ((prng() & 0xFF) < eff_gate)) {
        // Biased away from the main melody's most recent degree:
        // avoids unison (the counter is +12, so same degree reads as
        // the same pitch class) and prefers 3rd/6th intervals.
        const next = markov2NextVoicedVoiced(
            cur_degree_counter_prev,
            cur_degree_counter,
            active_mask,
            cur_degree,
        );
        cur_degree_counter_prev = cur_degree_counter;
        cur_degree_counter = next;
        const note: u8 = SCALES[cur_scale][cur_degree_counter] + 12;
        if ((vm & VF_COUNTER) != 0) c.voice_pool_trigger_role(note, VOICE_FM, ROLE_MELODY);
    }
}

/// Swing: the fire sample for the UPCOMING substep, computed ONCE at
/// schedule time. Not per-sample — the offset depends on
/// samples_per_substep, which the live tempo keys mutate, and a
/// per-sample recompute could move the target below sample_clock and
/// stall the tick. Odd 16th-steps only (substeps 3, 9, ..., 45), so
/// bass, chord, kick, the bar boundary and the CA advance all stay
/// straight. Timing-only by construction: the substep SEQUENCE, and
/// with it every prng() draw, is identical at any swing value.
export fn gen_step() void {
    // Signed-difference compare, not ==: a live tempo SHRINK during a
    // swung gap can leave next_fire at or below sample_clock, and the
    // tick then fires immediately with a bounded catch-up instead of
    // freezing forever. The signed idiom also survives the ~24.8 h u32
    // wrap, where a plain >= would burst thousands of ticks.
    if (@as(i32, @bitCast(sample_clock -% next_fire)) >= 0) {
        const substep_in_bar = substep_count % BAR_SUBSTEPS;

        if (substep_in_bar == 0) scheduleBarBoundary();

        // ca_harm advances 4 times per bar, every 12 substeps.
        if (substep_in_bar % 12 == 0) {
            ca_harm = caStep(ca_harm, RULE_30);
            if (ca_harm == 0) ca_harm = 0x87654321;
        }

        const active_mask = computeActiveMask();

        scheduleDrums(substep_in_bar);
        scheduleBass(substep_in_bar);
        scheduleChord(substep_in_bar, active_mask);
        scheduleMelody(substep_in_bar, active_mask);

        substep_count +%= 1;
        // One snapshot, so the increment and the cap cannot see two
        // different values if a tempo key lands mid-tick.
        const s = samples_per_substep;
        next_step +%= s;
        // Odd 16th-substeps {3, 9, ..., 45} are exactly sub % 6 == 3.
        var off: u32 = 0;
        if (swing_amount != 0 and (substep_count % BAR_SUBSTEPS) % 6 == 3) {
            off = @divTrunc(s * swing_amount, 100); // fits u32: 7600*100
            if (off >= s) off = s - 1;
        }
        next_fire = next_step +% off;
    }
    sample_clock +%= 1;
}

export fn gen_force_mutate() void {
    mutate();
}

export fn gen_set_tempo(delta_pct: c_int) void {
    var new_val: i32 = @as(i32, @intCast(samples_per_substep)) +
        @divTrunc(@as(i32, @intCast(samples_per_substep)) * delta_pct, 100);
    // ~760 (faster) .. ~7600 (slower) at 48 kHz: bar between ~0.75 s
    // and ~7.5 s.
    if (new_val < 760) new_val = 760;
    if (new_val > 7600) new_val = 7600;
    samples_per_substep = @intCast(new_val);
}

export fn gen_get_step_samples() u32 {
    return samples_per_substep;
}
export fn gen_get_bar() u32 {
    return bar_count;
}
/// Melody-step position (0..15) for the status display: 48 substeps
/// maps to 16 melody steps.
export fn gen_get_step() u8 {
    return @intCast((substep_count % BAR_SUBSTEPS) / MELODY_SUBSTRIDE);
}
export fn gen_get_scale() u8 {
    return cur_scale;
}
export fn gen_get_gate() u8 {
    return gate_prob;
}
export fn gen_get_degree() u8 {
    return cur_degree;
}

/// The same active_mask gen_step uses, for UI readout.
export fn gen_get_active_mask() u8 {
    const m: u8 = @intCast(ca_row & 0x7F);
    const h: u8 = @intCast((ca_harm >> 8) & 0x7F);
    const r = m & (h | 0x11);
    return if (r != 0) r else 0x01;
}

export fn gen_get_chord_pattern() u8 {
    return @intCast(bar_count % N_CHORD_PATTERNS);
}

export fn gen_get_chord_root() u8 {
    return c.chord_progression_get_root();
}

export fn gen_get_section_name() [*:0]const u8 {
    return c.section_name();
}

export fn gen_get_tension() u8 {
    return c.density_get_tension();
}

export fn gen_motif_replaying() c_int {
    return c.motif_in_replay();
}

export fn gen_cycle_scale() void {
    cur_scale = @intCast((@as(usize, cur_scale) + 1) % N_SCALES);
}

/// FR-010 scale-degree accessor for callers outside gen. Bounds
/// checked: out-of-range scale wraps to 0, degree is taken modulo 7 so
/// the 7-letter modal layout is preserved whatever the caller passes.
export fn gen_get_scale_note(scale_idx: u8, degree: u8) u8 {
    var si = scale_idx;
    if (si >= N_SCALES) si = 0;
    return SCALES[si][degree % 7];
}

export fn gen_adjust_gate(delta: c_int) void {
    var p: c_int = @as(c_int, gate_prob) + delta;
    if (p < 32) p = 32;
    if (p > 255) p = 255;
    gate_prob = @intCast(p);
}

// Absolute setters for the preset-capture CLI flags. Same clamps as
// the live-key adjusters; all take c_int so main.c's flag table holds
// one uniform function-pointer type. None consume PRNG draws, so
// output stays a pure function of (seed, flags).

export fn gen_set_scale(idx: c_int) void {
    var i = idx;
    if (i < 0) i = 0;
    if (i >= @as(c_int, N_SCALES)) i = @as(c_int, N_SCALES) - 1;
    cur_scale = @intCast(i);
}

export fn gen_set_gate(v: c_int) void {
    var x = v;
    if (x < 32) x = 32;
    if (x > 255) x = 255;
    gate_prob = @intCast(x);
}

/// At 48 kHz with 48 substeps per bar, samples-per-substep numerically
/// equals milliseconds-per-bar. The conversion keeps the flag
/// meaningful if SAMPLE_RATE ever changes.
export fn gen_set_bar_ms(ms: c_int) void {
    var s: i64 = @divTrunc(@as(i64, ms) * SAMPLE_RATE, 48000);
    if (s < 760) s = 760;
    if (s > 7600) s = 7600;
    samples_per_substep = @intCast(s);
}

/// Store-only, same no-PRNG contract as gen_set_bar_ms. 0 = straight
/// (the byte-inert default); 100 ~ MPC 66.66% triplet feel.
export fn gen_set_swing(amount: c_int) void {
    var a = amount;
    if (a < 0) a = 0;
    if (a > 100) a = 100;
    swing_amount = @intCast(a);
}

export fn gen_get_swing() u8 {
    return @intCast(swing_amount);
}
