#include "section.h"
#include "voice.h"      /* for VOICE_FM / VOICE_WT enum values */
#include <stdint.h>
#include <stddef.h>

/* Per-section parameter biases. Indexed by section (INTRO, BODY,
   TENSION, RESOLVE). Continuous biases are signed deltas applied
   on top of the user's live-tunable parameter values. */

/* gate_prob delta (-64 = sparser, +32 = denser, etc.) */
static const int8_t BIAS_GATE[SECTION_COUNT]   = { -64,   0, +32, -16 };
/* SVF cutoff delta in cutoff units */
static const int8_t BIAS_CUTOFF[SECTION_COUNT] = { -40,   0, +30, -10 };
/* Reverb wet delta (signed, applied on top of user wet) */
static const int8_t BIAS_REVERB[SECTION_COUNT] = { +40,   0, -20, +20 };
/* Bars-until-mutate delta (positive = stiller, negative = busier) */
static const int8_t BIAS_MUT[SECTION_COUNT]    = {  +8,   0,  -4,  +4 };

/* Discrete: kick pattern index (0..3) per section. */
static const uint8_t KICK_PATTERN[SECTION_COUNT]    = { 0, 0, 2, 0 };
/* Discrete: L-system character index (0..2) per section.
   INTRO -> sparse (2), BODY -> stepwise (0), TENSION -> leaping (1),
   RESOLVE -> stepwise (0). */
static const uint8_t LSYS_CHARACTER[SECTION_COUNT]  = { 2, 0, 1, 0 };

static const char *SECTION_NAMES[SECTION_COUNT] = {
    "intro", "body", "tens", "res"
};

static uint32_t current_bar = 0;

void section_init(void) {
    current_bar = 0;
}

void section_step(uint32_t bar) {
    current_bar = bar;
}

uint8_t section_current(void) {
    uint32_t cycle_bar = current_bar % SECTION_PERIOD_BARS;
    return (uint8_t)(cycle_bar / SECTION_LEN_BARS);
}

const char *section_name(void) {
    return SECTION_NAMES[section_current()];
}

/* Linear interpolation helper for signed biases. The crossfade
   window is centered on the section boundary: the last fade_half
   bars of a section blend toward the next, and the first fade_half
   bars of the next section finish the blend. At the boundary
   exactly, the bias is halfway between adjacent sections.
   Outside the window the unblended section value is returned. */
static int8_t lerp_bias(const int8_t *table) {
    const uint32_t fade_half = SECTION_FADE_BARS / 2u;
    uint32_t cycle_bar = current_bar % SECTION_PERIOD_BARS;
    uint32_t section   = cycle_bar / SECTION_LEN_BARS;
    uint32_t pos       = cycle_bar % SECTION_LEN_BARS;
    int prev_sec = (int)((section + SECTION_COUNT - 1u) % SECTION_COUNT);
    int next_sec = (int)((section + 1u) % SECTION_COUNT);
    int cur = table[section];

    if (pos < fade_half) {
        /* Entering: blend from prev -> cur, half complete at pos=0
           (w=128), reaching full cur at pos=fade_half (w=256). */
        int w = (int)(pos + fade_half) * 256 / (int)(2u * fade_half);
        return (int8_t)(((int)table[prev_sec] * (256 - w)
                       + cur * w) >> 8);
    }
    if (pos >= SECTION_LEN_BARS - fade_half) {
        /* Leaving: blend from cur -> next, w=0 at start of leave
           window, w=128 at the boundary (pos=SECTION_LEN_BARS - 1). */
        int k = (int)(pos - (SECTION_LEN_BARS - fade_half));
        int w = k * 256 / (int)(2u * fade_half);
        return (int8_t)((cur * (256 - w)
                       + (int)table[next_sec] * w) >> 8);
    }
    return (int8_t)cur;
}

int8_t section_bias_gate(void)               { return lerp_bias(BIAS_GATE); }
int8_t section_bias_cutoff(void)             { return lerp_bias(BIAS_CUTOFF); }
int8_t section_bias_reverb(void)             { return lerp_bias(BIAS_REVERB); }
int8_t section_bias_mutation_interval(void)  { return lerp_bias(BIAS_MUT); }

uint8_t section_kick_pattern(void)     { return KICK_PATTERN[section_current()]; }
uint8_t section_lsystem_character(void){ return LSYS_CHARACTER[section_current()]; }

/* Chord voice type per section.
     INTRO    wavetable    (animated pad, position swept by LFO)
     BODY     additive     (organ-y Hammond pad, steady)
     TENSION  FM           (cutting glassy, percussive attack)
     RESOLVE  wavetable    (returns to the animated pad)
   Each section gets a distinct chord timbre; no section is silent. */
static const uint8_t SECTION_CHORD_VOICE[SECTION_COUNT] = {
    VOICE_WT, VOICE_ADD, VOICE_FM, VOICE_WT,
};
uint8_t section_chord_voice_type(void) { return SECTION_CHORD_VOICE[section_current()]; }

/* Chord arpeggio toggle per section.
     INTRO    block      (sparse pad-like)
     BODY     block      (steady chord pulse)
     TENSION  arpeggio   (energy, sequential note motion)
     RESOLVE  block      (settle back to pads)
   Discrete bias - switches instantly at section boundary like the
   voice-type selection above. */
static const uint8_t SECTION_CHORD_ARP[SECTION_COUNT] = { 0, 0, 1, 0 };
uint8_t section_chord_arpeggio(void) { return SECTION_CHORD_ARP[section_current()]; }

/* Per-section voice-family mask.
     INTRO    dynamic - one of INTRO_COMBOS[], chosen per cycle
     BODY     full ensemble
     TENSION  full ensemble
     RESOLVE  drumless (ambient close)
   INTRO's entry is a placeholder; section_voice_mask() substitutes the
   live combo. */
static const uint8_t SECTION_VOICE_MASK[SECTION_COUNT] = {
    VF_ALL,                                   /* INTRO (overridden) */
    VF_ALL,                                   /* BODY */
    VF_ALL,                                   /* TENSION */
    VF_ALL & (uint8_t)~(VF_KICK | VF_SNARE | VF_HAT),  /* RESOLVE: no drums */
};

/* Curated INTRO combos: 1-3 voice families each, all musically
   coherent. gen.c picks one per 96-bar cycle via PRNG. */
static const uint8_t INTRO_COMBOS[8] = {
    VF_CHORD,                          /* solo pad */
    VF_CHORD | VF_COUNTER,             /* pad + counter line */
    VF_CHORD | VF_HAT,                 /* pad + tick */
    VF_BASS  | VF_CHORD,               /* deep pad foundation */
    VF_BASS  | VF_HAT,                 /* pulse + tick */
    VF_BASS  | VF_COUNTER,             /* bass + sparse line */
    VF_CHORD | VF_MELODY | VF_HAT,     /* melody-led trio */
    VF_BASS  | VF_CHORD  | VF_HAT,     /* minimal full-stack trio */
};
static uint8_t intro_combo_idx = 0;

void section_set_intro_combo(uint8_t idx) { intro_combo_idx = idx & 7u; }

uint8_t section_voice_mask(void) {
    uint8_t sec = section_current();
    if (sec == SEC_INTRO) return INTRO_COMBOS[intro_combo_idx];
    return SECTION_VOICE_MASK[sec];
}
