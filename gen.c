#include "gen.h"
#include "voice.h"
#include "euclid_table.h"
#include "lsystem.h"
#include "chord_progression.h"
#include "section.h"
#include "density.h"
#include "effects.h"
#include <stdint.h>
#include <time.h>

/* Substep grid for true 3-against-4 polyrhythm. 48 = LCM(3, 4, 16):
     bass fires at 3 evenly-spaced positions (every 16 substeps)
     chord fires at 4 evenly-spaced positions (every 12 substeps)
     melody continues on a 16-step Euclidean grid (every 3 substeps)
   All three lock at substep 0 of each bar, then diverge across the
   bar. The 3:4 cross-rhythm is heard as the bass and chord events
   interlock - a classical polyrhythm pattern. */
#define BAR_SUBSTEPS      48u
#define MELODY_SUBSTRIDE   3u   /* one melody slot every 3 substeps */

/* Mutation interval is dynamic. A slow LFO drives the interval
   between MUTATE_MIN bars (busy section) and MUTATE_MAX bars (calm
   section). LFO period of 128 bars (~4.3 min at 2 s bars) gives
   long-form ambient swells of activity. Starting interval (used
   until the first mutation fires) is MUTATE_DEFAULT. */
#define MUTATE_MIN         1u
#define MUTATE_MAX         16u
#define MUTATE_DEFAULT     4u
#define MUTATE_LFO_INC     512u    /* 65536 / 512 = 128-bar period */

/* 48 substeps * 2000 samples = 96000 samples = 2.00 s per bar at
   48 kHz (rescaled from the prior 1837 at 44.1 kHz). Tempo control
   scales this. */
static uint32_t samples_per_substep = 2000u;

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

/* 1st-order Markov transition seed for D Dorian. Rows are "from"
   degree, columns "to" degree, weights are unnormalised
   probabilities. The runtime table is the 2nd-order markov2[7][7][7]
   below; this 2D seed is replicated across the "previous-previous"
   axis at gen_init time so the new code starts with the same
   musical character as the original 1st-order chain. Mutation
   diverges from there.
   Tuning principles:
     - Stepwise motion (cols at +/-1) gets moderate weight (3-4).
     - Strong cadences favoured: leading tone (deg 6) and dominant
       (deg 4) bias toward tonic (deg 0) - high column-0 values in
       rows 4 and 6 (5 each).
     - No self-transitions (diagonal is 0) - prevents stuck notes
       on a single degree.
     - Tonic (row 0) opens out broadly to all degrees except itself,
       slight bias to dominant (deg 4) for V-I framing. */
static const uint8_t MARKOV_SEED[7][7] = {
    { 0, 4, 3, 2, 4, 1, 2 },
    { 5, 0, 3, 2, 1, 1, 0 },
    { 3, 4, 0, 4, 2, 1, 0 },
    { 2, 2, 3, 0, 4, 2, 1 },
    { 5, 1, 2, 3, 0, 3, 2 },
    { 1, 2, 1, 2, 3, 0, 3 },
    { 5, 1, 0, 1, 2, 2, 0 },
};

/* Runtime 2nd-order Markov table, indexed [prev_prev][prev][next].
   343 cells = 343 bytes static, vs the old 1st-order 49 cells. The
   extra context lets stepwise motifs and cadential figures persist
   longer (the chain remembers "we just leaped up, so we should step
   down" rather than treating each note in isolation). Seeded by
   replicating MARKOV_SEED across the prev_prev axis; mutation
   drifts individual cells so distinct prev_prev contexts evolve
   different transition tendencies. */
static uint8_t markov2[7][7][7];

static uint16_t mutate_lfo_phase = 0;
static uint8_t  bars_until_mutate = MUTATE_DEFAULT;
static uint8_t  cur_degree = 0;
/* Counter-melody runs an independent Markov walk and Euclidean pattern
   alongside the main melody. Pitches are shifted up one octave so the
   two lines stay distinguishable in register. */
