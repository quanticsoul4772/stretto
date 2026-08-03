//! Voice synthesis and the 11-slot pool, ported from voice.c.
//!
//! voice.h is unchanged and remains the seam.
//!
//! WHY THIS MODULE USES @cImport
//!
//! Every prior port hand-declared what it needed. This one imports the
//! C headers directly, and that is the safer choice here rather than a
//! shortcut — see `specs/005-zig-port/research.md`, "@cImport at voice
//! — resolved". In short:
//!
//!   * `Voice` is 1088 bytes with a six-arm union, and its layout is
//!     pinned from three independent directions: tests/unit/test_voice.c
//!     declares `Voice v;` on the stack and reads union members,
//!     voice_pool_init computes `@sizeOf(Voice)` for arena_alloc, and
//!     the field offsets have to agree with both. Deriving it from
//!     voice.h makes drift impossible instead of merely tested for.
//!     Verified: @sizeOf(c.Voice) == 1088 == C's sizeof(Voice).
//!
//!   * The four generated tables hold ~2500 constants. Transcribing
//!     them would fork Principle III's source-of-truth bytes.
//!
//!   * Principle V's objection to @cImport — a distinct incompatible
//!     type per importing module — needs two Zig modules importing the
//!     same struct. gen.c never names Voice or Stereo, so voice.zig is
//!     the only one that ever will.
//!
//! TRANSLATION HAZARDS
//!
//!   * Shifts that discard set bits — `x ^= x << 13` / `<< 5` in the
//!     PRNG, and `(mod * mod_depth) << 6` at the FM modulation index,
//!     which reaches ~1.97e8 before shifting. These need no special
//!     operator: Zig's `<<` on an unsigned type discards high bits
//!     exactly as C's does. **There is no `<<%` in Zig** — FR-006 in
//!     the spec said there was, and that was wrong. The illegal case
//!     for `<<` is a shift AMOUNT at or above the bit width, which
//!     none of these reach.
//!
//!   * Integer promotion at every mix site. C widens int16_t to int
//!     implicitly; each widening here is explicit, and the narrowing
//!     back uses @truncate where C used a cast that can be lossy and
//!     @intCast where the range is provable.
//!
//!   * Signed division uses @divTrunc: glide_advance's delta is
//!     genuinely negative on a downward slide, and bent_inc's is
//!     negative on every down-bend.

const c = @cImport({
    @cInclude("voice.h");
    @cInclude("arena.h");
    @cInclude("effects.h");
    @cInclude("sin_table.h");
    @cInclude("env_table.h");
    @cInclude("note_table.h");
    @cInclude("wavetable.h");
});

const Voice = c.Voice;
const Stereo = c.Stereo;

// Enum values derived from voice.h rather than restated, so a
// reordering there cannot silently disagree with this file.
const VOICE_OFF: u8 = @intCast(c.VOICE_OFF);
const VOICE_KS: u8 = @intCast(c.VOICE_KS);
const VOICE_FM: u8 = @intCast(c.VOICE_FM);
const VOICE_DRUM: u8 = @intCast(c.VOICE_DRUM);
const VOICE_WT: u8 = @intCast(c.VOICE_WT);
const VOICE_ADD: u8 = @intCast(c.VOICE_ADD);
const VOICE_SUB: u8 = @intCast(c.VOICE_SUB);

const ENV_OFF: u8 = @intCast(c.ENV_OFF);
const ENV_A: u8 = @intCast(c.ENV_A);
const ENV_D: u8 = @intCast(c.ENV_D);
const ENV_R: u8 = @intCast(c.ENV_R);

const ROLE_BASS: u8 = @intCast(c.ROLE_BASS);
const ROLE_CHORD: u8 = @intCast(c.ROLE_CHORD);
const ROLE_MELODY: u8 = @intCast(c.ROLE_MELODY);
const ROLE_DRUM: u8 = @intCast(c.ROLE_DRUM);

const DRUM_KICK: u8 = @intCast(c.DRUM_KICK);
const DRUM_SNARE: u8 = @intCast(c.DRUM_SNARE);
const DRUM_HIHAT: u8 = @intCast(c.DRUM_HIHAT);

const N_VOICES: usize = @intCast(c.N_VOICES);
const N_WT_WAVES: u32 = @intCast(c.N_WT_WAVES);

// Sample counts calibrated for SAMPLE_RATE = 48000.
const ENV_ATTACK_SAMPLES: u32 = 240;
const ENV_DECAY_SAMPLES: u32 = 9600;
const ENV_RELEASE_SAMPLES: u32 = 28800;
const ENV_SUSTAIN_LEVEL: u32 = 16384;
const ENV_PEAK: u32 = 32767;

const PEAK_WINDOW_SAMPLES: u16 = 2400;
const PEAK_TARGET: u32 = 16000;
const PEAK_GAIN_MAX: u32 = 1024;
const PEAK_GAIN_UNITY: u16 = 256;

/// 32440/65536 ~= 0.9897.
const KS_AVG_COEF: i32 = 32440;

const CUTOFF_LFO_SHIFT: u5 = 15;
const CUTOFF_FENV_GAIN: i32 = 30;
const CUTOFF_FENV_SHIFT: u5 = 14;

const GLIDE_SAMPLES: u16 = 2400;
const GLIDE_LEGATO_THRESH: u16 = 8192;

const SUSTAIN_HELD_BIT: u8 = 0x80;

