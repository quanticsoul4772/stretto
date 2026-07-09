#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>

#define KS_MAX_LEN 512
#define N_VOICES   11

/* Voice synthesis types. KS and FM produce pitched notes; DRUM is a
   noise/sine percussion generator whose flavor depends on a sub-type
   passed through note: 0 = kick, 1 = snare, 2 = hihat. WT (wavetable)
   reads from a build-time table of 8 morphed waveforms with the
   position swept by the per-voice pan LFO for animated pad timbres.
   ADD (additive) sums 8 sinusoidal partials at integer harmonics with
   per-partial amplitudes from one of several drawbar-style profiles
   for organ / strings / brass character. SUB (subtractive) sums 3
   slightly-detuned band-limited saws (super-saw style) for a thick
   bass texture; sent through the existing per-voice SVF for filter
   shaping. */
enum { VOICE_OFF = 0, VOICE_KS, VOICE_FM, VOICE_DRUM, VOICE_WT, VOICE_ADD, VOICE_SUB };
enum { ENV_OFF = 0, ENV_A, ENV_D, ENV_R };
enum { ROLE_BASS = 0, ROLE_CHORD, ROLE_MELODY, ROLE_DRUM };
enum { DRUM_KICK = 0, DRUM_SNARE, DRUM_HIHAT };

typedef struct {
    int16_t l;
    int16_t r;
} Stereo;

typedef struct {
    uint8_t  type;
    uint8_t  note;
    uint8_t  env_phase;
    uint8_t  role;
    /* Stereo placement: 0 = full left, 128 = center, 255 = full right.
       LFO modulates around this base for slow continuous movement. */
    uint8_t  pan;
    uint8_t  _pad[3];
    uint16_t env_amp;
    uint16_t env_time;
    uint32_t lfo_phase;
    uint32_t lfo_inc;
    /* Per-voice peak normalization. peak_seen tracks max |output| during
       the first PEAK_WINDOW samples; gain is recomputed each time a new
       peak is found so it monotonically decreases as the peak grows.
       After window, gain stays fixed for the rest of the voice's life. */
    uint16_t peak_seen;
    uint16_t gain;            /* 8.8 fixed: 256 = 1.0x, 1024 = 4.0x cap */
    uint16_t peak_window;     /* samples remaining in the measurement window */
    uint16_t _norm_pad;
    /* Per-voice filter envelope (chord voices only - unused otherwise).
       Same ADSR shape as the amplitude env but feeds cutoff modulation:
       on trigger fenv opens the filter; during release it closes it. */
    uint16_t fenv_amp;
    uint16_t fenv_time;
    uint8_t  fenv_phase;
    uint8_t  _fenv_pad[3];
    /* SVF state is int32, not int16. At Q ~ 2.56 (q=100, damp=q/256),
       resonance can ring the filter state to roughly 2.5x input
       amplitude; int16 would wrap and produce broadband clicks. */
    int32_t  svf_lp;
    int32_t  svf_bp;
    /* Portamento/glide: target phase increment and ramp-sample count.
       When glide_remain > 0, voice_step linearly walks the per-voice
       oscillator inc toward inc_target each sample, landing exactly
       at inc_target when glide_remain hits zero. Currently only armed
       for VOICE_SUB + ROLE_BASS legato re-triggers. */
    uint32_t inc_target;
    uint16_t glide_remain;
    /* MIDI discriminator (003-midi-input). Reuses the 2 bytes that
       were _glide_pad so the Voice struct grows by ZERO bytes
       (preflight D4 refinement, commit d41d76a).

       trigger_key=0 + trigger_channel=0 signals "this voice is owned
       by the generative scheduler" (default after voice_init()).

       trigger_key in [0..127] + trigger_channel in [1..16] tag the
       voice as MIDI-triggered so voice_pool_release_midi can match
       (key, channel) tuples for Note Off routing (per FR-012). */
    uint8_t  trigger_key;
    uint8_t  trigger_channel;
    union {
        struct {
            int16_t  buf[KS_MAX_LEN];
            uint16_t idx;
            uint16_t len;
        } ks;
        struct {
            uint32_t phase_c;
            uint32_t phase_m;
            uint32_t inc_c;
            uint32_t inc_m;
            uint16_t mod_depth;
        } fm;
        struct {
            uint32_t phase;       /* sine phase for kick body */
            uint32_t inc;         /* sine increment - decays for pitch sweep */
            uint8_t  drum_type;   /* 0 kick, 1 snare, 2 hihat */
        } drum;
        struct {
            uint32_t phase;       /* same accumulator semantics as VoiceFm */
            uint32_t inc;
            uint16_t position;    /* 0..(N_WT_WAVES-1)*256; lerps between */
                                  /* adjacent waves at position>>8 + frac */
        } wt;
        struct {
            uint32_t phase[8];    /* one phase per partial */
            uint32_t inc_base;    /* fundamental phase increment */
            const uint8_t *amps;  /* points into ADD_PROFILES[][8] */
        } add;
        struct {
            uint32_t phase[3];    /* one phase per detuned oscillator */
            uint32_t inc[3];      /* base, base + ~0.78%, base - ~0.78% */
        } sub;
    } u;
} Voice;

