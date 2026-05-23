/* Unit tests for density.c. */
#include "test.h"
#include "../../density.h"
#include <stdint.h>

TEST(density_tension_low_for_sparse) {
    density_update(0x01u, 32);   /* one degree, min gate */
    uint8_t t = density_get_tension();
    /* popcount=1 * 18 + 32/4=8 = 26. Below mid (128). */
    ASSERT_TRUE(t < DENSITY_TENSION_MID);
}

TEST(density_tension_high_for_dense) {
    density_update(0x7Fu, 255);  /* all 7 degrees, max gate */
    uint8_t t = density_get_tension();
    /* popcount=7 * 18 + 255/4=63 = 189. Above mid (128). */
    ASSERT_TRUE(t > DENSITY_TENSION_MID);
}

TEST(density_tension_stays_in_byte_range) {
    /* No combination should overflow 255. */
    density_update(0x7Fu, 255);
    ASSERT_TRUE(density_get_tension() <= 255);
}

TEST(density_bias_gate_inverse_to_tension) {
    /* High tension -> negative gate bias (pull back). */
    density_update(0x7Fu, 255);
    int8_t high_bias = density_bias_gate();
    /* Low tension -> positive gate bias (fill in). */
    density_update(0x01u, 32);
    int8_t low_bias = density_bias_gate();
    ASSERT_TRUE(high_bias < 0);
    ASSERT_TRUE(low_bias > 0);
    ASSERT_TRUE(low_bias > high_bias);
}

TEST(density_bias_reverb_inverse_to_tension) {
    density_update(0x7Fu, 255);
    int8_t high_bias = density_bias_reverb();
    density_update(0x01u, 32);
    int8_t low_bias = density_bias_reverb();
    ASSERT_TRUE(high_bias < 0);
    ASSERT_TRUE(low_bias > 0);
}

TEST(density_bias_zero_at_mid_tension) {
    /* Construct an input that lands near the mid tension. With
       formula popcount*18 + gate/4, a midpoint near 128 is e.g.
       popcount=4 (72) + gate=224 (56) = 128. */
    density_update(0x0Fu, 224);              /* 4 degrees + gate 224 */
    int8_t gb = density_bias_gate();
    int8_t rb = density_bias_reverb();
    /* Allow a small window because integer division can lose 1. */
    ASSERT_TRUE(gb >= -2 && gb <= 2);
    ASSERT_TRUE(rb >= -4 && rb <= 4);
}

TEST(density_bias_magnitudes_modest) {
    /* Worst case: tension goes 0..255 (impossible in practice but
       the math should still produce sane deltas). */
    density_update(0u, 0);          /* tension=0 */
    int8_t gb_max_pos = density_bias_gate();
    int8_t rb_max_pos = density_bias_reverb();
    density_update(0x7Fu, 255);     /* tension=189 */
    int8_t gb_neg = density_bias_gate();
    int8_t rb_neg = density_bias_reverb();
    /* Tension 0 -> gate bias = (128-0)/8 = +16. */
    ASSERT_EQ(gb_max_pos, 16);
    /* Tension 0 -> reverb bias = (128-0)/4 = +32. */
    ASSERT_EQ(rb_max_pos, 32);
    /* Tension 189 -> gate bias = (128-189)/8 = -7. */
    ASSERT_EQ(gb_neg, -7);
    /* Tension 189 -> reverb bias = (128-189)/4 = -15. */
    ASSERT_EQ(rb_neg, -15);
}

int main(void) {
    return RUN_ALL();
}
