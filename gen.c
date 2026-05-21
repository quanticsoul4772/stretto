#include "gen.h"
#include "voice.h"
#include "euclid_table.h"
#include <stdint.h>

#define BAR_STEPS         16

/* PLAN.md called for mutation every 16 bars. At our 2-second bar
   (16 steps * 5512 samples / 44100), 16 bars is ~32 s between
   mutations - too slow to feel evolving in a short listening window
   and never fires inside the 16-second regression render. Reduced
   to 4 bars (~8 s); still ~450 mutations per hour, plenty of
   variation at long timescales. */
#define MUTATE_BARS       4u

static uint32_t samples_per_step = 5512u;    /* ~125 ms = 120 BPM 16th notes */

static uint32_t gen_prng_state = 0xDEADBEEFu;
static uint32_t prng(void) {
    uint32_t x = gen_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gen_prng_state = x;
    return x;
}

/* Six modes/scales rooted on D, all 7 degrees in one octave starting
   from D4 (MIDI 62). Markov runs on degree indices (0..6) so the same
   transition matrix works for any 7-note scale; only the degree-to-
   MIDI mapping changes when scale switches.
     0 = Dorian          - D minor with raised 6  (starting mode)
     1 = Lydian          - D major with raised 4  (major triad on 0/2/4)
     2 = Phrygian        - D minor with lowered 2 (darkest minor mode)
     3 = Locrian         - half-diminished, very dark, unstable tonic
     4 = Harmonic Minor  - minor with raised 7, exotic / Middle-Eastern feel
     5 = Mixolydian      - major with lowered 7, bluesy / modal pop */
#define N_SCALES 6
static const uint8_t SCALES[N_SCALES][7] = {
    /* Dorian         */ { 62, 64, 65, 67, 69, 71, 72 },
    /* Lydian         */ { 62, 64, 66, 68, 69, 71, 73 },
    /* Phrygian       */ { 62, 63, 65, 67, 69, 70, 72 },
    /* Locrian        */ { 62, 63, 65, 67, 68, 70, 72 },
    /* Harmonic Minor */ { 62, 64, 65, 67, 69, 70, 73 },
    /* Mixolydian     */ { 62, 64, 66, 67, 69, 71, 72 },
};
static uint8_t cur_scale = 0;

/* Auto-rotate every N bars (~64 s at default tempo). */
#define SCALE_ROTATE_BARS 32u

/* Chord voicings: each entry is (degree, octave_offset). Three notes
   per voicing fits the three chord voice slots (2..4). Patterns cycle
   per bar to add harmonic variety beyond the basic root-3rd-5th triad.
     triad    - standard 1-3-5
     seventh  - 1-3-7  (drops the 5 to make room for the 7)
     sus4     - 1-4-5  (suspends the 3, classic open feel)
     sus2     - 1-2-5  (lighter suspension)
     inv1     - 3-5-1' (first inversion, 3rd in bass position)
     inv2     - 5-1'-3' (second inversion, 5th in bass position)
   Inversions use octave_offset = 1 to push the root and 3rd up an
   octave for a different voicing height. */
typedef struct { int8_t degree; int8_t octave; } ChordNote;

#define N_CHORD_PATTERNS 6
static const ChordNote CHORD_PATTERNS[N_CHORD_PATTERNS][3] = {
    /* triad   */ {{0,0}, {2,0}, {4,0}},
    /* seventh */ {{0,0}, {2,0}, {6,0}},
    /* sus4    */ {{0,0}, {3,0}, {4,0}},
    /* sus2    */ {{0,0}, {1,0}, {4,0}},
    /* inv1    */ {{2,0}, {4,0}, {0,1}},
    /* inv2    */ {{4,0}, {0,1}, {2,1}},
};

/* Markov transition weights for D Dorian (7 scale degrees, indices
   0..6 = tonic, supertonic, mediant, subdominant, dominant,
   submediant, leading-tone-flat). Rows are "from" degree, columns
   "to" degree. Weight = unnormalised probability; markov_next sums
   weights restricted to the active mask, then walks.
   Tuning principles for the initial values:
     - Stepwise motion (cols at +/-1) gets moderate weight (3-4).
     - Strong cadences favoured: leading tone (deg 6) and dominant
       (deg 4) bias toward tonic (deg 0) - high column-0 values in
       rows 4 and 6 (5 each).
     - No self-transitions (diagonal is 0) - prevents stuck notes
       on a single degree.
     - Tonic (row 0) opens out broadly to all degrees except itself,
       slight bias to dominant (deg 4) for V-I framing.
   These weights are MUTATED at runtime by mutate() once per
   MUTATE_BARS, so the matrix drifts; the initial values are just a
   musical seed. */