// SVF (2-pole Chamberlin) parameters, runtime-tunable.
var svf_f_base: u16 = 200;
var svf_q_base: u16 = 100;
/// section-driven additive bias on f_eff
var cutoff_bias: i8 = 0;
/// 0 LP, 1 HP, 2 BP, 3 notch (LP+HP)
var filter_mode: u8 = 0;
var lfo_filter_depth: u16 = 80;

const role_svf_f_off = [4]i16{ -100, -40, 0, -120 };
const role_svf_q_off = [4]i16{ -30, 0, 0, -50 };

var prng_state: u32 = 0xCAFEBABE;
var fm_mod_depth: u16 = 1500;

export fn voice_set_mod_depth(d: u16) void {
    var v = d;
    if (v < 100) v = 100;
    if (v > 8000) v = 8000;
    fm_mod_depth = v;
}

export fn voice_get_mod_depth() u16 {
    return fm_mod_depth;
}

fn prngNoise() i16 {
    var x = prng_state;
    // Plain <<, not a wrapping variant: Zig's << on an unsigned type
    // discards high bits exactly as C's does. (Zig has no `<<%`; the
    // illegal-behaviour case for << is a shift AMOUNT >= the bit
    // width, which 13/17/5 on a u32 never reach.)
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return @bitCast(@as(u16, @truncate(x >> 16)));
}

const role_mod_depth = [4]u16{ 200, 1500, 1500, 0 };
const role_fm_ratio = [4]u8{ 1, 2, 2, 1 };
/// 50 / 20 / 5 / 0.5 ms
const role_attack = [4]u16{ 2400, 960, 240, 24 };
/// 1000 / 600 / 600 / 100 ms
const role_release = [4]u16{ 48000, 28800, 28800, 4800 };

/// 0 = full left, 128 = center, 255 = full right.
const slot_base_pan = [N_VOICES]u8{
    128, 128, // bass slots 0-1: center
    72,  128, 184, // chord slots 2-4: L, C, R
    56,  200, 96, // melody slots 5-7: alternating outer
    128, 144, 112, // drum slots 8-10: kick C, snare +R, hihat -L
};

/// Slow pan motion, ~0.07-0.18 Hz. Drum slots get 0.
const slot_lfo_inc = [N_VOICES]u32{
    6263,  8946,
    9841,  7603,  11178,
    13418, 10737, 16103,
    0,     0,     0,
};

const role_pan_jitter = [4]u8{ 16, 32, 48, 8 };

/// 4 profiles x 8 partial amplitudes.
const ADD_PROFILES = [4][8]u8{
    .{ 64, 64, 32, 32, 32, 16, 8, 8 }, // Hammond, sum 256
    .{ 128, 0, 64, 0, 32, 0, 16, 0 }, // Square,  sum 240
    .{ 128, 64, 32, 16, 8, 4, 2, 2 }, // Strings, sum 256
    .{ 64, 64, 64, 48, 32, 16, 8, 4 }, // Brass,   sum 300
};

export fn voice_init(v: *Voice) void {
    v.type = VOICE_OFF;
    v.note = 0;
    v.env_phase = ENV_OFF;
    v.role = ROLE_MELODY;
    v.pan = 128;
    v.env_amp = 0;
    v.env_time = 0;
    v.lfo_phase = 0;
    v.lfo_inc = 0;
    v.peak_seen = 1;
    v.gain = PEAK_GAIN_UNITY;
    v.peak_window = 0;
    v.fenv_amp = 0;
    v.fenv_time = 0;
    v.fenv_phase = ENV_OFF;
    v.svf_lp = 0;
    v.svf_bp = 0;
    v.inc_target = 0;
    v.glide_remain = 0;
    v.trigger_key = 0;
    v.trigger_channel = 0;
}

