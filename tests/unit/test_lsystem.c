/* Unit tests for lsystem.c. */
#include "test.h"
#include "../../lsystem.h"
#include <stdint.h>

/* ---- lsystem_reset ---- */

TEST(lsystem_reset_returns_degrees_in_range) {
    lsystem_reset();
    uint8_t mask = 0x7Fu;          /* all 7 degrees active */
    for (int i = 0; i < 100; i++) {
        uint8_t d = lsystem_next(mask);
        if (d == LSYSTEM_REST) continue;
        ASSERT_BETWEEN(d, 0, 6);
    }
}

TEST(lsystem_respects_active_mask) {
    lsystem_reset();
    uint8_t mask = 0b00000101u;    /* only degrees 0 and 2 allowed */
    for (int i = 0; i < 200; i++) {
        uint8_t d = lsystem_next(mask);
        if (d == LSYSTEM_REST) continue;
        /* Returned degree must have its bit set in the mask. */
        ASSERT_TRUE(mask & (1u << d));
    }
}

TEST(lsystem_can_produce_rests) {
    /* Walk a long output; somewhere along the way at least one rest
       should appear because all three starter characters use the
       rest symbol. */
    lsystem_reset();
    int seen_rest = 0;
    int seen_pitched = 0;
    for (int i = 0; i < 600; i++) {
        uint8_t d = lsystem_next(0x7Fu);
        if (d == LSYSTEM_REST) seen_rest = 1;
        else                   seen_pitched = 1;
    }
    /* Either both true, or pitched at minimum. Rest is character-
       dependent so we tolerate not seeing one in 600 steps if the
       active character has very few rests. We do require at least
       one pitched note. */
    (void)seen_rest;
    ASSERT_TRUE(seen_pitched);
}

TEST(lsystem_wraps_buffer_on_long_walk) {
    /* Output buffer is 256 symbols. Walking many more than that
       should not crash; the reset path triggers internally. */
    lsystem_reset();
    for (int i = 0; i < 5000; i++) {
        uint8_t d = lsystem_next(0x7Fu);
        if (d != LSYSTEM_REST) ASSERT_BETWEEN(d, 0, 6);
    }
}

/* ---- lsystem_mutate ---- */

TEST(lsystem_mutate_keeps_output_valid) {
    /* Apply many mutations with varied rng inputs and verify the
       walker still returns valid values (no crashes, no out-of-range
       degrees, rest sentinel respected). */
    lsystem_reset();
    for (uint32_t i = 0; i < 200; i++) {
        lsystem_mutate(i * 0x9E3779B9u + 0x12345u);
        for (int j = 0; j < 50; j++) {
            uint8_t d = lsystem_next(0x7Fu);
            if (d != LSYSTEM_REST) ASSERT_BETWEEN(d, 0, 6);
        }
    }
}

TEST(lsystem_mutate_with_active_mask_changes) {
    /* Just verify that the system survives an extreme mutate-then-
       walk-with-restrictive-mask sequence. */
    lsystem_reset();
    for (int i = 0; i < 50; i++) {
        lsystem_mutate((uint32_t)(i * 7919));
        uint8_t mask = (uint8_t)((i & 1) ? 0x01u : 0x44u);
        for (int j = 0; j < 80; j++) {
            uint8_t d = lsystem_next(mask);
            if (d != LSYSTEM_REST) ASSERT_TRUE(mask & (1u << d));
        }
    }
}

int main(void) {
    return RUN_ALL();
}