static uint8_t markov[7][7] = {
    { 0, 4, 3, 2, 4, 1, 2 },
    { 5, 0, 3, 2, 1, 1, 0 },
    { 3, 4, 0, 4, 2, 1, 0 },
    { 2, 2, 3, 0, 4, 2, 1 },
    { 5, 1, 2, 3, 0, 3, 2 },
    { 1, 2, 1, 2, 3, 0, 3 },
    { 5, 1, 0, 1, 2, 2, 0 },
};

static uint8_t  cur_degree = 0;
static uint32_t ca_row     = 0x12345678u;
static uint32_t ca_harm    = 0x87654321u;
static uint8_t  eucl_k_a   = 3;
static uint8_t  eucl_k_b   = 5;
static uint32_t bar_count  = 0;
static uint32_t step_count = 0;
static uint32_t sample_clock = 0;
static uint32_t next_step  = 0;
/* Per-hit probability gate for the melody trigger. A Euclidean step
   only fires a note if (prng() % 256) < gate_prob. 256 = always fire,
   0 = never fire. Mutated occasionally so density drifts; tunable at
   runtime via the g / G keys. */
static uint8_t  gate_prob  = 200;

/* Two 1D cellular automata run in parallel, both as 32-bit rows.
   ca_row uses Rule 110 (class IV, "complex" - long-lived structures
   on a chaotic background) and contributes the scale-degree mask
   from its low 7 bits.
   ca_harm uses Rule 30 (class III, "chaotic" - statistically random
   bit stream) and contributes a harmonic-context filter that ANDs
   into the active mask.
   The pairing avoids two failure modes a single CA would hit:
     - Rule 110 alone tends to settle into long-period repeats.
     - Rule 30 alone is too random; no musical coherence.
   Combined: 110 supplies recurring structure, 30 supplies variation
   that prevents the structure from repeating verbatim. */
#define RULE_110 0x6Eu
#define RULE_30  0x1Eu

static uint32_t ca_step(uint32_t row, uint8_t rule) {
    uint32_t next = 0;
    for (int i = 0; i < 32; i++) {
        unsigned int left   = (row >> ((i + 1) & 31)) & 1u;
        unsigned int center = (row >> i) & 1u;
        unsigned int right  = (row >> ((i + 31) & 31)) & 1u;
        unsigned int p = (left << 2) | (center << 1) | right;
        if ((rule >> p) & 1u) next |= (1u << i);
    }
    return next;
}

static uint8_t markov_next(uint8_t cur, uint8_t active_mask) {
    uint16_t sum = 0;
    for (int i = 0; i < 7; i++) {
        if (active_mask & (1u << i)) sum += markov[cur][i];
    }
    if (sum == 0) return cur;
    uint16_t pick = (uint16_t)(prng() % sum);
    for (int i = 0; i < 7; i++) {
        if (active_mask & (1u << i)) {
            if (pick < markov[cur][i]) return (uint8_t)i;
            pick = (uint16_t)(pick - markov[cur][i]);
        }
    }
    return cur;
}

static void mutate(void) {
    uint32_t r = prng();
    int from = (r >> 0) % 7;
    int to   = (r >> 4) % 7;
    markov[from][to] = (uint8_t)((r >> 8) & 0x0Fu);

    int bit = (r >> 12) & 31;
    ca_row ^= (1u << bit);
    if (ca_row == 0) ca_row = 0x12345678u;

    if ((bar_count >> 4) & 1u) {
        eucl_k_a = (uint8_t)(1u + ((r >> 17) % 7u));
    } else {
        eucl_k_b = (uint8_t)(2u + ((r >> 17) % 7u));
    }

    /* Occasionally drift gate_prob so density evolves across the piece.
       Roughly 25% chance per mutate event, +/-16 around current value,
       clamped to musical range [64, 240] (25% .. 94%). */
    if (((r >> 24) & 3u) == 0u) {
        int delta = (int)((r >> 20) & 0x1Fu) - 16;
        int p = (int)gate_prob + delta;
        if (p < 64)  p = 64;
        if (p > 240) p = 240;
        gate_prob = (uint8_t)p;
    }
}