static uint8_t  cur_degree_counter      = 4;  /* start on dominant for harmonic interest */
static uint8_t  cur_degree_counter_prev = 0;  /* 2nd-order context: degree before cur */
static uint8_t  eucl_k_counter          = 4;
/* Voice-leading anchor: rough pitch center of the previous chord trigger.
   New chord pitches octave-shift toward this so chord-to-chord motion is
   stepwise rather than leaping. Anchored to a mid-range value to prevent
   long-term drift. */
static uint8_t  prev_chord_center  = 67;
static uint32_t ca_row     = 0x12345678u;
static uint32_t ca_harm    = 0x87654321u;
static uint8_t  eucl_k_a   = 3;
static uint8_t  eucl_k_b   = 5;
static uint32_t bar_count  = 0;
static uint32_t substep_count = 0;
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

/* 2nd-order Markov walk: pick next degree weighted by the
   (prev_prev, prev) context row, restricted to the active mask.
   Falls back to prev if the active row sums to zero (defensive;
   active mask is guaranteed non-empty upstream). */
static uint8_t markov2_next(uint8_t prev_prev, uint8_t prev, uint8_t active_mask) {
    const uint8_t *row = markov2[prev_prev % 7u][prev % 7u];
    uint16_t sum = 0;
    for (int i = 0; i < 7; i++) {
        if (active_mask & (1u << i)) sum += row[i];
    }
    if (sum == 0) return prev;
    uint16_t pick = (uint16_t)(prng() % sum);
    for (int i = 0; i < 7; i++) {
        if (active_mask & (1u << i)) {
            if (pick < row[i]) return (uint8_t)i;
            pick = (uint16_t)(pick - row[i]);
        }
    }
    return prev;
}

/* Replicate the 1st-order MARKOV_SEED across the prev_prev axis.
   Called once from gen_init so the chain starts with the original
   musical character; mutation diverges per (prev_prev, prev) row. */
static void markov2_init(void) {
    for (int pp = 0; pp < 7; pp++)
        for (int p = 0; p < 7; p++)
            for (int n = 0; n < 7; n++)
                markov2[pp][p][n] = MARKOV_SEED[p][n];
}

static void mutate(void) {
    uint32_t r = prng();
    /* Drift one cell of the 2nd-order Markov table. Distinct
       (prev_prev, prev) contexts evolve different transition
       tendencies over the piece. */
    int pp = (r >> 24) % 7;
    int pv = (r >> 0)  % 7;
    int to = (r >> 4)  % 7;
    markov2[pp][pv][to] = (uint8_t)((r >> 8) & 0x0Fu);

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

    /* Re-roll the counter-melody Euclidean k periodically so the
       counter-melody pattern shifts independently of the main melody. */
    if (((r >> 28) & 1u) == 0u) {
        eucl_k_counter = (uint8_t)(2u + ((r >> 18) % 7u));
    }

    /* Drift the filter base cutoff and resonance occasionally so the
       overall timbre evolves over long timescales. */
    if (((r >> 30) & 1u) == 0u) {
        voice_mutate_filter(prng());
    }

    /* Drift the L-system melodic grammar on roughly 1 in 3 mutate
       events so the main melody's character shifts across the piece. */
    if (((r >> 26) & 3u) == 0u) {
        lsystem_mutate(prng());
    }
}

/* Read the current dynamic mutation interval from the triangle LFO.
   Range: MUTATE_MIN (~1 bar, busy section) to MUTATE_MAX (~16 bars,
   calm section). LFO period is 128 bars (~4.3 min at default tempo). */
static uint8_t dynamic_mutate_interval(void) {
    uint16_t tri = (mutate_lfo_phase < 32768u)
                   ? mutate_lfo_phase
                   : (uint16_t)(65535u - mutate_lfo_phase);
    uint32_t v = (uint32_t)MUTATE_MIN
               + ((uint32_t)tri * (MUTATE_MAX - MUTATE_MIN + 1u)) / 32768u;
    if (v < MUTATE_MIN) v = MUTATE_MIN;
    if (v > MUTATE_MAX) v = MUTATE_MAX;
    return (uint8_t)v;
}