export fn voice_trigger(v: *Voice, note: u8, vtype: u8, role: u8) void {
    // Legato glide: a SUB bass slot still above GLIDE_LEGATO_THRESH
    // slides to the new note instead of hard re-triggering. Phases,
    // envelope, peak normalizer and SVF state all carry across.
    if (vtype == VOICE_SUB and role == ROLE_BASS and
        v.type == VOICE_SUB and v.role == ROLE_BASS and
        v.env_phase != ENV_OFF and
        v.env_amp > GLIDE_LEGATO_THRESH)
    {
        v.inc_target = c.note_phase_inc[note];
        v.glide_remain = GLIDE_SAMPLES;
        v.note = note;
        return;
    }

    v.type = vtype;
    v.note = note;
    v.role = role;
    v.env_phase = ENV_A;
    v.env_time = 0;
    v.env_amp = 0;
    v.svf_lp = 0;
    v.svf_bp = 0;
    v.peak_seen = 1;
    v.gain = PEAK_GAIN_UNITY;
    v.peak_window = PEAK_WINDOW_SAMPLES;
    v.fenv_amp = 0;
    v.fenv_time = 0;
    v.fenv_phase = ENV_A;
    // A slot previously held by a MIDI note must not keep its tags, or
    // a stale Note Off would release an unrelated generative voice.
    // The legato branch above deliberately preserves them.
    v.trigger_key = 0;
    v.trigger_channel = 0;

    if (vtype == VOICE_KS) {
        v.u.ks.len = c.note_ks_len[note];
        v.u.ks.idx = 0;
        const len = v.u.ks.len;
        var i: u16 = 0;
        while (i < len) : (i += 1) {
            v.u.ks.buf[i] = prngNoise() >> 1;
        }
    } else if (vtype == VOICE_DRUM) {
        v.role = ROLE_DRUM;
        v.u.drum.drum_type = note;
        v.u.drum.phase = 0;
        // Kick: sine at ~150 Hz decaying toward ~50 Hz.
        // inc = 150 * 2^32 / 48000. Snare/hihat are noise, so 0.
        v.u.drum.inc = if (note == DRUM_KICK) 13421773 else 0;
    } else if (vtype == VOICE_WT) {
        v.u.wt.phase = 0;
        v.u.wt.inc = c.note_phase_inc[note];
        v.u.wt.position = 0;
    } else if (vtype == VOICE_ADD) {
        var k: usize = 0;
        while (k < 8) : (k += 1) v.u.add.phase[k] = 0;
        v.u.add.inc_base = c.note_phase_inc[note];
        v.u.add.amps = &ADD_PROFILES[0];
    } else if (vtype == VOICE_SUB) {
        // Super-saw: 3 detuned band-limited saws. Detune ~0.78%
        // (inc >> 7) keeps the beating slow enough to read as phase
        // movement rather than vibrato.
        const base = c.note_phase_inc[note];
        v.u.sub.phase[0] = 0;
        v.u.sub.phase[1] = 0;
        v.u.sub.phase[2] = 0;
        v.u.sub.inc[0] = base;
        v.u.sub.inc[1] = base +% (base >> 7);
        v.u.sub.inc[2] = base -% (base >> 7);
        // Fresh trigger, not legato: clear any prior ramp.
        v.inc_target = 0;
        v.glide_remain = 0;
    } else {
        v.u.fm.phase_c = 0;
        v.u.fm.phase_m = 0;
        v.u.fm.inc_c = c.note_phase_inc[note];
        v.u.fm.inc_m = c.note_phase_inc[note] *% role_fm_ratio[role];
        v.u.fm.mod_depth = if (role == ROLE_MELODY) fm_mod_depth else role_mod_depth[role];
    }
}

fn ksStep(v: *Voice) i16 {
    const idx = v.u.ks.idx;
    const len = v.u.ks.len;
    var next = idx + 1;
    if (next >= len) next = 0;
    const a = v.u.ks.buf[idx];
    const b = v.u.ks.buf[next];
    const avg: i16 = @truncate(((@as(i32, a) + @as(i32, b)) * KS_AVG_COEF) >> 16);
    v.u.ks.buf[idx] = avg;
    v.u.ks.idx = next;
    return a;
}

/// 8 partials at integer multiples of the fundamental, each weighted
/// by ADD_PROFILES[idx][k]. Profile amplitudes sum to ~256 and the
/// sin_table peak is ~24576, so >>8 lands in int16 range; sat16
/// catches the brass profile's overshoot (sum 300).
fn addStep(v: *Voice) i16 {
    var out: i32 = 0;
    var k: usize = 0;
    while (k < 8) : (k += 1) {
        const s = c.sin_table[v.u.add.phase[k] >> 22];
        out += @as(i32, s) * @as(i32, v.u.add.amps[k]);
        v.u.add.phase[k] +%= v.u.add.inc_base *% @as(u32, @intCast(k + 1));
    }
    return c.sat16(out >> 8);
}

/// Position (0..N_WT_WAVES*256) selects between adjacent waves with
/// linear interpolation; the mix is sampled at phase>>22 as an 8-bit
/// index into the 256-sample wave.
fn wtStep(v: *Voice) i16 {
    const pos: u32 = v.u.wt.position;
    const wave_idx = (pos >> 8) % N_WT_WAVES;
    const wave_frac = pos & 0xFF;
    const next_idx = (wave_idx + 1) % N_WT_WAVES;
    const ph = (v.u.wt.phase >> 22) & 0xFF;
    const a = c.WAVETABLE[wave_idx][ph];
    const b = c.WAVETABLE[next_idx][ph];
    const s: i16 = @truncate(@as(i32, a) +
        (((@as(i32, b) - @as(i32, a)) * @as(i32, @intCast(wave_frac))) >> 8));
    v.u.wt.phase +%= v.u.wt.inc;
    return s;
}

/// Sums 3 detuned band-limited saws (WAVETABLE[4]) and averages. The
/// per-voice SVF does the spectral shaping.
fn subStep(v: *Voice) i16 {
    var out: i32 = 0;
    var k: usize = 0;
    while (k < 3) : (k += 1) {
        const ph = (v.u.sub.phase[k] >> 22) & 0xFF;
        out += c.WAVETABLE[4][ph];
        v.u.sub.phase[k] +%= v.u.sub.inc[k];
    }
    return @truncate(@divTrunc(out, 3));
}

fn fmStep(v: *Voice) i16 {
    // The pan LFO also detunes carrier and modulator, same scale on
    // both so the FM ratio holds. Peak excursion ~0.29% of inc, about
    // 5 cents. i64 for the multiply so high notes cannot overflow.
    const lfo = c.sin_table[v.lfo_phase >> 22];
    const det_m: i32 = @intCast((@as(i64, v.u.fm.inc_m) * @as(i64, lfo)) >> 23);
    const det_c: i32 = @intCast((@as(i64, v.u.fm.inc_c) * @as(i64, lfo)) >> 23);

    const mod = c.sin_table[v.u.fm.phase_m >> 22];
    v.u.fm.phase_m +%= @bitCast(@as(i32, @bitCast(v.u.fm.inc_m)) +% det_m);
    // The shift discards set bits and the algorithm depends on it:
    // mod reaches +/-24576 and mod_depth up to 8000, so the product
    // hits ~1.97e8 before shifting. Plain << discards exactly as C.
    const phase_with_mod = v.u.fm.phase_c +%
        (@as(u32, @bitCast(@as(i32, mod) * @as(i32, v.u.fm.mod_depth))) << 6);
    const out = c.sin_table[phase_with_mod >> 22];
    v.u.fm.phase_c +%= @bitCast(@as(i32, @bitCast(v.u.fm.inc_c)) +% det_c);
    return out;
}

