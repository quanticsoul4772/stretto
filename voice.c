#include "voice.h"
#include "arena.h"
#include "effects.h"
#include "sin_table.h"
#include "env_table.h"
#include "note_table.h"
#include "wavetable.h"
#include <stdint.h>

/* Sample counts calibrated for SAMPLE_RATE = 48000 (formerly 44100):
     220   ->  240   (5 ms attack)
     8820  ->  9600  (200 ms decay)
     26460 -> 28800  (600 ms release) */
#define ENV_ATTACK_SAMPLES   240
#define ENV_DECAY_SAMPLES    9600
#define ENV_RELEASE_SAMPLES  28800
#define ENV_SUSTAIN_LEVEL    16384
#define ENV_PEAK             32767

/* Per-voice peak normalization. After PEAK_WINDOW_SAMPLES of every
   trigger, each voice's output is scaled so its observed peak hits
   PEAK_TARGET. Gain is clamped between 1.0x and PEAK_GAIN_MAX (8.8 fp,
   so 1024 = 4.0x). */
#define PEAK_WINDOW_SAMPLES  2400    /* 50 ms at 48 kHz */
#define PEAK_TARGET          16000   /* per-voice peak after normalization;
                                        chosen so 3-4 simultaneous voices
                                        after the >>3 mix divide produce
                                        mid-range output level */
#define PEAK_GAIN_MAX        1024    /* 4.0x in 8.8 fixed point */
#define PEAK_GAIN_UNITY      256     /* 1.0x in 8.8 fixed point */

/* KS averaging-filter feedback coefficient. 32440/65536 ~= 0.9897.
   Higher values let the pluck ring longer; lower values dampen it
   faster. Calibrated empirically for the existing pluck character. */
#define KS_AVG_COEF       32440

/* Cutoff modulation scales for voice_step's f_eff composition.
   LFO contribution: sin_table peak 24576 * lfo_filter_depth (0..255)
   gives up to ~6.3M; >>15 shifts that into a +/-60-unit swing on
   the cutoff. Chord fenv contribution: fenv_amp peak 32767 * 30
   = ~983k; >>14 shifts into a 0..+60-unit opening sweep. Both
   stay well within the [20, 230] effective cutoff clamp. */
#define CUTOFF_LFO_SHIFT  15
#define CUTOFF_FENV_GAIN  30
#define CUTOFF_FENV_SHIFT 14

/* Portamento (glide) ramp length in samples and the envelope-amplitude
   threshold above which a re-trigger is considered legato. 2400 samples
   = 50 ms at 48 kHz: classic mono-synth slide, audibly a glide rather
   than a lazy attack, lands well before the next bass beat
   (~620 ms away). Threshold = ENV_SUSTAIN_LEVEL / 2 = 8192 keeps the
   ramp off when the previous note is already deep in its release tail
   (otherwise glide would fire on every consecutive bass trigger just
   because release length > inter-bass interval). */
#define GLIDE_SAMPLES        2400u
#define GLIDE_LEGATO_THRESH  8192u

/* SVF (2-pole Chamberlin) parameters - runtime-tunable. Base values,
   with per-role offsets and per-voice LFO modulation added inside
   voice_step. f=200 -> ~6.1 kHz at 48 kHz, Q=100 -> ~2.56. */
static uint16_t svf_f_base = 200;
static uint16_t svf_q_base = 100;
static int8_t   cutoff_bias = 0;     /* section-driven additive bias on f_eff */

/* Filter mode: 0 LP, 1 HP, 2 BP, 3 notch (LP+HP). */
static uint8_t  filter_mode = 0;

/* Per-voice-LFO depth driving cutoff (0..255 = 0..100% of base cutoff
   range). LFO is the existing pan LFO; this just scales how much it
   bends the cutoff. */
static uint16_t lfo_filter_depth = 80;

/* Per-role SVF base offset (signed). Bass darker, melody more open.
   Drums get a low cutoff so the noise hihat doesn't sizzle the mix. */
static const int16_t role_svf_f_off[4] = { -100, -40, 0, -120 };
static const int16_t role_svf_q_off[4] = {  -30,   0, 0,  -50 };

static uint32_t prng_state = 0xCAFEBABEu;
static uint16_t fm_mod_depth = 1500;

void voice_set_mod_depth(uint16_t d) {
    if (d < 100) d = 100;
    if (d > 8000) d = 8000;
    fm_mod_depth = d;
}

uint16_t voice_get_mod_depth(void) {
    return fm_mod_depth;
}

static int16_t prng_noise(void) {
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return (int16_t)(x >> 16);
}