/* Effective mutation interval = dynamic LFO value + section bias,
   clamped to [1, 32]. Single source of truth so new bias sources
   (density, future modulators) extend this function instead of the
   gen_step call-site. */
static uint8_t effective_mutate_interval(void) {
    int v = (int)dynamic_mutate_interval()
          + (int)section_bias_mutation_interval();
    if (v < 1)  v = 1;
    if (v > 32) v = 32;
    return (uint8_t)v;
}

/* Flag set by gen_seed; gen_init checks it and seeds from clock if
   never explicitly seeded. */
static int gen_seeded_explicitly = 0;

/* xorshift32: turn one input seed into a sequence of well-distributed
   uint32 values so PRNG, ca_row, and ca_harm all start from
   independent points. The zero fixed point of xorshift32 is handled
   by gen_seed() XORing the input with a constant first so seed=0
   maps somewhere meaningful and does not collide with seed=1. */
static uint32_t hash32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void gen_seed(uint32_t seed) {
    /* XOR with a non-zero constant so seed=0 and seed=1 hash to
       different chains and seed=0 does not hit xorshift's zero
       fixed point. */
    uint32_t s = hash32(seed ^ 0xDEADBEEFu);
    if (s == 0) s = 0x12345678u;
    gen_prng_state = s;
    s = hash32(s);
    ca_row = s ? s : 0x12345678u;
    s = hash32(s);
    ca_harm = s ? s : 0x87654321u;
    gen_seeded_explicitly = 1;
}

void gen_init(void) {
    cur_degree         = 0;
    cur_degree_counter      = 4;
    cur_degree_counter_prev = 0;
    markov2_init();
    eucl_k_counter     = 4;
    prev_chord_center  = 67;
    cur_scale          = 0;
    eucl_k_a           = 3;
    eucl_k_b           = 5;
    bar_count          = 0;
    substep_count      = 0;
    sample_clock       = 0;
    next_step          = 0;
    gate_prob          = 200;
    mutate_lfo_phase   = 0;
    bars_until_mutate  = MUTATE_DEFAULT;

    /* If the caller did not seed explicitly, derive seeds from the
       wall clock so each launch is a different generative output. */
    if (!gen_seeded_explicitly) {
        gen_seed((uint32_t)time(NULL));
    }

    /* Build the initial L-system expansion so the first melody trigger
       has output to walk. */
    lsystem_reset();

    /* Reset chord-progression root to tonic. */
    chord_progression_init();

    /* Reset song-section state to the start of INTRO. */
    section_init();
    /* Apply initial biases so first bar of audio reflects the section. */
    voice_set_cutoff_bias(section_bias_cutoff());
    lsystem_set_character(section_lsystem_character());

    /* Adaptive density: prime tension with a conservative initial
       state. The first bar-boundary in gen_step recomputes from
       the live active mask + gate. */
    density_update(0x01u, gate_prob);

    /* Reverb bias is the sum of section + density. */
    reverb_set_wet_bias((int8_t)(section_bias_reverb()
                               + density_bias_reverb()));
}

/* Bar-boundary work: advance CA, fire scheduled mutations, advance
   the chord-progression Markov (every 2 bars), step the section LFO
   and push its biases to voice / lsystem / reverb, recompute density
   bias from current ca_row + gate. Called once per bar from gen_step
   at substep 0. */
static inline void schedule_bar_boundary(void) {
    ca_row = ca_step(ca_row, RULE_110);
    if (ca_row == 0) ca_row = 0x12345678u;
    bar_count++;
    mutate_lfo_phase += MUTATE_LFO_INC;
    if (--bars_until_mutate == 0u) {
        mutate();
        bars_until_mutate = effective_mutate_interval();
    }
    if ((bar_count % 2u) == 0u) {
        chord_progression_step(prng(), cur_scale);
    }
    section_step(bar_count);
    voice_set_cutoff_bias(section_bias_cutoff());
    lsystem_set_character(section_lsystem_character());
    /* ca_harm has not yet updated for this bar (the % 12 advance
       runs after this block in gen_step), so density uses ca_row
       alone as the bar-stable active-scale-degree measure. */
    density_update((uint8_t)(ca_row & 0x7Fu), gate_prob);
    reverb_set_wet_bias((int8_t)(section_bias_reverb()
                               + density_bias_reverb()));
}