fn envStep(v: *Voice) u16 {
    var amp: u32 = v.env_amp;
    const attack_n: u32 = role_attack[v.role];
    var release_n: u32 = role_release[v.role];

    // Drums get a per-type release: kick 150 ms, snare 100 ms,
    // hihat 30 ms. Attack stays at the role default.
    if (v.type == VOICE_DRUM) {
        const drum_release = [3]u16{ 7200, 4800, 1440 };
        if (v.u.drum.drum_type < 3) release_n = drum_release[v.u.drum.drum_type];
    }

    if (v.env_phase == ENV_A) {
        var idx: u32 = @divTrunc(@as(u32, v.env_time) * 255, attack_n);
        if (idx > 255) idx = 255;
        amp = @divTrunc(@as(u32, c.env_table[idx]) * ENV_PEAK, 255);
        v.env_time += 1;
        if (v.env_time >= attack_n) {
            // Drums skip decay/sustain: peak straight into release.
            v.env_phase = if (v.type == VOICE_DRUM) ENV_R else ENV_D;
            v.env_time = 0;
            amp = ENV_PEAK;
        }
    } else if (v.env_phase == ENV_D) {
        var idx: u32 = @divTrunc(@as(u32, v.env_time) * 255, ENV_DECAY_SAMPLES);
        if (idx > 255) idx = 255;
        const curve: u32 = 255 - c.env_table[idx];
        amp = ENV_SUSTAIN_LEVEL + @divTrunc((ENV_PEAK - ENV_SUSTAIN_LEVEL) * curve, 255);
        v.env_time += 1;
        if (v.env_time >= ENV_DECAY_SAMPLES) {
            // Gate semantics: MIDI-tagged voices park at sustain until
            // their Note Off (or pedal-up). Generative voices keep the
            // fire-and-forget auto-release, so goldens are untouched.
            if (v.trigger_channel != 0) {
                v.env_time = @intCast(ENV_DECAY_SAMPLES);
            } else {
                v.env_phase = ENV_R;
                v.env_time = 0;
            }
            amp = ENV_SUSTAIN_LEVEL;
        }
    } else if (v.env_phase == ENV_R) {
        if (v.type == VOICE_DRUM) {
            // Linear decay from peak to zero over release_n.
            if (v.env_time >= release_n) {
                amp = 0;
                v.env_phase = ENV_OFF;
                v.type = VOICE_OFF;
            } else {
                amp = @divTrunc(ENV_PEAK * (release_n - v.env_time), release_n);
            }
            v.env_time +%= 1;
        } else {
            var idx: u32 = @divTrunc(@as(u32, v.env_time) * 255, release_n);
            if (idx > 255) idx = 255;
            const curve: u32 = 255 - c.env_table[idx];
            amp = @divTrunc(ENV_SUSTAIN_LEVEL * curve, 255);
            v.env_time += 1;
            if (v.env_time >= release_n) {
                v.env_phase = ENV_OFF;
                v.type = VOICE_OFF;
                amp = 0;
            }
        }
    } else {
        amp = 0;
    }

    v.env_amp = @truncate(amp);
    return @truncate(amp);
}

/// KICK  sine sweep + brief noise click on attack
/// SNARE noise-dominant (90/10) + ~200 Hz sine body
/// HIHAT noise only
fn drumStep(v: *Voice) i16 {
    if (v.u.drum.drum_type == DRUM_KICK) {
        const body = c.sin_table[v.u.drum.phase >> 22];
        v.u.drum.phase +%= v.u.drum.inc;
        // Pitch decay: drop inc by ~1/4096 per sample.
        v.u.drum.inc -%= v.u.drum.inc >> 12;
        // First ~5 ms blends a noise burst with the sine so the kick
        // stays audible on speakers with weak bass response.
        if (v.env_time < 240) {
            const click = prngNoise();
            return @truncate(@divTrunc(@as(i32, body) + @as(i32, click), 2));
        }
        return body;
    }
    if (v.u.drum.drum_type == DRUM_SNARE) {
        const noise = prngNoise();
        const tone = c.sin_table[v.u.drum.phase >> 22];
        v.u.drum.phase +%= 17895697;
        return @truncate(@divTrunc(@as(i32, noise) * 9 + @as(i32, tone), 10));
    }
    return prngNoise();
}