void gen_init(void) {
    cur_degree     = 0;
    cur_scale      = 0;
    ca_row         = 0x12345678u;
    ca_harm        = 0x87654321u;
    eucl_k_a       = 3;
    eucl_k_b       = 5;
    bar_count      = 0;
    step_count     = 0;
    sample_clock   = 0;
    next_step      = 0;
    gate_prob      = 200;
    gen_prng_state = 0xDEADBEEFu;
}

void gen_step(void) {
    if (sample_clock == next_step) {
        uint32_t step_in_bar = step_count % BAR_STEPS;

        if (step_in_bar == 0) {
            ca_row = ca_step(ca_row, RULE_110);
            if (ca_row == 0) ca_row = 0x12345678u;
            bar_count++;
            if (bar_count % MUTATE_BARS == 0) mutate();
            if (bar_count % SCALE_ROTATE_BARS == 0) {
                cur_scale = (uint8_t)((cur_scale + 1u) % N_SCALES);
            }
        }

        if (step_in_bar % 4 == 0) {
            ca_harm = ca_step(ca_harm, RULE_30);
            if (ca_harm == 0) ca_harm = 0x87654321u;
        }

        uint8_t active_mask = (uint8_t)(ca_row & 0x7Fu);
        uint8_t harm_mask = (uint8_t)((ca_harm >> 8) & 0x7Fu);
        active_mask = active_mask & (harm_mask | 0x11u);
        if (active_mask == 0) active_mask = 0x01u;

        uint16_t hits = (uint16_t)(euclid_table[eucl_k_a] | euclid_table[eucl_k_b]);
        unsigned int hit = (unsigned int)((hits >> (15 - step_in_bar)) & 1u);

        /* Bass trigger: once at the start of each bar. */
        if (step_in_bar == 0) {
            uint8_t bass_deg = (bar_count & 1u) ? 4u : 0u;
            uint8_t bass_note = (uint8_t)(SCALES[cur_scale][bass_deg] - 12);
            voice_pool_trigger_role(bass_note, VOICE_FM, ROLE_BASS);
        }

        /* Chord trigger: at steps 0 and 8, fire a 3-note voicing from
           the rotating CHORD_PATTERNS table, filtered by active_mask.
           Pattern index advances per bar so consecutive chord beats hit
           different voicings (triad, 7th, sus4, sus2, inversions...). */
        if (step_in_bar == 0 || step_in_bar == 8) {
            const ChordNote *pat = CHORD_PATTERNS[bar_count % N_CHORD_PATTERNS];
            for (int i = 0; i < 3; i++) {
                uint8_t d = (uint8_t)pat[i].degree;
                if (active_mask & (1u << d)) {
                    uint8_t note = SCALES[cur_scale][d] + (uint8_t)(pat[i].octave * 12);
                    voice_pool_trigger_role(note, VOICE_FM, ROLE_CHORD);
                }
            }
        }

        /* Melody trigger: Euclidean rhythm gated by per-hit probability.
           A hit only fires if (prng % 256) < gate_prob, so some hits
           drop and the melody breathes. */
        if (hit && ((prng() & 0xFFu) < (uint32_t)gate_prob)) {
            cur_degree = markov_next(cur_degree, active_mask);
            uint8_t note = SCALES[cur_scale][cur_degree];
            uint8_t type = (step_in_bar & 1u) ? VOICE_FM : VOICE_KS;
            voice_pool_trigger_role(note, type, ROLE_MELODY);
        }

        step_count++;
        next_step += samples_per_step;
    }
    sample_clock++;
}

void gen_force_mutate(void) {
    mutate();
}

void gen_set_tempo(int delta_pct) {
    int32_t new_val = (int32_t)samples_per_step + ((int32_t)samples_per_step * delta_pct) / 100;
    if (new_val < 2000) new_val = 2000;
    if (new_val > 20000) new_val = 20000;
    samples_per_step = (uint32_t)new_val;
}

uint32_t gen_get_step_samples(void) { return samples_per_step; }
uint32_t gen_get_bar(void) { return bar_count; }
uint8_t  gen_get_step(void) { return (uint8_t)(step_count % BAR_STEPS); }
uint8_t  gen_get_scale(void) { return cur_scale; }
uint8_t  gen_get_gate(void) { return gate_prob; }

void gen_cycle_scale(void) {
    cur_scale = (uint8_t)((cur_scale + 1u) % N_SCALES);
}

void gen_adjust_gate(int delta) {
    int p = (int)gate_prob + delta;
    if (p < 32)  p = 32;
    if (p > 255) p = 255;
    gate_prob = (uint8_t)p;
}
