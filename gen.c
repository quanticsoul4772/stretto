#include "gen.h"
#include "voice.h"
#include "euclid_table.h"
#include <stdint.h>

#define BAR_STEPS         16
#define SAMPLES_PER_STEP  5512u    /* ~125 ms = 120 BPM 16th notes */
#define MUTATE_BARS       4u

static uint32_t gen_prng_state = 0xDEADBEEFu;
static uint32_t prng(void) {
    uint32_t x = gen_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gen_prng_state = x;
    return x;
}

/* D Dorian, one octave */
static const uint8_t SCALE[7] = { 62, 64, 65, 67, 69, 71, 72 };

/* Hand-tuned transition weights; mutated at runtime. */
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
static uint8_t  eucl_k_a   = 3;
static uint8_t  eucl_k_b   = 5;
static uint32_t bar_count  = 0;
static uint32_t step_count = 0;
static uint32_t sample_clock = 0;
static uint32_t next_step  = 0;

/* Rule 110 lookup: bit p of 0x6E (= 01101110b) gives output for input pattern p */
#define RULE_110 0x6Eu

static uint32_t rule110_step(uint32_t row) {
    uint32_t next = 0;
    for (int i = 0; i < 32; i++) {
        unsigned int left   = (row >> ((i + 1) & 31)) & 1u;
        unsigned int center = (row >> i) & 1u;
        unsigned int right  = (row >> ((i + 31) & 31)) & 1u;
        unsigned int p = (left << 2) | (center << 1) | right;
        if ((RULE_110 >> p) & 1u) next |= (1u << i);
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
}

void gen_init(void) {
    cur_degree     = 0;
    ca_row         = 0x12345678u;
    eucl_k_a       = 3;
    eucl_k_b       = 5;
    bar_count      = 0;
    step_count     = 0;
    sample_clock   = 0;
    next_step      = 0;
    gen_prng_state = 0xDEADBEEFu;
}

void gen_step(void) {
    if (sample_clock == next_step) {
        uint32_t step_in_bar = step_count % BAR_STEPS;

        if (step_in_bar == 0) {
            ca_row = rule110_step(ca_row);
            if (ca_row == 0) ca_row = 0x12345678u;
            bar_count++;
            if (bar_count % MUTATE_BARS == 0) mutate();
        }

        uint8_t active_mask = (uint8_t)(ca_row & 0x7Fu);
        if (active_mask == 0) active_mask = 0x01u;

        uint16_t hits = (uint16_t)(euclid_table[eucl_k_a] | euclid_table[eucl_k_b]);
        unsigned int hit = (unsigned int)((hits >> (15 - step_in_bar)) & 1u);

        if (hit) {
            cur_degree = markov_next(cur_degree, active_mask);
            uint8_t note = SCALE[cur_degree];
            uint8_t type = (step_in_bar & 1u) ? VOICE_FM : VOICE_KS;
            voice_pool_trigger(note, type);
        }

        step_count++;
        next_step += SAMPLES_PER_STEP;
    }
    sample_clock++;
}