/* Per-role parameters: BASS, CHORD, MELODY, DRUM. The DRUM row's
   release value is a fallback - env_step overrides it with a per-
   drum-type table (kick 150 ms / snare 100 ms / hihat 30 ms). */
static const uint16_t role_mod_depth[4]  = {  200, 1500, 1500,    0 };
static const uint8_t  role_fm_ratio[4]   = {    1,    2,    2,    1 };
static const uint16_t role_attack[4]     = { 2400,  960,  240,   24 }; /* 50/20/5/0.5 ms */
static const uint16_t role_release[4]    = {48000,28800,28800, 4800 }; /* 1000/600/600/100 ms */

/* Per-voice-slot base pan position (0 = full left, 128 = center,
   255 = full right). Drum slots: kick center, snare slight off-center,
   hihat slightly the other way. */
static const uint8_t slot_base_pan[N_VOICES] = {
    128, 128,            /* bass slots 0-1: center */
     72, 128, 184,       /* chord slots 2-4: L, C, R */
     56, 200,  96,       /* melody slots 5-7: alternating outer */
    128, 144, 112,       /* drum slots 8-10: kick C, snare +R, hihat -L */
};

/* Per-voice-slot LFO increment for slow pan motion (~0.07-0.18 Hz).
   Drum slots use 0 - no pan modulation on percussion. */
static const uint32_t slot_lfo_inc[N_VOICES] = {
     6263,  8946,        /* bass */
     9841,  7603,11178,  /* chord */
    13418,10737,16103,   /* melody */
        0,    0,    0,   /* drum: no LFO */
};

/* Pan jitter range per role: bass tight, chord medium, melody wide,
   drum minimal (drums should sound consistent in stereo position). */
static const uint8_t role_pan_jitter[4] = { 16, 32, 48, 8 };

void voice_init(Voice *v) {
    v->type = VOICE_OFF;
    v->note = 0;
    v->env_phase = ENV_OFF;
    v->role = ROLE_MELODY;
    v->pan = 128;
    v->env_amp = 0;
    v->env_time = 0;
    v->lfo_phase = 0;
    v->lfo_inc = 0;
    v->peak_seen = 1;
    v->gain = PEAK_GAIN_UNITY;
    v->peak_window = 0;
    v->fenv_amp = 0;
    v->fenv_time = 0;
    v->fenv_phase = ENV_OFF;
    v->svf_lp = 0;
    v->svf_bp = 0;
    v->inc_target = 0;
    v->glide_remain = 0;
}

/* Additive-synth profile table. 4 profiles x 8 partial amplitudes.
   See add_step below for the per-partial math. Defined here (rather
   than next to add_step) so voice_trigger's VOICE_ADD branch can
   point u.add.amps at row 0 on trigger. */
#define N_ADD_PROFILES 4
static const uint8_t ADD_PROFILES[N_ADD_PROFILES][8] = {
    /* Hammond */ {  64,  64,  32,  32,  32,  16,   8,   8 },  /* sum 256 */
    /* Square  */ { 128,   0,  64,   0,  32,   0,  16,   0 },  /* sum 240 */
    /* Strings */ { 128,  64,  32,  16,   8,   4,   2,   2 },  /* sum 256 */
    /* Brass   */ {  64,  64,  64,  48,  32,  16,   8,   4 },  /* sum 300 */
};

