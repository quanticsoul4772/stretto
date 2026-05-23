#include "section.h"
#include <stdint.h>
#include <stddef.h>

/* Weak stub so unit tests (which do not link main.o) still resolve
   the reverb-bias setter that gen.c calls. main.c provides the
   real, strong definition; the linker picks it when present. */
__attribute__((weak)) void main_set_reverb_wet_bias(int8_t bias) {
    (void)bias;
}

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