void    voice_init(Voice *v);
void    voice_trigger(Voice *v, uint8_t note, uint8_t type, uint8_t role);
int16_t voice_step(Voice *v);

void    voice_pool_init(void);
void    voice_pool_trigger_role(uint8_t note, uint8_t type, uint8_t role);
void    voice_pool_trigger_drum(uint8_t drum_type);
Stereo  voice_pool_mix(void);

/* MIDI-triggered voice (003-midi-input). Non-role-scoped, polyphonic
   11 voices with voice stealing per Clarifications 2026-07-06 Q1
   (oldest in-release; oldest regardless fallback). Walks the full
   N_VOICES pool independently of the role-slot ranges used by the
   generative scheduler's voice_pool_trigger_role. The trigger_key +
   trigger_channel fields on Voice enable Note Off matching by
   (key, channel) tuple (FR-012). */
void voice_pool_trigger_midi(uint8_t note, uint8_t velocity, uint8_t channel);
void voice_pool_release_midi(uint8_t key, uint8_t channel);

void     voice_set_mod_depth(uint16_t d);
uint16_t voice_get_mod_depth(void);
uint32_t voice_pool_active_mask(void);

/* Filter controls (Phase A + B). All are global; per-role offsets and
   per-voice LFO modulation get added on top inside voice_step. */
void     voice_adjust_cutoff(int delta);
void     voice_adjust_resonance(int delta);
void     voice_adjust_lfo_filter_depth(int delta);
void     voice_cycle_filter_mode(void);
/* Absolute setters for the preset-capture CLI flags; clamps mirror
   the adjusters ([30,180] / [0,180] / [0,255] / &3). */
void     voice_set_cutoff(int v);
void     voice_set_resonance(int v);
void     voice_set_lfo_filter_depth(int v);
void     voice_set_filter_mode(int m);
uint16_t voice_get_cutoff(void);
uint16_t voice_get_resonance(void);
uint16_t voice_get_lfo_filter_depth(void);
uint8_t  voice_get_filter_mode(void);   /* 0 LP, 1 HP, 2 BP, 3 notch */

/* Filter param mutation hook (gen.c mutate() calls this with a small
   random delta on cutoff and resonance occasionally). */
void     voice_mutate_filter(uint32_t rng);

/* Section-driven additive cutoff bias. gen.c updates this per bar
   from section_bias_cutoff(). Applied on top of svf_f_base and the
   per-role offset and the LFO modulation, before the [20,230] clamp. */
void     voice_set_cutoff_bias(int8_t bias);

#endif