void voice_trigger(Voice *v, uint8_t note, uint8_t type, uint8_t role) {
    /* Legato glide: if the slot is currently holding a SUB bass note
       whose amplitude envelope has not fallen below GLIDE_LEGATO_THRESH,
       slide the existing oscillator inc to the new note's inc over
       GLIDE_SAMPLES instead of hard re-triggering. Phases, envelope,
       peak normalizer, SVF state all carry across so the result is one
       continuous voice with a swept pitch. */
    if (type == VOICE_SUB && role == ROLE_BASS
        && v->type == VOICE_SUB && v->role == ROLE_BASS
        && v->env_phase != ENV_OFF
        && v->env_amp > GLIDE_LEGATO_THRESH) {
        v->inc_target   = note_phase_inc[note];
        v->glide_remain = GLIDE_SAMPLES;
        v->note         = note;
        return;
    }

    v->type = type;
    v->note = note;
    v->role = role;
    v->env_phase = ENV_A;
    v->env_time = 0;
    v->env_amp = 0;
    v->svf_lp = 0;
    v->svf_bp = 0;
    /* Reset peak detection for this trigger. Starts at 1.0x gain;
       gain will decrease as the voice's peak grows during the window. */
    v->peak_seen = 1;
    v->gain = PEAK_GAIN_UNITY;
    v->peak_window = PEAK_WINDOW_SAMPLES;
    /* Reset chord filter envelope on every trigger so the cutoff
       sweep restarts for each chord change. */
    v->fenv_amp = 0;
    v->fenv_time = 0;
    v->fenv_phase = ENV_A;

    if (type == VOICE_KS) {
        v->u.ks.len = note_ks_len[note];
        v->u.ks.idx = 0;
        uint16_t len = v->u.ks.len;
        for (uint16_t i = 0; i < len; i++) {
            v->u.ks.buf[i] = (int16_t)(prng_noise() >> 1);
        }
    } else if (type == VOICE_DRUM) {
        /* Drum voices: note parameter holds the drum sub-type
           (DRUM_KICK/DRUM_SNARE/DRUM_HIHAT). Initialize drum union
           fields so drum_step reads valid state. */
        v->role = ROLE_DRUM;
        v->u.drum.drum_type = note;
        v->u.drum.phase = 0;
        /* Kick: sine starting at ~150 Hz, decays toward ~50 Hz over its
           envelope. inc = 150 * 2^32 / 48000 = 13,421,773.
           Snare/hihat use noise, so inc = 0. */
        v->u.drum.inc = (note == DRUM_KICK) ? 13421773u : 0u;
    } else if (type == VOICE_WT) {
        /* Wavetable: single phase accumulator at the note's carrier
           rate; position starts at 0 and is overwritten per sample in
           voice_step from the per-voice LFO. */
        v->u.wt.phase    = 0;
        v->u.wt.inc      = note_phase_inc[note];
        v->u.wt.position = 0;
    } else if (type == VOICE_ADD) {
        /* Additive: 8 phase accumulators, each at k*fundamental.
           First-cut profile is fixed (Hammond); a future PR can rotate
           profile per trigger / per section / via mutation. */
        for (int k = 0; k < 8; k++) v->u.add.phase[k] = 0;
        v->u.add.inc_base = note_phase_inc[note];
        v->u.add.amps     = ADD_PROFILES[0];
    } else if (type == VOICE_SUB) {
        /* Subtractive super-saw: 3 detuned band-limited saws.
           Detune ratio ~0.78% (inc >> 7) keeps beating slow enough to
           feel like phase movement rather than vibrato. Source wave is
           WAVETABLE[4] (band-limited 8-harmonic saw) so we add no new
           .rodata; the SVF further shapes the spectrum. */
        uint32_t base = note_phase_inc[note];
        v->u.sub.phase[0] = 0;
        v->u.sub.phase[1] = 0;
        v->u.sub.phase[2] = 0;
        v->u.sub.inc[0] = base;
        v->u.sub.inc[1] = base + (base >> 7);
        v->u.sub.inc[2] = base - (base >> 7);
        /* Fresh trigger (not legato): zero glide state so a prior ramp
           can't leak through. */
        v->inc_target   = 0;
        v->glide_remain = 0;
    } else {
        v->u.fm.phase_c   = 0;
        v->u.fm.phase_m   = 0;
        v->u.fm.inc_c     = note_phase_inc[note];
        v->u.fm.inc_m     = note_phase_inc[note] * role_fm_ratio[role];
        v->u.fm.mod_depth = (role == ROLE_MELODY) ? fm_mod_depth : role_mod_depth[role];
    }
}

static int16_t ks_step(Voice *v) {
    uint16_t idx  = v->u.ks.idx;
    uint16_t len  = v->u.ks.len;
    uint16_t next = idx + 1;
    if (next >= len) next = 0;
    int16_t a = v->u.ks.buf[idx];
    int16_t b = v->u.ks.buf[next];
    int16_t avg = (int16_t)((((int32_t)a + b) * KS_AVG_COEF) >> 16);
    v->u.ks.buf[idx] = avg;
    v->u.ks.idx = next;
    return a;
}

/* Additive synthesis step. 8 partials at integer multiples of the
   fundamental, each weighted by ADD_PROFILES[idx][k]. Output range
   is bounded because the profile amplitudes sum to ~256 and the
   sin_table peak is ~24576, so the >>8 puts the result in int16
   range; sat16 catches the rare overshoot at the brass profile
   (sum 300). Profile selection is one indirection (u.add.amps). */
static int16_t add_step(Voice *v) {
    int32_t out = 0;
    for (int k = 0; k < 8; k++) {
        int16_t s = sin_table[v->u.add.phase[k] >> 22];
        out += (int32_t)s * v->u.add.amps[k];
        v->u.add.phase[k] += v->u.add.inc_base * (uint32_t)(k + 1);
    }
    return sat16(out >> 8);
}