/// Chord filter envelope, reusing the amplitude env's timing.
fn fenvStep(v: *Voice) void {
    if (v.role != ROLE_CHORD) return;
    var amp: u32 = v.fenv_amp;

    if (v.fenv_phase == ENV_A) {
        var idx: u32 = @divTrunc(@as(u32, v.fenv_time) * 255, ENV_ATTACK_SAMPLES);
        if (idx > 255) idx = 255;
        amp = @divTrunc(@as(u32, c.env_table[idx]) * ENV_PEAK, 255);
        v.fenv_time += 1;
        if (v.fenv_time >= ENV_ATTACK_SAMPLES) {
            v.fenv_phase = ENV_D;
            v.fenv_time = 0;
            amp = ENV_PEAK;
        }
    } else if (v.fenv_phase == ENV_D) {
        var idx: u32 = @divTrunc(@as(u32, v.fenv_time) * 255, ENV_DECAY_SAMPLES);
        if (idx > 255) idx = 255;
        const curve: u32 = 255 - c.env_table[idx];
        amp = ENV_SUSTAIN_LEVEL + @divTrunc((ENV_PEAK - ENV_SUSTAIN_LEVEL) * curve, 255);
        v.fenv_time += 1;
        if (v.fenv_time >= ENV_DECAY_SAMPLES) {
            v.fenv_phase = ENV_R;
            v.fenv_time = 0;
            amp = ENV_SUSTAIN_LEVEL;
        }
    } else if (v.fenv_phase == ENV_R) {
        var idx: u32 = @divTrunc(@as(u32, v.fenv_time) * 255, ENV_RELEASE_SAMPLES);
        if (idx > 255) idx = 255;
        const curve: u32 = 255 - c.env_table[idx];
        amp = @divTrunc(ENV_SUSTAIN_LEVEL * curve, 255);
        v.fenv_time += 1;
        if (v.fenv_time >= ENV_RELEASE_SAMPLES) {
            v.fenv_phase = ENV_OFF;
            amp = 0;
        }
    } else {
        amp = 0;
    }
    v.fenv_amp = @truncate(amp);
}

/// Linear walk by remaining-sample division so the result lands
/// exactly on inc_target when glide_remain hits 0. Detune is rebuilt
/// from the new base each step so the super-saw character holds.
fn glideAdvance(v: *Voice) void {
    if (v.glide_remain == 0 or v.type != VOICE_SUB) return;
    const cur = v.u.sub.inc[0];
    // Genuinely negative on a downward slide, hence @divTrunc.
    const diff: i32 = @bitCast(v.inc_target -% cur);
    const delta = @divTrunc(diff, @as(i32, v.glide_remain));
    const new_base: u32 = @bitCast(@as(i32, @bitCast(cur)) +% delta);
    v.u.sub.inc[0] = new_base;
    v.u.sub.inc[1] = new_base +% (new_base >> 7);
    v.u.sub.inc[2] = new_base -% (new_base >> 7);
    v.glide_remain -= 1;
}

fn oscDispatch(v: *Voice) i16 {
    if (v.type == VOICE_KS) return ksStep(v);
    if (v.type == VOICE_FM) return fmStep(v);
    if (v.type == VOICE_DRUM) return drumStep(v);
    if (v.type == VOICE_WT) {
        // Map the pan LFO onto wavetable position so the timbre sweeps
        // all 8 waveforms over ~10 s. No extra modulator state.
        v.u.wt.position = @truncate(((v.lfo_phase >> 16) * (N_WT_WAVES * 256)) >> 16);
        return wtStep(v);
    }
    if (v.type == VOICE_ADD) return addStep(v);
    if (v.type == VOICE_SUB) return subStep(v);
    return 0;
}

/// base + per-role offset + section bias + pan-LFO sweep + (chord
/// only) filter-envelope opening, clamped to [20, 230].
fn computeCutoff(v: *Voice) i32 {
    var f_eff: i32 = @as(i32, svf_f_base) + @as(i32, role_svf_f_off[v.role]) +
        @as(i32, cutoff_bias);
    const lfo = c.sin_table[v.lfo_phase >> 22];
    f_eff += (@as(i32, lfo) * @as(i32, lfo_filter_depth)) >> CUTOFF_LFO_SHIFT;
    if (v.role == ROLE_CHORD) {
        f_eff += (@as(i32, v.fenv_amp) * CUTOFF_FENV_GAIN) >> CUTOFF_FENV_SHIFT;
    }
    if (f_eff < 20) f_eff = 20;
    if (f_eff > 230) f_eff = 230;
    return f_eff;
}

/// During the measurement window, observe |lp| and recompute gain on
/// every new peak. The peak grows monotonically and gain falls with
/// it, so the ramp is click-free. After the window gain is fixed.
fn peakNormalize(v: *Voice, lp: i32) i32 {
    const abs_lp = if (lp < 0) -lp else lp;
    if (v.peak_window > 0) {
        if (abs_lp > @as(i32, v.peak_seen)) {
            v.peak_seen = @intCast(if (abs_lp > 0xFFFF) 0xFFFF else abs_lp);
            var g: u32 = @divTrunc(PEAK_TARGET * PEAK_GAIN_UNITY, v.peak_seen);
            if (g > PEAK_GAIN_MAX) g = PEAK_GAIN_MAX;
            v.gain = @intCast(g);
        }
        v.peak_window -= 1;
    }
    return (lp * @as(i32, v.gain)) >> 8;
}

/// kick 3x, snare 2.5x, hihat 1.5x. Low-frequency content needs more
/// amplitude to read as loud on small speakers.
fn drumBoost(v: *Voice, scaled: i32) i32 {
    if (v.role != ROLE_DRUM) return scaled;
    var numer: i32 = 3; // 1.5x for hihat
    if (v.u.drum.drum_type == DRUM_KICK) {
        numer = 6; // 3.0x
    } else if (v.u.drum.drum_type == DRUM_SNARE) {
        numer = 5; // 2.5x
    }
    return @divTrunc(scaled * numer, 2);
}