/* Combine bar-scale ca_row (Rule 110) with the faster ca_harm
   (Rule 30, advances every 12 substeps), forcing the result to a
   non-empty 7-bit mask. The (harm | 0x11) keeps degrees 0 and 4
   always available so the active set never starves the chord
   progression. */
static inline uint8_t compute_active_mask(void) {
    uint8_t am = (uint8_t)(ca_row & 0x7Fu);
    uint8_t hm = (uint8_t)((ca_harm >> 8) & 0x7Fu);
    am &= (hm | 0x11u);
    return am ? am : 0x01u;
}

/* Drum trigger: each drum has its own rotating bank of bitmask
   patterns. Bit N of a pattern set means "trigger this drum at
   substep N within the bar". Banks have coprime sizes (4, 3, 5)
   so the combined kit cycles through LCM(4,3,5) = 60 bars before
   exactly repeating. Kick bank is pinned by the current section
   (sparse / 4-on-floor); snare and hihat advance per bar. */
static inline void schedule_drums(uint32_t substep_in_bar) {
    #define S(n) (1ULL << (n))
    static const uint64_t kick_patterns[4] = {
        /* basic 1+3 */          S(0) | S(24),
        /* syncopated 1+3+ */    S(0) | S(24) | S(30),
        /* four-on-the-floor */  S(0) | S(12) | S(24) | S(36),
        /* off-kilter w/ 2+ */   S(0) | S(18) | S(24),
    };
    static const uint64_t snare_patterns[3] = {
        /* classic 2+4 */        S(12) | S(36),
        /* with ghost 2e 4e */   S(9)  | S(12) | S(33) | S(36),
        /* half-time 3 only */   S(24),
    };
    static const uint64_t hihat_patterns[5] = {
        /* 8ths every 6 */       S(0)|S(6)|S(12)|S(18)|S(24)|S(30)|S(36)|S(42),
        /* 16ths every 3 */      S(0)|S(3)|S(6)|S(9)|S(12)|S(15)|S(18)|S(21)|
                                 S(24)|S(27)|S(30)|S(33)|S(36)|S(39)|S(42)|S(45),
        /* quarters only */      S(0)|S(12)|S(24)|S(36),
        /* offbeats only */      S(6)|S(18)|S(30)|S(42),
        /* triplet feel */       S(0)|S(8)|S(16)|S(24)|S(32)|S(40),
    };
    #undef S
    uint64_t kbits = kick_patterns [section_kick_pattern() % 4u];
    uint64_t sbits = snare_patterns[bar_count % 3u];
    uint64_t hbits = hihat_patterns[bar_count % 5u];
    if ((kbits >> substep_in_bar) & 1u) voice_pool_trigger_drum(DRUM_KICK);
    if ((sbits >> substep_in_bar) & 1u) voice_pool_trigger_drum(DRUM_SNARE);
    if ((hbits >> substep_in_bar) & 1u) voice_pool_trigger_drum(DRUM_HIHAT);
}

/* Bass trigger: 4 events per bar at unequal spacing for a bouncing
   feel. Beats 1 and 3 (substeps 0, 24) anchor the tempo; offbeats at
   "and of 2" and "and of 4" (substeps 18, 42) anticipate beats 3 and
   the next bar 1 - a classic dub / reggae bass groove. Pitch
   alternates root/fifth per event so the line moves rather than
   thudding the same note; bar parity swaps the order so consecutive
   bars do not duplicate. Root and fifth are computed relative to the
   current chord function from chord_progression. */