/* Wavetable oscillator. Reads from WAVETABLE[N_WT_WAVES][WT_WAVE_LEN]
   built at compile time by gen_wavetable.c. Position (0..N_WT_WAVES*256)
   selects between adjacent waves with linear interpolation; the
   selected mix is then sampled at phase>>22 as an 8-bit index into the
   256-sample wave. Position is derived from the per-voice pan LFO in
   voice_step so no extra modulator state is needed. */
static int16_t wt_step(Voice *v) {
    uint32_t pos       = v->u.wt.position;
    uint32_t wave_idx  = (pos >> 8) % N_WT_WAVES;
    uint32_t wave_frac = pos & 0xFFu;
    uint32_t next_idx  = (wave_idx + 1u) % N_WT_WAVES;
    uint32_t ph        = (v->u.wt.phase >> 22) & 0xFFu;
    int16_t a = WAVETABLE[wave_idx][ph];
    int16_t b = WAVETABLE[next_idx][ph];
    int16_t s = (int16_t)(a + (((int32_t)(b - a) * (int32_t)wave_frac) >> 8));
    v->u.wt.phase += v->u.wt.inc;
    return s;
}

/* Super-saw subtractive step. Sums 3 detuned band-limited saw
   oscillators (WAVETABLE[4]) and averages. Output then feeds the
   per-voice SVF for spectral shaping. Detune is static (no LFO
   modulation) - bass slots have lfo_inc = 0 anyway, and steady
   detuning is what gives the genre-defining "wide" character. */
static int16_t sub_step(Voice *v) {
    int32_t out = 0;
    for (int k = 0; k < 3; k++) {
        uint32_t ph = (v->u.sub.phase[k] >> 22) & 0xFFu;
        out += WAVETABLE[4][ph];
        v->u.sub.phase[k] += v->u.sub.inc[k];
    }
    return (int16_t)(out / 3);
}

static int16_t fm_step(Voice *v) {
    /* Re-use the per-voice pan LFO to also detune the carrier and
       modulator frequencies. Same scale on both so the FM ratio
       stays constant. Peak excursion: lfo (+/-24576) * inc / 2^23 ~=
       inc * 0.29%, about 5 cents at LFO peak - subtle chorus
       motion. Uses int64 for the multiply to avoid overflow on
       higher-pitched notes. */
    int16_t lfo = sin_table[v->lfo_phase >> 22];
    int32_t det_m = (int32_t)(((int64_t)v->u.fm.inc_m * lfo) >> 23);
    int32_t det_c = (int32_t)(((int64_t)v->u.fm.inc_c * lfo) >> 23);

    int16_t mod = sin_table[v->u.fm.phase_m >> 22];
    v->u.fm.phase_m += (uint32_t)((int32_t)v->u.fm.inc_m + det_m);
    uint32_t phase_with_mod = v->u.fm.phase_c + ((uint32_t)((int32_t)mod * v->u.fm.mod_depth) << 6);
    int16_t out = sin_table[phase_with_mod >> 22];
    v->u.fm.phase_c += (uint32_t)((int32_t)v->u.fm.inc_c + det_c);
    return out;
}

static uint16_t env_step(Voice *v) {
    uint32_t amp = v->env_amp;
    uint16_t attack_n  = role_attack[v->role];
    uint16_t release_n = role_release[v->role];

    /* Drum voices: per-drum-type release so hihat is short (30 ms),
       kick medium (150 ms), snare in between (100 ms). Attack stays
       at the role default (0.5 ms). */
    if (v->type == VOICE_DRUM) {
        static const uint16_t drum_release[3] = { 7200, 4800, 1440 };
        if (v->u.drum.drum_type < 3) release_n = drum_release[v->u.drum.drum_type];
    }

    switch (v->env_phase) {
        case ENV_A: {
            uint32_t idx = ((uint32_t)v->env_time * 255u) / attack_n;
            if (idx > 255u) idx = 255u;
            amp = ((uint32_t)env_table[idx] * ENV_PEAK) / 255u;
            v->env_time++;
            if (v->env_time >= attack_n) {
                /* Drums skip the decay/sustain phases entirely - they
                   go straight from full peak into release for a one-shot
                   percussive envelope. */
                v->env_phase = (v->type == VOICE_DRUM) ? ENV_R : ENV_D;
                v->env_time = 0;
                amp = ENV_PEAK;
            }
            break;
        }
        case ENV_D: {
            uint32_t idx = ((uint32_t)v->env_time * 255u) / ENV_DECAY_SAMPLES;
            if (idx > 255u) idx = 255u;
            uint32_t curve = 255u - env_table[idx];
            amp = ENV_SUSTAIN_LEVEL + ((uint32_t)(ENV_PEAK - ENV_SUSTAIN_LEVEL) * curve) / 255u;
            v->env_time++;
            if (v->env_time >= ENV_DECAY_SAMPLES) {
                v->env_phase = ENV_R;
                v->env_time = 0;
                amp = ENV_SUSTAIN_LEVEL;
            }
            break;
        }
        case ENV_R: {
            if (v->type == VOICE_DRUM) {
                /* Drums: linear decay from PEAK to 0 over release_n
                   samples. Simple and gives natural drum tail. */
                if (v->env_time >= release_n) {
                    amp = 0;
                    v->env_phase = ENV_OFF;
                    v->type = VOICE_OFF;
                } else {
                    amp = (uint32_t)ENV_PEAK * (release_n - v->env_time) / release_n;
                }
                v->env_time++;
            } else {
                uint32_t idx = ((uint32_t)v->env_time * 255u) / release_n;
                if (idx > 255u) idx = 255u;
                uint32_t curve = 255u - env_table[idx];
                amp = ((uint32_t)ENV_SUSTAIN_LEVEL * curve) / 255u;
                v->env_time++;
                if (v->env_time >= release_n) {
                    v->env_phase = ENV_OFF;
                    v->type = VOICE_OFF;
                    amp = 0;
                }
            }
            break;
        }
        default:
            amp = 0;
    }

    v->env_amp = (uint16_t)amp;
    return (uint16_t)amp;
}