export fn voice_step(v: *Voice) i16 {
    if (v.env_phase == ENV_OFF) return 0;

    glideAdvance(v);
    const raw = oscDispatch(v);
    const env = envStep(v);
    fenvStep(v);
    const shaped: i16 = @truncate((@as(i32, raw) * @as(i32, env)) >> 15);

    const f_eff = computeCutoff(v);
    var q_eff: i32 = @as(i32, svf_q_base) + @as(i32, role_svf_q_off[v.role]);
    if (q_eff < 0) q_eff = 0;
    if (q_eff > 220) q_eff = 220;

    const hp = @as(i32, shaped) - v.svf_lp - ((v.svf_bp * q_eff) >> 8);
    const bp = v.svf_bp + ((hp * f_eff) >> 8);
    var lp = v.svf_lp + ((bp * f_eff) >> 8);
    v.svf_bp = bp;
    v.svf_lp = lp;

    const out: i32 = switch (filter_mode) {
        1 => hp,
        2 => bp,
        3 => hp + lp,
        else => lp,
    };
    lp = c.sat16(out);

    var scaled = peakNormalize(v, lp);
    scaled = drumBoost(v, scaled);
    return c.sat16(scaled);
}

var pool: ?[*]Voice = null;

/// Per-channel wheel position, -8192..8191, 0 = center. Audio-thread
/// only. Deliberately not reset by audio_midi_init: wheel position
/// persists across a MIDI close/re-open per MIDI semantics.
var midi_bend = [_]i16{0} ** 17;

export fn voice_pool_init() void {
    // Idempotent: only allocate on the first call so test binaries can
    // call this per TEST without draining the arena.
    if (pool == null) {
        const p = c.arena_alloc(N_VOICES * @sizeOf(Voice));
        pool = @ptrCast(@alignCast(p.?));
    }
    var i: usize = 0;
    while (i < N_VOICES) : (i += 1) voice_init(&pool.?[i]);
    i = 0;
    while (i < 17) : (i += 1) midi_bend[i] = 0;
}

/// Drum slots are dedicated per type, so kick/snare/hihat never steal
/// from each other.
const role_slot_start = [4]u8{ 0, 2, 5, 8 };
const role_slot_end = [4]u8{ 2, 5, 8, 11 };

fn pickSlotRange(lo: u8, hi: u8) usize {
    var i: usize = lo;
    while (i < hi) : (i += 1) {
        if (pool.?[i].env_phase == ENV_OFF) return i;
    }
    var chosen: usize = lo;
    var min_amp: u16 = 0xFFFF;
    i = lo;
    while (i < hi) : (i += 1) {
        if (pool.?[i].env_phase == ENV_R and pool.?[i].env_amp < min_amp) {
            min_amp = pool.?[i].env_amp;
            chosen = i;
        }
    }
    return chosen;
}

export fn voice_pool_trigger_role(note: u8, vtype: u8, role: u8) void {
    const slot = pickSlotRange(role_slot_start[role], role_slot_end[role]);
    voice_trigger(&pool.?[slot], note, vtype, role);

    // Place the voice on the stage: slot base pan plus jitter within
    // the role's range. PRNG advances here, so determinism holds.
    const base: i32 = slot_base_pan[slot];
    const jitter_range: i32 = role_pan_jitter[role];
    const jitter: i32 = prngNoise() >> 8; // roughly -128..127
    const j = @divTrunc(jitter * jitter_range, 128);
    var p = base + j;
    if (p < 0) {
        p = 0;
    } else if (p > 255) {
        p = 255;
    }
    pool.?[slot].pan = @intCast(p);
    pool.?[slot].lfo_phase = 0;
    pool.?[slot].lfo_inc = slot_lfo_inc[slot];
}

/// One dedicated slot per drum type: kick 8, snare 9, hihat 10.
export fn voice_pool_trigger_drum(drum_type: u8) void {
    if (drum_type > DRUM_HIHAT) return;
    const slot: usize = 8 + @as(usize, drum_type);
    const v = &pool.?[slot];

    v.type = VOICE_DRUM;
    v.note = drum_type;
    v.role = ROLE_DRUM;
    v.env_phase = ENV_A;
    v.env_time = 0;
    v.env_amp = 0;
    v.svf_lp = 0;
    v.svf_bp = 0;

    v.u.drum.drum_type = drum_type;
    v.u.drum.phase = 0;
    v.u.drum.inc = if (drum_type == DRUM_KICK) 13421773 else 0;

    v.peak_seen = 1;
    v.gain = 256;
    v.peak_window = 2400;

    const base: i32 = slot_base_pan[slot];
    const jitter: i32 = prngNoise() >> 8;
    const j = @divTrunc(jitter * @as(i32, role_pan_jitter[ROLE_DRUM]), 128);
    var p = base + j;
    if (p < 0) {
        p = 0;
    } else if (p > 255) {
        p = 255;
    }
    v.pan = @intCast(p);
    v.lfo_phase = 0;
    v.lfo_inc = 0;
}

