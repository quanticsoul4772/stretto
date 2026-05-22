#include "chord_progression.h"
#include <stdint.h>

/* Major-flavored Markov weights. Used for Lydian (scale=1) and
   Mixolydian (scale=5). Cadences (V->I, IV->I, vii->I, ii->V)
   carry the highest weights so resolutions feel earned. The
   diagonal stays nonzero so the synth can sit on a chord for
   multiple advances. */
static const uint8_t CHORD_MARKOV_MAJOR[7][7] = {
    /* from\to  I   ii  iii IV  V   vi  vii */
    /* I   */ {  2,  1,  1,  4,  4,  3,  1 },
    /* ii  */ {  3,  0,  0,  1,  5,  0,  0 },
    /* iii */ {  2,  0,  0,  2,  1,  4,  0 },
    /* IV  */ {  4,  1,  0,  1,  4,  2,  0 },
    /* V   */ {  5,  0,  0,  1,  0,  3,  0 },
    /* vi  */ {  2,  4,  0,  4,  2,  0,  0 },
    /* vii */ {  4,  0,  0,  0,  2,  0,  0 },
};

/* Minor-flavored weights. Used for Dorian (0), Phrygian (2),
   Locrian (3), Harmonic Minor (4). Modal/cadential motion:
   VII<->i, iv<->i, weaker dominant pull than major-mode. */
static const uint8_t CHORD_MARKOV_MINOR[7][7] = {
    /* from\to  i   iiº III iv  v   VI  VII */
    /* i   */ {  3,  1,  2,  3,  3,  2,  3 },
    /* iiº */ {  3,  0,  0,  0,  2,  0,  0 },
    /* III */ {  3,  0,  0,  2,  1,  2,  1 },
    /* iv  */ {  3,  1,  0,  1,  2,  0,  2 },
    /* v   */ {  4,  0,  0,  1,  0,  1,  1 },
    /* VI  */ {  2,  0,  1,  3,  1,  0,  2 },
    /* VII */ {  4,  0,  1,  0,  1,  0,  0 },
};

static uint8_t current_root = 0;

static const uint8_t (*select_table(uint8_t scale))[7] {
    /* Lydian (1) and Mixolydian (5) get the major table. */
    if (scale == 1u || scale == 5u) return CHORD_MARKOV_MAJOR;
    return CHORD_MARKOV_MINOR;
}

void chord_progression_init(void) {
    current_root = 0;
}

void chord_progression_step(uint32_t rng, uint8_t scale) {
    const uint8_t (*table)[7] = select_table(scale);
    const uint8_t *row = table[current_root % CHORD_N_DEGREES];

    uint32_t sum = 0;
    for (int i = 0; i < CHORD_N_DEGREES; i++) sum += row[i];
    if (sum == 0u) {                   /* defensive: degenerate row */
        current_root = 0;
        return;
    }

    uint32_t pick = rng % sum;
    uint32_t acc = 0;
    for (int i = 0; i < CHORD_N_DEGREES; i++) {
        acc += row[i];
        if (pick < acc) {
            current_root = (uint8_t)i;
            return;
        }
    }
    /* unreachable; falls through if rounding pushed past last accumulation */
}

uint8_t chord_progression_get_root(void) {
    return current_root;
}
