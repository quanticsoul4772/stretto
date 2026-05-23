#include "lsystem.h"
#include <stdint.h>
#include <string.h>

/* Alphabet (6 symbols, fit in 1 byte each for simplicity):
     SYM_UP   - move scale-degree pointer +1
     SYM_UP2  - move +2 (leap)
     SYM_DN   - move -1
     SYM_DN2  - move -2 (leap)
     SYM_REP  - repeat current degree (no move)
     SYM_REST - emit rest (no trigger this hit)
*/
enum {
    SYM_UP   = 1,
    SYM_UP2  = 2,
    SYM_DN   = 3,
    SYM_DN2  = 4,
    SYM_REP  = 5,
    SYM_REST = 6,
    N_SYMBOLS = 6   /* SYM_UP..SYM_REST */
};

/* Rule set ("character"). One RHS per symbol; each RHS is up to
   RULE_RHS_MAX symbols, NUL-terminated (terminator byte = 0). */
#define N_CHARACTERS    3
#define N_RULES         6
#define RULE_RHS_MAX    6
#define AXIOM_MAX       8
#define OUTPUT_BUF_LEN  256
#define EXPAND_GENS     3

typedef struct {
    uint8_t rhs[N_RULES][RULE_RHS_MAX + 1];   /* +1 for NUL terminator */
} Character;

/* Hand-tuned starter characters.
   Indexed as rhs[symbol - 1] since symbols are 1-based. */
static Character characters[N_CHARACTERS] = {
    /* Character 0: stepwise. Lots of step movement, occasional
       rest, leaps resolve back stepwise. */
    {{
        /* UP   */ { SYM_UP,   SYM_UP,   SYM_DN,   0 },
        /* UP2  */ { SYM_UP,   SYM_DN,   0 },
        /* DN   */ { SYM_DN,   SYM_DN,   SYM_UP,   0 },
        /* DN2  */ { SYM_DN,   SYM_UP,   0 },
        /* REP  */ { SYM_UP,   SYM_REP,  SYM_DN,   0 },
        /* REST */ { SYM_REP,  0 },
    }},
    /* Character 1: leaping. Leaps beget more leaps, with stepwise
       resolutions. */
    {{
        /* UP   */ { SYM_UP2,  SYM_DN,   0 },
        /* UP2  */ { SYM_UP2,  SYM_UP,   SYM_DN,   0 },
        /* DN   */ { SYM_DN2,  SYM_UP,   0 },
        /* DN2  */ { SYM_DN2,  SYM_DN,   SYM_UP,   0 },
        /* REP  */ { SYM_UP2,  SYM_DN2,  0 },
        /* REST */ { SYM_REP,  SYM_REST, 0 },
    }},
    /* Character 2: sparse. Lots of rests interleaved with motion. */
    {{
        /* UP   */ { SYM_UP,   SYM_REST, SYM_DN,   0 },
        /* UP2  */ { SYM_REST, SYM_UP,   SYM_REST, 0 },
        /* DN   */ { SYM_DN,   SYM_REST, SYM_UP,   0 },
        /* DN2  */ { SYM_REST, SYM_DN,   SYM_REST, 0 },
        /* REP  */ { SYM_REST, SYM_UP,   SYM_REST, 0 },
        /* REST */ { SYM_REST, SYM_REST, 0 },
    }},
};

static uint8_t axiom[AXIOM_MAX + 1] = { SYM_UP, SYM_REST, SYM_DN, SYM_UP, 0 };
static uint8_t output_buf[OUTPUT_BUF_LEN];
static uint16_t output_len = 0;
static uint16_t pos = 0;
static int8_t  pointer = 0;     /* current scale-degree 0..6 */
static uint8_t cur_character = 0;

static uint16_t append_rhs(uint8_t *dst, uint16_t dst_pos,
                           const uint8_t *rhs, uint16_t dst_max) {
    for (uint16_t i = 0; rhs[i] != 0; i++) {
        if (dst_pos >= dst_max) return dst_pos;
        dst[dst_pos++] = rhs[i];
    }
    return dst_pos;
}

/* Expand the axiom EXPAND_GENS generations using the current
   character. Cap at OUTPUT_BUF_LEN. */