export fn voice_pool_mix() Stereo {
    var sum_l: i32 = 0;
    var sum_r: i32 = 0;
    var i: usize = 0;
    while (i < N_VOICES) : (i += 1) {
        const v = &pool.?[i];
        const s = voice_step(v);
        if (s == 0 and v.env_phase == ENV_OFF) continue;

        // Slow LFO drifts pan around its base. sin_table is +/-24576;
        // >>9 gives roughly +/-48 pan units.
        v.lfo_phase +%= v.lfo_inc;
        const lfo: i32 = c.sin_table[v.lfo_phase >> 22];
        var p: i32 = @as(i32, v.pan) + (lfo >> 9);
        if (p < 0) {
            p = 0;
        } else if (p > 255) {
            p = 255;
        }

        // Linear pan: L gain = (255 - p), R gain = p, /255 via >>8.
        sum_l += (@as(i32, s) * (255 - p)) >> 8;
        sum_r += (@as(i32, s) * p) >> 8;
    }
    return .{
        .l = @truncate(sum_l >> 3),
        .r = @truncate(sum_r >> 3),
    };
}

export fn voice_pool_active_mask() u32 {
    var mask: u32 = 0;
    var i: usize = 0;
    while (i < N_VOICES) : (i += 1) {
        if (pool.?[i].env_phase != ENV_OFF) mask |= (@as(u32, 1) << @intCast(i));
    }
    return mask;
}

// Filter control. User-tunable base ranges are deliberately tighter
// than the effective-value clamps so LFO and filter-envelope
// modulation always have room at the top of the dial.