static inline void schedule_bass(uint32_t substep_in_bar) {
    static const uint8_t bass_substeps[4] = { 0u, 18u, 24u, 42u };
    static const uint8_t bass_deg_a[4]    = { 0u, 4u, 0u, 4u };
    static const uint8_t bass_deg_b[4]    = { 4u, 0u, 4u, 0u };
    for (int i = 0; i < 4; i++) {
        if (substep_in_bar != bass_substeps[i]) continue;
        const uint8_t *degs = (bar_count & 1u) ? bass_deg_b : bass_deg_a;
        uint8_t chord_root = chord_progression_get_root();
        uint8_t deg = (uint8_t)((chord_root + degs[i]) % 7u);
        uint8_t bass_note = (uint8_t)(SCALES[cur_scale][deg] - 12);
        voice_pool_trigger_role(bass_note, VOICE_FM, ROLE_BASS);
        return;
    }
}

/* Chord trigger: 4 evenly-spaced events per bar (substeps 0, 12, 24,
   36) - the "4" side of the 3-against-4 polyrhythm. Voicing pattern
   rotates per bar; root is rebased onto current chord function; each
   pitch is octave-shifted toward the previous chord's centroid for
   voice leading. */
static inline void schedule_chord(uint32_t substep_in_bar, uint8_t active_mask) {
    if (substep_in_bar != 0u && substep_in_bar != 12u &&
        substep_in_bar != 24u && substep_in_bar != 36u) return;
    const ChordNote *pat = CHORD_PATTERNS[bar_count % N_CHORD_PATTERNS];
    uint8_t chord_root = chord_progression_get_root();
    uint16_t sum = 0;
    uint8_t  count = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t d = (uint8_t)((pat[i].degree + chord_root) % 7u);
        if (!(active_mask & (1u << d))) continue;
        int note = (int)SCALES[cur_scale][d] + (int)pat[i].octave * 12;
        while (note > (int)prev_chord_center + 8) note -= 12;
        while (note < (int)prev_chord_center - 8) note += 12;
        if (note < 24)  note += 12;
        if (note > 96)  note -= 12;
        voice_pool_trigger_role((uint8_t)note, VOICE_FM, ROLE_CHORD);
        sum += (uint16_t)note;
        count++;
    }
    if (count > 0) {
        uint8_t new_center = (uint8_t)(sum / count);
        prev_chord_center = (uint8_t)(((uint16_t)prev_chord_center * 3u
                                     + (uint16_t)new_center) >> 2);
        if (prev_chord_center < 55) prev_chord_center = 55;
        if (prev_chord_center > 79) prev_chord_center = 79;
    }
}

/* Melody trigger: main + counter, only on Euclidean grid substeps
   (every MELODY_SUBSTRIDE = 3). Main melody is L-system phrased,
   counter is Markov walked, both pitched into the active mask, both
   probability-gated by the effective gate (user + section + density). */
static inline void schedule_melody(uint32_t substep_in_bar, uint8_t active_mask) {
    if (substep_in_bar % MELODY_SUBSTRIDE != 0u) return;
    uint32_t step_in_bar = substep_in_bar / MELODY_SUBSTRIDE;

    int eff_gate_i = (int)gate_prob
                   + (int)section_bias_gate()
                   + (int)density_bias_gate();
    if (eff_gate_i < 32)  eff_gate_i = 32;
    if (eff_gate_i > 255) eff_gate_i = 255;
    uint32_t eff_gate = (uint32_t)eff_gate_i;

    uint16_t hits = (uint16_t)(euclid_table[eucl_k_a] | euclid_table[eucl_k_b]);
    if (((hits >> (15 - step_in_bar)) & 1u) && ((prng() & 0xFFu) < eff_gate)) {
        uint8_t deg = lsystem_next(active_mask);
        if (deg != LSYSTEM_REST) {
            cur_degree = deg;
            uint8_t note = SCALES[cur_scale][cur_degree];
            uint8_t type = (step_in_bar & 1u) ? VOICE_FM : VOICE_KS;
            voice_pool_trigger_role(note, type, ROLE_MELODY);
        }
    }

    uint16_t cnt_hits = (uint16_t)euclid_table[eucl_k_counter];
    if (((cnt_hits >> (15 - step_in_bar)) & 1u) && ((prng() & 0xFFu) < eff_gate)) {
        /* 2nd-order Markov walk: each step considers the previous
           two degrees, so leap+resolve and step+turn motifs persist
           longer than a memoryless first-order walk would allow. */
        uint8_t next = markov2_next(cur_degree_counter_prev,
                                    cur_degree_counter, active_mask);
        cur_degree_counter_prev = cur_degree_counter;
        cur_degree_counter      = next;
        uint8_t note = (uint8_t)(SCALES[cur_scale][cur_degree_counter] + 12);
        voice_pool_trigger_role(note, VOICE_FM, ROLE_MELODY);
    }
}