/* Drum voice: cheap percussion synthesis. Three sub-types selected
   by u.drum.drum_type:
     KICK  - sine sweep + brief noise click on attack for cut-through
     SNARE - white noise (loud) + ~200 Hz sine body
     HIHAT - white noise only */
static int16_t drum_step(Voice *v) {
    if (v->u.drum.drum_type == DRUM_KICK) {
        int16_t body = sin_table[v->u.drum.phase >> 22];
        v->u.drum.phase += v->u.drum.inc;
        /* Pitch decay: drop inc by ~1/4096 per sample. */
        v->u.drum.inc -= v->u.drum.inc >> 12;
        /* Attack click: first ~5 ms (240 samples) blend a noise
           burst with the sine. Adds high-frequency content so the
           kick is audible on speakers with weak bass response. */
        if (v->env_time < 240) {
            int16_t click = (int16_t)prng_noise();
            return (int16_t)(((int32_t)body + click) / 2);
        }
        return body;
    }
    if (v->u.drum.drum_type == DRUM_SNARE) {
        /* Noise-dominant snare (90/10 noise/tone) so the crack
           cuts through on any speaker. */
        int16_t noise = (int16_t)prng_noise();
        int16_t tone = sin_table[v->u.drum.phase >> 22];
        v->u.drum.phase += 17895697u;
        return (int16_t)(((int32_t)noise * 9 + (int32_t)tone) / 10);
    }
    /* DRUM_HIHAT */
    return (int16_t)prng_noise();
}

/* Advance the chord filter envelope (A 5 ms / D 200 ms / R 600 ms,
   reusing the amplitude env's timing constants). Returns 0..32767. */
static void fenv_step(Voice *v) {
    if (v->role != ROLE_CHORD) return;
    uint32_t amp = v->fenv_amp;
    switch (v->fenv_phase) {
        case ENV_A: {
            uint32_t idx = ((uint32_t)v->fenv_time * 255u) / ENV_ATTACK_SAMPLES;
            if (idx > 255u) idx = 255u;
            amp = ((uint32_t)env_table[idx] * ENV_PEAK) / 255u;
            v->fenv_time++;
            if (v->fenv_time >= ENV_ATTACK_SAMPLES) {
                v->fenv_phase = ENV_D;
                v->fenv_time = 0;
                amp = ENV_PEAK;
            }
            break;
        }
        case ENV_D: {
            uint32_t idx = ((uint32_t)v->fenv_time * 255u) / ENV_DECAY_SAMPLES;
            if (idx > 255u) idx = 255u;
            uint32_t curve = 255u - env_table[idx];
            amp = ENV_SUSTAIN_LEVEL + ((uint32_t)(ENV_PEAK - ENV_SUSTAIN_LEVEL) * curve) / 255u;
            v->fenv_time++;
            if (v->fenv_time >= ENV_DECAY_SAMPLES) {
                v->fenv_phase = ENV_R;
                v->fenv_time = 0;
                amp = ENV_SUSTAIN_LEVEL;
            }
            break;
        }
        case ENV_R: {
            uint32_t idx = ((uint32_t)v->fenv_time * 255u) / ENV_RELEASE_SAMPLES;
            if (idx > 255u) idx = 255u;
            uint32_t curve = 255u - env_table[idx];
            amp = ((uint32_t)ENV_SUSTAIN_LEVEL * curve) / 255u;
            v->fenv_time++;
            if (v->fenv_time >= ENV_RELEASE_SAMPLES) {
                v->fenv_phase = ENV_OFF;
                amp = 0;
            }
            break;
        }
        default: amp = 0;
    }
    v->fenv_amp = (uint16_t)amp;
}