void lsystem_reset(void) {
    /* Generation 0: copy axiom into output_buf. */
    uint16_t len = 0;
    for (uint16_t i = 0; axiom[i] != 0 && len < OUTPUT_BUF_LEN; i++) {
        output_buf[len++] = axiom[i];
    }

    /* Successive generations: rewrite in-place using a scratch buffer. */
    static uint8_t scratch[OUTPUT_BUF_LEN];
    Character *c = &characters[cur_character % N_CHARACTERS];

    for (int g = 0; g < EXPAND_GENS; g++) {
        uint16_t scratch_pos = 0;
        for (uint16_t i = 0; i < len; i++) {
            uint8_t s = output_buf[i];
            if (s < SYM_UP || s > SYM_REST) {
                /* unknown symbol; copy verbatim */
                if (scratch_pos < OUTPUT_BUF_LEN) {
                    scratch[scratch_pos++] = s;
                }
                continue;
            }
            scratch_pos = append_rhs(scratch, scratch_pos,
                                     c->rhs[s - 1], OUTPUT_BUF_LEN);
            if (scratch_pos >= OUTPUT_BUF_LEN) break;
        }
        len = scratch_pos;
        memcpy(output_buf, scratch, len);
        if (len >= OUTPUT_BUF_LEN - RULE_RHS_MAX) break;
    }

    output_len = len;
    pos = 0;
    pointer = 0;
}

static uint8_t snap_to_mask(int8_t deg, uint8_t active_mask) {
    /* Wrap to [0, 6]. */
    while (deg < 0) deg += 7;
    while (deg >  6) deg -= 7;
    if (active_mask & (1u << deg)) return (uint8_t)deg;

    /* Try alternating outward search for the nearest in-mask degree. */
    for (int8_t off = 1; off <= 6; off++) {
        int8_t up = deg + off;
        int8_t dn = deg - off;
        while (up > 6) up -= 7;
        while (dn < 0) dn += 7;
        if (active_mask & (1u << up)) return (uint8_t)up;
        if (active_mask & (1u << dn)) return (uint8_t)dn;
    }
    /* mask was zero (shouldn't happen; gen.c forces 0x01 fallback). */
    return 0;
}

uint8_t lsystem_next(uint8_t active_mask) {
    if (output_len == 0) lsystem_reset();
    if (pos >= output_len) lsystem_reset();

    uint8_t s = output_buf[pos++];

    if (s == SYM_REST) return LSYSTEM_REST;

    switch (s) {
        case SYM_UP:  pointer += 1; break;
        case SYM_UP2: pointer += 2; break;
        case SYM_DN:  pointer -= 1; break;
        case SYM_DN2: pointer -= 2; break;
        case SYM_REP: /* no move */ break;
        default:      /* unknown */ break;
    }

    uint8_t snapped = snap_to_mask(pointer, active_mask);
    pointer = (int8_t)snapped;   /* anchor pointer to whatever degree we settled on */
    return snapped;
}

/* Mutation: ~50% chance to re-roll one rule's RHS, ~25% to cycle
   character, ~25% to swap an axiom symbol. The caller is expected
   to call lsystem_reset() after - but we do it here so the next
   phrase reflects the new rules without extra work in the caller. */
void lsystem_mutate(uint32_t rng) {
    uint32_t pick = (rng >> 24) & 3u;   /* 0,1 => rule reroll; 2 => cycle; 3 => axiom */

    if (pick <= 1u) {
        uint8_t rule_idx = (uint8_t)((rng >> 4) % N_RULES);
        Character *c = &characters[cur_character % N_CHARACTERS];
        uint8_t *rhs = c->rhs[rule_idx];
        /* New RHS length 2..5 from a different region of the rng. */
        uint8_t new_len = (uint8_t)(2u + ((rng >> 12) % 4u));
        for (uint8_t i = 0; i < new_len; i++) {
            /* Symbols are 1..6 (SYM_UP..SYM_REST). */
            rhs[i] = (uint8_t)(SYM_UP + ((rng >> (16 + 3 * i)) % N_SYMBOLS));
        }
        rhs[new_len] = 0;
    } else if (pick == 2u) {
        cur_character = (uint8_t)((cur_character + 1) % N_CHARACTERS);
    } else {
        uint8_t i = (uint8_t)((rng >> 20) & 3u);     /* axiom slot 0..3 */
        if (i < AXIOM_MAX) {
            axiom[i] = (uint8_t)(SYM_UP + ((rng >> 28) % N_SYMBOLS));
        }
    }

    lsystem_reset();
}

void lsystem_set_character(uint8_t idx) {
    uint8_t target = (uint8_t)(idx % N_CHARACTERS);
    if (target == cur_character) return;
    cur_character = target;
    lsystem_reset();
}
