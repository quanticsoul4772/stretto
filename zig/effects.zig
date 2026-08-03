//! Master-bus stereo effects, ported from effects.c.
//!
//! effects.h is unchanged and remains the seam. Delay, Schroeder
//! reverb, soft saturation, compressor + brickwall limiter, and the
//! shared `sat16` helper that voice.c calls per sample.
//!
//! THE FIRST MODULE THAT CALLS ANOTHER
//!
//! Every prior port was a leaf. This one needs `arena_alloc`, so it
//! carries the `extern fn` half of the Principle V exception — the
//! declaration below is hand-matched against arena.h and nothing
//! checks it at compile time.
//!
//! It is a narrow exposure: one function, one argument, and a
//! mismatch cannot be subtle. `arena.h` declares
//! `void *arena_alloc(size_t n)`; a wrong width here would corrupt
//! every buffer allocated in effects_init(), which the bit-exact
//! golden catches on the first render.
//!
//! `arena_alloc` never returns null — arena.c treats OOM as a
//! programmer error and exits the process — so `.?` below matches the
//! C, which does not check either.
//!
//! Translation notes:
//!
//!   * Buffer pointers are `?[*]i16` initialised to null, matching the
//!     C statics that C zero-fills into .bss. `delay_init` /
//!     `reverb_init` test them for null to stay idempotent exactly as
//!     the C does.
//!
//!   * Every `int` in the header is `c_int` here, not `i32`: main.c's
//!     uniform flag table passes these directly.
//!
//!   * `soft_sat` widens to i64 before cubing, as the C does. |x| tops
//!     out at 32768 so x^3 reaches ~3.5e13 — it needs the 64-bit
//!     intermediate, and `>> 31` brings it back inside i32.
//!
//!   * Signed division uses @divTrunc (`over / COMP_RATIO`,
//!     `(target * 256) / comp_env`). Both numerators are non-negative
//!     at every reachable call site — `over` is guarded by
//!     `comp_env > comp_threshold` — so truncation and flooring agree
//!     today; @divTrunc is what C means regardless.
//!
//!   * All shifts are right shifts by constants. No `<<%` site.

/// From arena.h. See the module note on why this is hand-declared.
extern fn arena_alloc(n: usize) ?*anyopaque;

// ---- int16 saturating clamp -------------------------------------

export fn sat16(v: i32) i16 {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return @intCast(v);
}

// ---- master-bus stereo delay ------------------------------------
// Two independent mono buffers, 250 ms at 48 kHz. Feed-forward +
// feedback: out = dry + tap*wet; buffer-write = dry + tap*feedback.

const DELAY_SAMPLES: usize = 12000;

var delay_l: ?[*]i16 = null;
var delay_r: ?[*]i16 = null;
var delay_idx: usize = 0;
/// 0..256, mix amount
var delay_wet: u16 = 100;
/// 0..200, capped to avoid runaway
var delay_feedback: u16 = 140;

fn allocZero(n_samples: usize) [*]i16 {
    const p = arena_alloc(n_samples * @sizeOf(i16));
    const buf: [*]i16 = @ptrCast(@alignCast(p.?));
    @memset(buf[0..n_samples], 0);
    return buf;
}

fn delayInit() void {
    // Idempotent: allocate on first call; afterwards clear in place.
    if (delay_l == null) {
        delay_l = allocZero(DELAY_SAMPLES);
        delay_r = allocZero(DELAY_SAMPLES);
    } else {
        @memset(delay_l.?[0..DELAY_SAMPLES], 0);
        @memset(delay_r.?[0..DELAY_SAMPLES], 0);
    }
    delay_idx = 0;
}

export fn delay_process(buf: [*]i16, frames: u32) void {
    const dl = delay_l.?;
    const dr = delay_r.?;
    var i: usize = 0;
    while (i < frames) : (i += 1) {
        const dry_l: i32 = buf[2 * i];
        const dry_r: i32 = buf[2 * i + 1];
        const tap_l: i32 = dl[delay_idx];
        const tap_r: i32 = dr[delay_idx];

        const out_l = dry_l + ((tap_l * @as(i32, delay_wet)) >> 8);
        const out_r = dry_r + ((tap_r * @as(i32, delay_wet)) >> 8);

        const fb_l = dry_l + ((tap_l * @as(i32, delay_feedback)) >> 8);
        const fb_r = dry_r + ((tap_r * @as(i32, delay_feedback)) >> 8);

        dl[delay_idx] = sat16(fb_l);
        dr[delay_idx] = sat16(fb_r);

        delay_idx += 1;
        if (delay_idx >= DELAY_SAMPLES) delay_idx = 0;

        buf[2 * i] = sat16(out_l);
        buf[2 * i + 1] = sat16(out_r);
    }
}