int16_t voice_step(Voice *v) {
    if (v->env_phase == ENV_OFF) return 0;
    /* Glide ramp. Linear walk by remaining-sample division so the
       result lands exactly at inc_target when glide_remain hits 0.
       Detune is rebuilt from the new base each step so the super-saw
       character holds across the slide. */
    if (v->glide_remain > 0 && v->type == VOICE_SUB) {
        uint32_t cur   = v->u.sub.inc[0];
        int32_t  delta = (int32_t)(v->inc_target - cur) / (int32_t)v->glide_remain;
        uint32_t new_base = (uint32_t)((int32_t)cur + delta);
        v->u.sub.inc[0] = new_base;
        v->u.sub.inc[1] = new_base + (new_base >> 7);
        v->u.sub.inc[2] = new_base - (new_base >> 7);
        v->glide_remain--;
    }
    int16_t raw;
    if (v->type == VOICE_KS)        raw = ks_step(v);
    else if (v->type == VOICE_FM)   raw = fm_step(v);
    else if (v->type == VOICE_DRUM) raw = drum_step(v);
    else if (v->type == VOICE_WT) {
        /* Map the existing pan LFO (~0.07-0.18 Hz) onto the wavetable
           position so the timbre sweeps through all 8 waveforms over
           ~10 s. Zero extra modulator state. */
        v->u.wt.position = (uint16_t)(((uint32_t)v->lfo_phase >> 16) * (N_WT_WAVES * 256u) >> 16);
        raw = wt_step(v);
    }
    else if (v->type == VOICE_ADD)  raw = add_step(v);
    else if (v->type == VOICE_SUB)  raw = sub_step(v);
    else                            raw = 0;
    uint16_t env = env_step(v);
    fenv_step(v);
    int16_t shaped = (int16_t)(((int32_t)raw * env) >> 15);

    /* Compute effective cutoff. The Chamberlin SVF is stable for
       f * pi / 2 < 1, i.e. f < 256/pi ~= 81 in our >>8 units... but
       in practice with int32 state and Q ~= 2.5 it stays sensible up
       to ~230. Clamp at 220 to leave margin. */
    int32_t f_eff = (int32_t)svf_f_base + role_svf_f_off[v->role]
                  + (int32_t)cutoff_bias;
    {
        /* LFO contributes up to +/-60 units (sin peak 24576 * 80 / 32768). */
        int16_t lfo = sin_table[v->lfo_phase >> 22];
        f_eff += ((int32_t)lfo * lfo_filter_depth) >> CUTOFF_LFO_SHIFT;
    }
    if (v->role == ROLE_CHORD) {
        /* Filter env opens the cutoff by up to +60 units at peak
           (fenv_amp 32767 * 30 / 16384 ~= 60). Subtle sweep. */
        f_eff += ((int32_t)v->fenv_amp * CUTOFF_FENV_GAIN) >> CUTOFF_FENV_SHIFT;
    }
    /* Effective f_eff range [20, 230] gives 50 units of headroom above
       the user's base ceiling (180) for LFO sweep and filter-envelope
       motion to land without clipping. */
    if (f_eff < 20)  f_eff = 20;
    if (f_eff > 230) f_eff = 230;

    int32_t q_eff = (int32_t)svf_q_base + role_svf_q_off[v->role];
    if (q_eff < 0)   q_eff = 0;
    if (q_eff > 220) q_eff = 220;

    int32_t hp = shaped - v->svf_lp - ((v->svf_bp * q_eff) >> 8);
    int32_t bp = v->svf_bp + ((hp * f_eff) >> 8);
    int32_t lp = v->svf_lp + ((bp * f_eff) >> 8);
    v->svf_bp = bp;
    v->svf_lp = lp;

    /* Select filter mode output: LP / HP / BP / notch. */
    int32_t out;
    switch (filter_mode) {
        case 1: out = hp; break;
        case 2: out = bp; break;
        case 3: out = hp + lp; break;
        default: out = lp; break;
    }
    /* Reassign to the variable the rest of voice_step works with. */
    lp = sat16(out);

    /* Peak-normalize. During the measurement window, observe |lp| and
       recompute gain whenever a new peak is found. Gain can be below
       1.0x (loud voices like chord get attenuated) or above 1.0x (quiet
       voices like bass get boosted, up to PEAK_GAIN_MAX). The peak grows
       monotonically and gain decreases with it, so the gain ramp is
       smooth (no clicks). After the window, gain is fixed for the rest
       of the voice's life. */
    int32_t abs_lp = lp < 0 ? -lp : lp;
    if (v->peak_window > 0) {
        if ((uint16_t)abs_lp > v->peak_seen) {
            v->peak_seen = (uint16_t)(abs_lp > 0xFFFF ? 0xFFFF : abs_lp);
            uint32_t g = ((uint32_t)PEAK_TARGET * PEAK_GAIN_UNITY) / v->peak_seen;
            if (g > PEAK_GAIN_MAX) g = PEAK_GAIN_MAX;
            v->gain = (uint16_t)g;
        }
        v->peak_window--;
    }
    int32_t scaled = ((int32_t)lp * v->gain) >> 8;

    /* Per-drum-type post-normalization boost: kick 3x, snare 2.5x,
       hihat 1.5x. Low-frequency content (kick body) needs more
       amplitude to be perceived loud on small speakers, and snare
       needs to cut through the harmonic content. */
    if (v->role == ROLE_DRUM) {
        int numer = 3;   /* default 1.5x = 3/2 for hihat */
        if (v->u.drum.drum_type == DRUM_KICK)       numer = 6;  /* 3.0x */
        else if (v->u.drum.drum_type == DRUM_SNARE) numer = 5;  /* 2.5x */
        scaled = (scaled * numer) / 2;
    }

    return sat16(scaled);
}