void gen_step(void) {
    if (sample_clock == next_step) {
        uint32_t substep_in_bar = substep_count % BAR_SUBSTEPS;

        if (substep_in_bar == 0) schedule_bar_boundary();

        /* ca_harm advances 4 times per bar - every 12 substeps. */
        if (substep_in_bar % 12u == 0u) {
            ca_harm = ca_step(ca_harm, RULE_30);
            if (ca_harm == 0) ca_harm = 0x87654321u;
        }

        uint8_t active_mask = compute_active_mask();

        schedule_drums(substep_in_bar);
        schedule_bass(substep_in_bar);
        schedule_chord(substep_in_bar, active_mask);
        schedule_melody(substep_in_bar, active_mask);

        substep_count++;
        next_step += samples_per_substep;
    }
    sample_clock++;
}

void gen_force_mutate(void) {
    mutate();
}

void gen_set_tempo(int delta_pct) {
    int32_t new_val = (int32_t)samples_per_substep
                    + ((int32_t)samples_per_substep * delta_pct) / 100;
    /* Range: ~760 (faster) .. ~7600 (slower) at 48 kHz, keeping the
       bar between ~0.75 s and ~7.5 s. */
    if (new_val < 760)  new_val = 760;
    if (new_val > 7600) new_val = 7600;
    samples_per_substep = (uint32_t)new_val;
}

uint32_t gen_get_step_samples(void) { return samples_per_substep; }
uint32_t gen_get_bar(void) { return bar_count; }
/* Report the melody-step position (0..15) for backward-compatible
   status display: 48 substeps maps to 16 melody steps. */
uint8_t  gen_get_step(void) { return (uint8_t)((substep_count % BAR_SUBSTEPS) / MELODY_SUBSTRIDE); }
uint8_t  gen_get_scale(void) { return cur_scale; }
uint8_t  gen_get_gate(void) { return gate_prob; }
uint8_t  gen_get_degree(void) { return cur_degree; }

/* Recompute the same active_mask gen_step uses each step, for UI
   readout. ca_row contributes the low 7 bits; ca_harm bits 8-14
   narrow it; fallback to tonic if everything zeroes. */
uint8_t gen_get_active_mask(void) {
    uint8_t m = (uint8_t)(ca_row & 0x7Fu);
    uint8_t h = (uint8_t)((ca_harm >> 8) & 0x7Fu);
    m = m & (h | 0x11u);
    if (m == 0) m = 0x01u;
    return m;
}

uint8_t gen_get_chord_pattern(void) {
    return (uint8_t)(bar_count % N_CHORD_PATTERNS);
}

uint8_t gen_get_chord_root(void) {
    return chord_progression_get_root();
}

const char *gen_get_section_name(void) {
    return section_name();
}

uint8_t gen_get_tension(void) {
    return density_get_tension();
}

void gen_cycle_scale(void) {
    cur_scale = (uint8_t)((cur_scale + 1u) % N_SCALES);
}

void gen_adjust_gate(int delta) {
    int p = (int)gate_prob + delta;
    if (p < 32)  p = 32;
    if (p > 255) p = 255;
    gate_prob = (uint8_t)p;
}