fn clampInt(v: c_int, lo: c_int, hi: c_int) c_int {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

export fn delay_adjust_wet(delta: c_int) void {
    delay_wet = @intCast(clampInt(@as(c_int, delay_wet) + delta, 0, 256));
}

export fn delay_adjust_feedback(delta: c_int) void {
    delay_feedback = @intCast(clampInt(@as(c_int, delay_feedback) + delta, 0, 200));
}

export fn delay_set_wet(v: c_int) void {
    delay_wet = @intCast(clampInt(v, 0, 256));
}

export fn delay_set_feedback(v: c_int) void {
    delay_feedback = @intCast(clampInt(v, 0, 200));
}

export fn delay_get_wet() u16 {
    return delay_wet;
}
export fn delay_get_feedback() u16 {
    return delay_feedback;
}

// ---- Schroeder reverb -------------------------------------------
// 4 parallel combs per channel summed into 2 series all-passes.
// Prime delays (Schroeder 1962, rescaled by 48000/44100) avoid
// metallic resonance; slightly different L/R delays keep the tail
// stereo.

const REV_C1L: u16 = 1693;
const REV_C2L: u16 = 1759;
const REV_C3L: u16 = 1621;
const REV_C4L: u16 = 1549;
const REV_C1R: u16 = 1721;
const REV_C2R: u16 = 1747;
const REV_C3R: u16 = 1613;
const REV_C4R: u16 = 1571;
const REV_AP1L: u16 = 241;
const REV_AP2L: u16 = 607;
const REV_AP1R: u16 = 251;
const REV_AP2R: u16 = 613;
/// ~0.70 in 8.8 fixed, RT60 ~1.5 s
const COMB_G: i32 = 180;
const AP_G: i32 = 180;

var rev_c1l: ?[*]i16 = null;
var rev_c2l: ?[*]i16 = null;
var rev_c3l: ?[*]i16 = null;
var rev_c4l: ?[*]i16 = null;
var rev_c1r: ?[*]i16 = null;
var rev_c2r: ?[*]i16 = null;
var rev_c3r: ?[*]i16 = null;
var rev_c4r: ?[*]i16 = null;
var rev_ap1l: ?[*]i16 = null;
var rev_ap2l: ?[*]i16 = null;
var rev_ap1r: ?[*]i16 = null;
var rev_ap2r: ?[*]i16 = null;

var i_c1l: u16 = 0;
var i_c2l: u16 = 0;
var i_c3l: u16 = 0;
var i_c4l: u16 = 0;
var i_c1r: u16 = 0;
var i_c2r: u16 = 0;
var i_c3r: u16 = 0;
var i_c4r: u16 = 0;
var i_ap1l: u16 = 0;
var i_ap2l: u16 = 0;
var i_ap1r: u16 = 0;
var i_ap2r: u16 = 0;

/// 0..256, mix amount
var reverb_wet: u16 = 60;
/// section-driven additive bias
var reverb_wet_bias: i8 = 0;

fn reverbInit() void {
    if (rev_c1l == null) {
        rev_c1l = allocZero(REV_C1L);
        rev_c1r = allocZero(REV_C1R);
        rev_c2l = allocZero(REV_C2L);
        rev_c2r = allocZero(REV_C2R);
        rev_c3l = allocZero(REV_C3L);
        rev_c3r = allocZero(REV_C3R);
        rev_c4l = allocZero(REV_C4L);
        rev_c4r = allocZero(REV_C4R);
        rev_ap1l = allocZero(REV_AP1L);
        rev_ap1r = allocZero(REV_AP1R);
        rev_ap2l = allocZero(REV_AP2L);
        rev_ap2r = allocZero(REV_AP2R);
    } else {
        @memset(rev_c1l.?[0..REV_C1L], 0);
        @memset(rev_c1r.?[0..REV_C1R], 0);
        @memset(rev_c2l.?[0..REV_C2L], 0);
        @memset(rev_c2r.?[0..REV_C2R], 0);
        @memset(rev_c3l.?[0..REV_C3L], 0);
        @memset(rev_c3r.?[0..REV_C3R], 0);
        @memset(rev_c4l.?[0..REV_C4L], 0);
        @memset(rev_c4r.?[0..REV_C4R], 0);
        @memset(rev_ap1l.?[0..REV_AP1L], 0);
        @memset(rev_ap1r.?[0..REV_AP1R], 0);
        @memset(rev_ap2l.?[0..REV_AP2L], 0);
        @memset(rev_ap2r.?[0..REV_AP2R], 0);
    }
    i_c1l = 0;
    i_c2l = 0;
    i_c3l = 0;
    i_c4l = 0;
    i_c1r = 0;
    i_c2r = 0;
    i_c3r = 0;
    i_c4r = 0;
    i_ap1l = 0;
    i_ap2l = 0;
    i_ap1r = 0;
    i_ap2r = 0;
}

/// y[n] = x[n-D] + g*y[n-D], delay line as a recirculating buffer.
inline fn combStep(buf: [*]i16, size: u16, idx: *u16, in: i16) i16 {
    const tap: i32 = buf[idx.*];
    const w = @as(i32, in) + ((tap * COMB_G) >> 8);
    buf[idx.*] = sat16(w);
    idx.* = (idx.* + 1) % size;
    return @intCast(tap);
}

/// All-pass: flat magnitude, shifts phase. Smooths the dense comb
/// output into a continuous tail.
inline fn apStep(buf: [*]i16, size: u16, idx: *u16, in: i16) i16 {
    const tap: i32 = buf[idx.*];
    const y = tap - ((@as(i32, in) * AP_G) >> 8);
    const w = @as(i32, in) + ((tap * AP_G) >> 8);
    buf[idx.*] = sat16(w);
    idx.* = (idx.* + 1) % size;
    return sat16(y);
}

export fn reverb_process(buf: [*]i16, frames: u32) void {
    var i: usize = 0;
    while (i < frames) : (i += 1) {
        const in_l: i16 = buf[2 * i];
        const in_r: i16 = buf[2 * i + 1];

        var sum_l: i32 = @as(i32, combStep(rev_c1l.?, REV_C1L, &i_c1l, in_l)) +
            combStep(rev_c2l.?, REV_C2L, &i_c2l, in_l) +
            combStep(rev_c3l.?, REV_C3L, &i_c3l, in_l) +
            combStep(rev_c4l.?, REV_C4L, &i_c4l, in_l);
        sum_l >>= 2;
        var sum_r: i32 = @as(i32, combStep(rev_c1r.?, REV_C1R, &i_c1r, in_r)) +
            combStep(rev_c2r.?, REV_C2R, &i_c2r, in_r) +
            combStep(rev_c3r.?, REV_C3R, &i_c3r, in_r) +
            combStep(rev_c4r.?, REV_C4R, &i_c4r, in_r);
        sum_r >>= 2;

        var ap_l = apStep(rev_ap1l.?, REV_AP1L, &i_ap1l, @intCast(sum_l));
        ap_l = apStep(rev_ap2l.?, REV_AP2L, &i_ap2l, ap_l);
        var ap_r = apStep(rev_ap1r.?, REV_AP1R, &i_ap1r, @intCast(sum_r));
        ap_r = apStep(rev_ap2r.?, REV_AP2R, &i_ap2r, ap_r);

        // Section bias on top of user wet, clamped to [0, 256].
        var eff_wet: i32 = @as(i32, reverb_wet) + @as(i32, reverb_wet_bias);
        if (eff_wet < 0) eff_wet = 0;
        if (eff_wet > 256) eff_wet = 256;
        const out_l = @as(i32, in_l) + ((@as(i32, ap_l) * eff_wet) >> 8);
        const out_r = @as(i32, in_r) + ((@as(i32, ap_r) * eff_wet) >> 8);
        buf[2 * i] = sat16(out_l);
        buf[2 * i + 1] = sat16(out_r);
    }
}

export fn reverb_adjust_wet(delta: c_int) void {
    reverb_wet = @intCast(clampInt(@as(c_int, reverb_wet) + delta, 0, 256));
}

export fn reverb_set_wet(v: c_int) void {
    reverb_wet = @intCast(clampInt(v, 0, 256));
}

export fn reverb_get_wet() u16 {
    return reverb_wet;
}

export fn reverb_set_wet_bias(bias: i8) void {
    reverb_wet_bias = bias;
}

// ---- soft saturation --------------------------------------------
// Cubic soft-clip: y = x - x^3 / 2^31. Linear for small x, smoothly
// compresses peaks. At full-scale input the output is ~50%; at
// typical levels (10-20% of full scale) the change is sub-1%.

inline fn softSat(x: i16) i16 {
    const xw: i64 = x;
    const x3: i64 = xw * xw * xw;
    const cubic: i32 = @intCast(x3 >> 31);
    return sat16(@as(i32, x) - cubic);
}

export fn saturate_process(buf: [*]i16, frames: u32) void {
    var i: usize = 0;
    while (i < frames) : (i += 1) {
        buf[2 * i] = softSat(buf[2 * i]);
        buf[2 * i + 1] = softSat(buf[2 * i + 1]);
    }
}

// ---- master compressor + brickwall limiter ----------------------
// Feed-forward, stereo-linked: the envelope tracks max(|L|,|R|) so
// gain reduction is identical on both channels and stereo imaging is
// preserved.

const COMP_THRESHOLD_DEFAULT: i32 = 20000;
const COMP_THRESHOLD_MIN: c_int = 8000;
const COMP_THRESHOLD_MAX: c_int = 30000;
/// 4:1 above threshold
const COMP_RATIO: i32 = 4;
/// (1-exp(-1/(0.005*48000)))*65536
const COMP_ATTACK_COEF: i32 = 268;
/// (1-exp(-1/(0.200*48000)))*65536
const COMP_RELEASE_COEF: i32 = 7;
/// ~+1 dB in 8.8 fixed
const COMP_MAKEUP_GAIN: i32 = 288;
/// brickwall, below int16 max so sat16 keeps margin
const LIMIT_CEILING: i32 = 32000;

var comp_env: i32 = 0;
var comp_threshold: i32 = COMP_THRESHOLD_DEFAULT;

export fn compressor_process(buf: [*]i16, frames: u32) void {
    var i: usize = 0;
    while (i < frames) : (i += 1) {
        const l: i32 = buf[2 * i];
        const r: i32 = buf[2 * i + 1];
        const al = if (l < 0) -l else l;
        const ar = if (r < 0) -r else r;
        const a = if (al > ar) al else ar;

        // Asymmetric envelope follower: fast attack, slow release.
        if (a > comp_env) {
            comp_env += ((a - comp_env) * COMP_ATTACK_COEF) >> 16;
        } else {
            comp_env -= ((comp_env - a) * COMP_RELEASE_COEF) >> 16;
        }

        // 1.0 below threshold, compressed above. gain is 8.8 fixed;
        // target_env = threshold + over/ratio.
        var gain: i32 = 256;
        if (comp_env > comp_threshold) {
            const over = comp_env - comp_threshold;
            const target = comp_threshold + @divTrunc(over, COMP_RATIO);
            gain = @divTrunc(target * 256, comp_env);
        }

        // Apply gain + makeup, then brickwall.
        var yl = (l * gain) >> 8;
        var yr = (r * gain) >> 8;
        yl = (yl * COMP_MAKEUP_GAIN) >> 8;
        yr = (yr * COMP_MAKEUP_GAIN) >> 8;
        if (yl > LIMIT_CEILING) yl = LIMIT_CEILING;
        if (yl < -LIMIT_CEILING) yl = -LIMIT_CEILING;
        if (yr > LIMIT_CEILING) yr = LIMIT_CEILING;
        if (yr < -LIMIT_CEILING) yr = -LIMIT_CEILING;

        buf[2 * i] = sat16(yl);
        buf[2 * i + 1] = sat16(yr);
    }
}

export fn compressor_adjust_threshold(delta: c_int) void {
    const v = clampInt(@as(c_int, comp_threshold) + delta, COMP_THRESHOLD_MIN, COMP_THRESHOLD_MAX);
    comp_threshold = @intCast(v);
}

export fn compressor_set_threshold(v: c_int) void {
    comp_threshold = @intCast(clampInt(v, COMP_THRESHOLD_MIN, COMP_THRESHOLD_MAX));
}

export fn compressor_get_threshold() u16 {
    return @intCast(comp_threshold);
}

// ---- one-shot init ----------------------------------------------

export fn effects_init() void {
    delayInit();
    reverbInit();
    comp_env = 0;
    comp_threshold = COMP_THRESHOLD_DEFAULT;
}