static Voice *pool;

void voice_pool_init(void) {
    /* Idempotent: only arena-allocate on the first call so test
       binaries can call this from each TEST without draining the
       arena. Re-init zeros the voice fields either way. */
    if (!pool) pool = arena_alloc(N_VOICES * sizeof(Voice));
    for (int i = 0; i < N_VOICES; i++) voice_init(&pool[i]);
}

/* Voice slot ranges per role. Drum slots are dedicated per drum type
   so kick/snare/hihat each have their own slot - no stealing within
   the drum kit. */
static const uint8_t role_slot_start[4] = { 0, 2, 5,  8 };
static const uint8_t role_slot_end[4]   = { 2, 5, 8, 11 };

static int pick_slot_range(uint8_t lo, uint8_t hi) {
    for (int i = lo; i < hi; i++) {
        if (pool[i].env_phase == ENV_OFF) return i;
    }
    int chosen = lo;
    uint16_t min_amp = 0xFFFFu;
    for (int i = lo; i < hi; i++) {
        if (pool[i].env_phase == ENV_R && pool[i].env_amp < min_amp) {
            min_amp = pool[i].env_amp;
            chosen = i;
        }
    }
    return chosen;
}

void voice_pool_trigger_role(uint8_t note, uint8_t type, uint8_t role) {
    int slot = pick_slot_range(role_slot_start[role], role_slot_end[role]);
    voice_trigger(&pool[slot], note, type, role);

    /* Place this voice on the stage: base pan from its slot, plus a
       random jitter within the role's range. PRNG state advances here
       so determinism is preserved across runs of the same binary. */
    int base = slot_base_pan[slot];
    int jitter_range = role_pan_jitter[role];
    int jitter = (int)(prng_noise() >> 8);    /* int16 -> roughly -128..127 */
    int j = (jitter * jitter_range) / 128;     /* scale to +/- range */
    int p = base + j;
    if (p < 0) p = 0;
    else if (p > 255) p = 255;
    pool[slot].pan = (uint8_t)p;
    pool[slot].lfo_phase = 0;
    pool[slot].lfo_inc = slot_lfo_inc[slot];
}

/* Drum trigger: one dedicated slot per drum_type (kick = 8, snare = 9,
   hihat = 10). Sets the voice's drum sub-type and seeds kick pitch
   sweep / leaves snare/hihat to noise. Per-drum envelope timings:
     kick:  attack 1 ms, release 150 ms (low thump)
     snare: attack 0.5 ms, release 100 ms (noise+sine crack)
     hihat: attack 0.5 ms, release 30 ms (very short tick) */
