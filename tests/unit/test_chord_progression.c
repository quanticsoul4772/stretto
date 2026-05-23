/* Unit tests for chord_progression.c. */
#include "test.h"
#include "../../chord_progression.h"
#include <stdint.h>

TEST(chord_progression_init_resets_to_zero) {
    chord_progression_init();
    ASSERT_EQ(chord_progression_get_root(), 0);
}

TEST(chord_progression_step_stays_in_range_all_scales) {
    /* 100 advancements per scale, all six scales, all roots must
       be in [0, 6]. */
    for (uint8_t scale = 0; scale < 6; scale++) {
        chord_progression_init();
        for (int i = 0; i < 100; i++) {
            chord_progression_step((uint32_t)(i * 2654435761u + scale), scale);
            ASSERT_TRUE(chord_progression_get_root() < 7);
        }
    }
}

TEST(chord_progression_visits_multiple_functions) {
    /* Over 200 steps with varying rng, the walk should visit at
       least 3 distinct chord functions on a major-flavored scale. */
    chord_progression_init();
    uint8_t visited[7] = {0};
    for (uint32_t i = 0; i < 200; i++) {
        chord_progression_step(i * 0x9E3779B9u, 1 /* Lydian -> major table */);
        visited[chord_progression_get_root()] = 1;
    }
    int distinct = 0;
    for (int i = 0; i < 7; i++) if (visited[i]) distinct++;
    ASSERT_TRUE(distinct >= 3);
}

TEST(chord_progression_walk_is_deterministic) {
    chord_progression_init();
    uint8_t seq_a[50];
    for (int i = 0; i < 50; i++) {
        chord_progression_step((uint32_t)(i * 17u), 1);
        seq_a[i] = chord_progression_get_root();
    }
    /* Reset and replay the exact same rng sequence with same scale;
       must produce identical results. */
    chord_progression_init();
    for (int i = 0; i < 50; i++) {
        chord_progression_step((uint32_t)(i * 17u), 1);
        ASSERT_EQ(chord_progression_get_root(), seq_a[i]);
    }
}

TEST(chord_progression_different_scales_can_differ) {
    /* Same rng sequence on major vs minor table should sometimes
       produce different roots (their weight rows differ). */
    chord_progression_init();
    uint8_t seq_major[100];
    for (int i = 0; i < 100; i++) {
        chord_progression_step((uint32_t)(i * 13u + 7u), 1);
        seq_major[i] = chord_progression_get_root();
    }
    chord_progression_init();
    int differ = 0;
    for (int i = 0; i < 100; i++) {
        chord_progression_step((uint32_t)(i * 13u + 7u), 0 /* Dorian -> minor */);
        if (chord_progression_get_root() != seq_major[i]) differ++;
    }
    ASSERT_TRUE(differ > 0);
}

TEST(chord_progression_tracks_prev_root) {
    chord_progression_init();
    ASSERT_EQ(chord_progression_get_prev_root(), 0);
    chord_progression_step(0xABCDEF01u, 1);
    uint8_t after_first = chord_progression_get_root();
    chord_progression_step(0x12345678u, 1);
    /* prev_root captured at the top of the second step should equal
       what current_root was after the first step. */
    ASSERT_EQ(chord_progression_get_prev_root(), after_first);
}

int main(void) {
    return RUN_ALL();
}