fn clampInt(v: c_int, lo: c_int, hi: c_int) c_int {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

export fn voice_adjust_cutoff(delta: c_int) void {
    svf_f_base = @intCast(clampInt(@as(c_int, svf_f_base) + delta, 30, 180));
}

export fn voice_adjust_resonance(delta: c_int) void {
    svf_q_base = @intCast(clampInt(@as(c_int, svf_q_base) + delta, 0, 180));
}

export fn voice_adjust_lfo_filter_depth(delta: c_int) void {
    lfo_filter_depth = @intCast(clampInt(@as(c_int, lfo_filter_depth) + delta, 0, 255));
}

export fn voice_cycle_filter_mode() void {
    filter_mode = (filter_mode +% 1) & 3;
}

export fn voice_set_cutoff(v: c_int) void {
    svf_f_base = @intCast(clampInt(v, 30, 180));
}

export fn voice_set_resonance(v: c_int) void {
    svf_q_base = @intCast(clampInt(v, 0, 180));
}

export fn voice_set_lfo_filter_depth(v: c_int) void {
    lfo_filter_depth = @intCast(clampInt(v, 0, 255));
}

export fn voice_set_filter_mode(m: c_int) void {
    filter_mode = @intCast(m & 3);
}

export fn voice_get_cutoff() u16 {
    return svf_f_base;
}
export fn voice_get_resonance() u16 {
    return svf_q_base;
}
export fn voice_get_lfo_filter_depth() u16 {
    return lfo_filter_depth;
}
export fn voice_get_filter_mode() u8 {
    return filter_mode;
}

/// Small random drift on cutoff and Q, from gen.c's mutate() with the
/// same PRNG word.
export fn voice_mutate_filter(rng: u32) void {
    const df: c_int = @as(c_int, @intCast((rng >> 16) & 0x1F)) - 16; // -16..+15
    const dq: c_int = @as(c_int, @intCast((rng >> 24) & 0x0F)) - 8; //  -8..+7
    voice_adjust_cutoff(df);
    voice_adjust_resonance(dq);
}

export fn voice_set_cutoff_bias(bias: i8) void {
    cutoff_bias = bias;
}

/// Selects a slot by the voice-stealing rules (idle first; quietest
/// in-release; oldest regardless), triggers FM/MELODY, then bypasses
/// peak-normalize and sets gain from velocity — env_step overwrites
/// env_amp every sample, so gain is what carries velocity through.
export fn voice_pool_trigger_midi(note: u8, velocity: u8, channel: u8) void {
    var chosen: i32 = -1;
    var i: usize = 0;

    while (i < N_VOICES) : (i += 1) {
        if (pool.?[i].env_phase == ENV_OFF) {
            chosen = @intCast(i);
            break;
        }
    }
    // Steal the quietest voice in release. env_time counts UP in
    // ENV_R, so the LARGEST env_time is closest to silence and the
    // least audible to steal.
    if (chosen == -1) {
        var max_t: u16 = 0;
        i = 0;
        while (i < N_VOICES) : (i += 1) {
            if (pool.?[i].env_phase == ENV_R and pool.?[i].env_time > max_t) {
                max_t = pool.?[i].env_time;
                chosen = @intCast(i);
            }
        }
    }
    // Fallback: oldest voice regardless of state.
    if (chosen == -1) {
        var min_time: u16 = 0xFFFF;
        i = 0;
        while (i < N_VOICES) : (i += 1) {
            if (pool.?[i].env_time < min_time) {
                min_time = pool.?[i].env_time;
                chosen = @intCast(i);
            }
        }
        if (chosen == -1) chosen = 0;
    }

    const slot: usize = @intCast(chosen);
    voice_trigger(&pool.?[slot], note, VOICE_FM, ROLE_MELODY);

    pool.?[slot].peak_window = 0;
    if (velocity == 0) {
        pool.?[slot].gain = 0;
    } else {
        var g: u32 = @divTrunc(@as(u32, velocity) * 256, 127);
        if (g < 64) g = 64; // FR-010 minimum audible clamp
        if (g > 1024) g = 1024; // PEAK_GAIN_MAX
        pool.?[slot].gain = @intCast(g);
    }

    pool.?[slot].trigger_key = note;
    pool.?[slot].trigger_channel = channel;

    // FR-015: new notes sound bent while the wheel is off-center.
    if (channel >= 1 and channel <= 16 and midi_bend[channel] != 0) {
        const inc = bentInc(note, midi_bend[channel]);
        pool.?[slot].u.fm.inc_c = inc;
        pool.?[slot].u.fm.inc_m = inc *% role_fm_ratio[ROLE_MELODY];
    }
}

/// Linear interpolation between note_phase_inc[k] and its +/-2
/// semitone neighbour — no new tables; max deviation from the true
/// 2^(x/12) curve is ~2.9 cents mid-span, inside fm_step's own ~5-cent
/// LFO excursion.
fn bentInc(note: u8, bend: i16) u32 {
    const base = c.note_phase_inc[note];
    if (bend == 0) return base;
    var k2: i32 = @as(i32, note) + (if (bend > 0) @as(i32, 2) else @as(i32, -2));
    if (k2 < 0) k2 = 0;
    if (k2 > 127) k2 = 127;
    // Negate AFTER widening (-8192 would overflow i16), and subtract
    // as i64 — a u32 subtraction wraps on every down-bend.
    const frac: i64 = if (bend > 0) bend else -@as(i64, bend);
    const d: i64 = @as(i64, c.note_phase_inc[@intCast(k2)]) - @as(i64, base);
    return @intCast(@as(i64, base) + @divTrunc(d * frac, 8192));
}

export fn voice_pool_bend_midi(channel: u8, bend: i16) void {
    // Guard 1..16: this walk matches on channel ALONE, so an
    // out-of-contract value must never run.
    if (channel < 1 or channel > 16) return;
    midi_bend[channel] = bend;
    var i: usize = 0;
    while (i < N_VOICES) : (i += 1) {
        const v = &pool.?[i];
        // Both guards required: env completion leaves stale trigger
        // tags on ENV_OFF voices, and the VOICE_FM check protects the
        // u.fm access. & 0x7F strips the held tag so pedal-held voices
        // rebend too. Phase accumulators untouched: frequency steps
        // are phase-continuous, no click.
        if ((v.trigger_channel & 0x7F) != channel) continue;
        if (v.type != VOICE_FM or v.env_phase == ENV_OFF) continue;
        const inc = bentInc(v.trigger_key, bend);
        v.u.fm.inc_c = inc;
        v.u.fm.inc_m = inc *% role_fm_ratio[ROLE_MELODY];
    }
}

/// Walks for the FIRST voice tagged (key, channel) in an ACTIVE phase.
/// Voices already in ENV_R are skipped so a still-sounding match in a
/// later slot is found — repeated Note Ons can leave one older release
/// plus one newer active voice on the same tag. Falling out of the
/// loop is the FR-013 no-op case.
export fn voice_pool_release_midi(key: u8, channel: u8) void {
    var i: usize = 0;
    while (i < N_VOICES) : (i += 1) {
        const v = &pool.?[i];
        if (v.env_phase == ENV_OFF) continue;
        if (v.trigger_channel != channel) continue;
        if (v.trigger_key != key) continue;
        if (v.env_phase == ENV_R) continue;
        v.env_phase = ENV_R;
        v.env_time = 0;
        return;
    }
}

/// CC#64 sustain. Same walk as release, but marks held instead of
/// entering ENV_R: the envelope stays where it is, so the note rings
/// at sustain until pedal-up.
export fn voice_pool_hold_midi(key: u8, channel: u8) void {
    var i: usize = 0;
    while (i < N_VOICES) : (i += 1) {
        const v = &pool.?[i];
        if (v.env_phase == ENV_OFF) continue;
        if (v.trigger_channel != channel) continue;
        if (v.trigger_key != key) continue;
        if (v.env_phase == ENV_R) continue;
        v.trigger_channel = channel | SUSTAIN_HELD_BIT;
        return;
    }
}

/// CC#123 All Notes Off, strict MIDI 1.0: acts as a Note Off per
/// sounding note. With the damper down a Note Off means HOLD, so
/// sounding notes convert to held. Held-tagged voices do not match the
/// plain walk, which is right — they already had their Note Off.
export fn voice_pool_release_all_midi(channel: u8, hold: c_int) void {
    if (channel < 1 or channel > 16) return;
    var i: usize = 0;
    while (i < N_VOICES) : (i += 1) {
        const v = &pool.?[i];
        if (v.trigger_channel != channel) continue;
        if (v.env_phase == ENV_OFF or v.env_phase == ENV_R) continue;
        if (hold != 0) {
            v.trigger_channel = channel | SUSTAIN_HELD_BIT;
        } else {
            v.env_phase = ENV_R;
            v.env_time = 0;
        }
    }
}

/// Pedal-up: release every voice held on this channel. The plain tag
/// is restored first so post-release matching sees a normal MIDI
/// voice.
export fn voice_pool_flush_sustained(channel: u8) void {
    const held: u8 = channel | SUSTAIN_HELD_BIT;
    var i: usize = 0;
    while (i < N_VOICES) : (i += 1) {
        const v = &pool.?[i];
        if (v.trigger_channel != held) continue;
        v.trigger_channel = channel;
        if (v.env_phase == ENV_OFF or v.env_phase == ENV_R) continue;
        v.env_phase = ENV_R;
        v.env_time = 0;
    }
}