void voice_pool_trigger_drum(uint8_t drum_type) {
    if (drum_type > DRUM_HIHAT) return;
    int slot = 8 + drum_type;   /* 8=kick, 9=snare, 10=hihat */
    Voice *v = &pool[slot];

    v->type = VOICE_DRUM;
    v->note = drum_type;
    v->role = ROLE_DRUM;
    v->env_phase = ENV_A;
    v->env_time = 0;
    v->env_amp = 0;
    v->svf_lp = 0;
    v->svf_bp = 0;

    /* Per-drum-type envelope release is in env_step (drum_release[]).
       Attack uses role_attack[ROLE_DRUM] = 24 samples (0.5 ms) for
       all three drums. */

    /* Kick: sine starting at ~150 Hz, decays toward ~50 Hz over its
       envelope. inc = 150 * 2^32 / 48000 = 13,421,773. */
    v->u.drum.drum_type = drum_type;
    v->u.drum.phase = 0;
    v->u.drum.inc = (drum_type == DRUM_KICK) ? 13421773u : 0u;

    v->peak_seen = 1;
    v->gain = 256;
    v->peak_window = 2400;

    /* Pan: drum slot base + small jitter. */
    int base = slot_base_pan[slot];
    int jitter = (int)(prng_noise() >> 8);
    int j = (jitter * role_pan_jitter[ROLE_DRUM]) / 128;
    int p = base + j;
    if (p < 0) p = 0;
    else if (p > 255) p = 255;
    v->pan = (uint8_t)p;
    v->lfo_phase = 0;
    v->lfo_inc = 0;
}

Stereo voice_pool_mix(void) {
    int32_t sum_l = 0;
    int32_t sum_r = 0;
    for (int i = 0; i < N_VOICES; i++) {
        Voice *v = &pool[i];
        int16_t s = voice_step(v);
        if (s == 0 && v->env_phase == ENV_OFF) continue;

        /* Slow LFO modulates pan around its base. sin_table entry is
           +/-24576; >> 9 gives roughly +/-48 pan units of drift. */
        v->lfo_phase += v->lfo_inc;
        int lfo = sin_table[v->lfo_phase >> 22];
        int p = (int)v->pan + (lfo >> 9);
        if (p < 0) p = 0;
        else if (p > 255) p = 255;

        /* Linear pan: L gain = (255 - p), R gain = p, divide by 255
           via >> 8 (treats 255 as 256; -6 dB center is acceptable). */
        sum_l += ((int32_t)s * (255 - p)) >> 8;
        sum_r += ((int32_t)s * p) >> 8;
    }
    Stereo out;
    out.l = (int16_t)(sum_l >> 3);
    out.r = (int16_t)(sum_r >> 3);
    return out;
}

uint32_t voice_pool_active_mask(void) {
    uint32_t mask = 0;
    for (int i = 0; i < N_VOICES; i++) {
        if (pool[i].env_phase != ENV_OFF) mask |= (1u << i);
    }
    return mask;
}

/* Filter control API. Clamps keep the SVF stable - too-high f or q
   cause the state to overflow or self-oscillate uncontrollably even
   with int32 state. */
/* User-tunable base ranges are intentionally tighter than the
   effective-value clamps so LFO and filter-envelope modulation
   always have room at the top of the dial. */
void voice_adjust_cutoff(int delta) {
    int v = (int)svf_f_base + delta;
    if (v < 30)  v = 30;
    if (v > 180) v = 180;     /* leaves 50 units headroom for LFO+fenv */
    svf_f_base = (uint16_t)v;
}

void voice_adjust_resonance(int delta) {
    int v = (int)svf_q_base + delta;
    if (v < 0)   v = 0;
    if (v > 180) v = 180;     /* above ~180 the filter screeches */
    svf_q_base = (uint16_t)v;
}

void voice_adjust_lfo_filter_depth(int delta) {
    int v = (int)lfo_filter_depth + delta;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    lfo_filter_depth = (uint16_t)v;
}

void voice_cycle_filter_mode(void) {
    filter_mode = (uint8_t)((filter_mode + 1u) & 3u);
}

uint16_t voice_get_cutoff(void)           { return svf_f_base; }
uint16_t voice_get_resonance(void)        { return svf_q_base; }
uint16_t voice_get_lfo_filter_depth(void) { return lfo_filter_depth; }
uint8_t  voice_get_filter_mode(void)      { return filter_mode; }

/* Mutate filter params: small random drift to cutoff and Q. Called
   from gen.c mutate() with the same PRNG word. */
void voice_mutate_filter(uint32_t rng) {
    int df = (int)((rng >> 16) & 0x1F) - 16;   /* -16..+15 */
    int dq = (int)((rng >> 24) & 0x0F) - 8;    /*  -8..+7  */
    voice_adjust_cutoff(df);
    voice_adjust_resonance(dq);
}

void voice_set_cutoff_bias(int8_t bias) {
    cutoff_bias = bias;
}
