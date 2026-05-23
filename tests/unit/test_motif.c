/* Unit tests for motif.c. */
#include "test.h"
#include "../../motif.h"
#include <stdint.h>

TEST(motif_init_resets_state) {
    motif_init();
    ASSERT_FALSE(motif_in_replay());
    /* Replay-at on a fresh init must return MOTIF_NO_NOTE since
       we're not in replay mode. */
    ASSERT_EQ(motif_replay_at(0), MOTIF_NO_NOTE);
}

TEST(motif_record_in_capture_then_replay_returns_same) {
    motif_init();
    motif_record(3, 5);     /* bar 0, step 3 -> slot 3 */
    motif_record(7, 2);     /* bar 0, step 7 -> slot 7 */

    /* Force into replay of phrase 0 with transpose 0. We have to
       drive bar_step enough to trigger replay; instead, exploit
       the fact that motif_in_replay starts false and motif_replay_at
       returns MOTIF_NO_NOTE outside replay. So drive bar_step
       30+ times with rng forcing the trigger. */
    /* Drive 30 bar_steps with rng=0. The 30th call sets
       bars_since_replay=30, hits the threshold, fires the trigger.
       Picker: (rng>>8)%8=0 phrase; (rng>>16)&3=0 transpose=0. */
    for (int i = 0; i < 30; i++) motif_bar_step((uint32_t)i, 0);
    ASSERT_TRUE(motif_in_replay());

    /* Replay should return the recorded degrees. */
    ASSERT_EQ(motif_replay_at(3), 5);
    ASSERT_EQ(motif_replay_at(7), 2);
}

TEST(motif_replay_at_empty_slot_returns_no_note) {
    motif_init();
    motif_record(5, 4);
    for (int i = 0; i < 30; i++) motif_bar_step((uint32_t)i, 0);
    ASSERT_TRUE(motif_in_replay());
    /* Slot 0 was never recorded into. */
    ASSERT_EQ(motif_replay_at(0), MOTIF_NO_NOTE);
    /* Slot 5 was recorded with degree 4. */
    ASSERT_EQ(motif_replay_at(5), 4);
}

TEST(motif_record_is_noop_during_replay) {
    motif_init();
    motif_record(2, 6);   /* in capture mode, written */
    for (int i = 0; i < 30; i++) motif_bar_step((uint32_t)i, 0);
    ASSERT_TRUE(motif_in_replay());
    /* Try to record while in replay - should be a no-op. */
    motif_record(2, 1);
    /* Replay should still return the original degree 6, not 1. */
    ASSERT_EQ(motif_replay_at(2), 6);
}

TEST(motif_bar_step_does_not_replay_before_threshold) {
    motif_init();
    /* 29 ticks should NOT trigger replay (threshold is 30 bars). */
    for (int i = 0; i < 29; i++) motif_bar_step((uint32_t)i, 0);
    ASSERT_FALSE(motif_in_replay());
}

TEST(motif_replay_exits_after_phrase_length) {
    motif_init();
    motif_record(0, 3);
    for (int i = 0; i < 30; i++) motif_bar_step((uint32_t)i, 0);
    ASSERT_TRUE(motif_in_replay());
    /* Replay lasts MOTIF_PHRASE_BARS = 4 ticks total. */
    motif_bar_step(32, 0);   /* replay_bar 1 */
    motif_bar_step(33, 0);   /* replay_bar 2 */
    motif_bar_step(34, 0);   /* replay_bar 3 */
    motif_bar_step(35, 0);   /* replay_bar 4 -> exit */
    ASSERT_FALSE(motif_in_replay());
}

TEST(motif_replay_transposes_degrees) {
    motif_init();
    motif_record(0, 2);
    /* Drive 29 bar_steps with rng=0 (below the threshold), then the
       30th with rng=0x20000 so the trigger fires with transpose +2.
       Picker reads (rng>>16)&3 = 2 -> case 2 returns +2. */
    for (int i = 0; i < 29; i++) motif_bar_step((uint32_t)i, 0);
    motif_bar_step(29, 0x20000u);
    ASSERT_TRUE(motif_in_replay());
    /* recorded 2 + transpose 2 = 4. */
    ASSERT_EQ(motif_replay_at(0), 4);
}

TEST(motif_replay_transpose_wraps_mod_7) {
    motif_init();
    motif_record(0, 6);
    for (int i = 0; i < 29; i++) motif_bar_step((uint32_t)i, 0);
    motif_bar_step(29, 0x20000u);
    ASSERT_TRUE(motif_in_replay());
    /* recorded 6 + 2 = 8 -> wrap mod 7 -> 1. */
    ASSERT_EQ(motif_replay_at(0), 1);
}

int main(void) {
    return RUN_ALL();
}
