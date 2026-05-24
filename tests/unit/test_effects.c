/* Unit tests for effects.c — focus on the compressor + limiter
   contract. The other effects (reverb, delay, soft sat) are
   exercised by test_keys via their adjust setters and by the
   regression render. */
#include "test.h"
#include "../../effects.h"
#include <stdint.h>
#include <string.h>

/* effects_init() is now idempotent (per-init guards on the arena
   allocations), so each test can call it freely to reset the
   delay/reverb buffers + envelope + threshold to defaults. */
static void reset_effects(void) {
    effects_init();
}

/* Helper: fill a stereo buffer with a constant amplitude on both
   channels. */
static void fill_const(int16_t *buf, uint32_t frames, int16_t amp) {
    for (uint32_t i = 0; i < frames; i++) {
        buf[2 * i]     = amp;
        buf[2 * i + 1] = amp;
    }
}

/* ---- compressor / limiter ---- */

TEST(compressor_passes_quiet_signal_with_makeup_only) {
    /* Input well below threshold (default 20000). Output should be
       input * makeup gain (~+1 dB, COMP_MAKEUP_GAIN/256 = 288/256
       = 1.125). Allow small tolerance for the envelope ramp. */
    reset_effects();
    int16_t buf[256 * 2];
    fill_const(buf, 256, 5000);
    compressor_process(buf, 256);
    /* Sample 200 (well past attack ramp): expect ~5000 * 1.125 = 5625. */
    int16_t out = buf[200 * 2];
    ASSERT_BETWEEN(out, 5500, 5800);
}

TEST(compressor_reduces_amplitude_above_threshold) {
    /* Input at 30000 (above threshold 20000). Steady-state output
       must be below the brickwall and below the input. */
    reset_effects();
    int16_t buf[1024 * 2];
    fill_const(buf, 1024, 30000);
    compressor_process(buf, 1024);
    /* Sample 1000: well past attack settling. */
    int16_t out = buf[1000 * 2];
    ASSERT_TRUE(out < 30000);
    ASSERT_TRUE(out <= 32000);
}

TEST(compressor_never_exceeds_brickwall_ceiling) {
    /* Input at full int16 scale. Every output sample must be in
       [-32000, +32000]. */
    reset_effects();
    int16_t buf[1024 * 2];
    fill_const(buf, 1024, 32767);
    compressor_process(buf, 1024);
    for (uint32_t i = 0; i < 1024 * 2; i++) {
        ASSERT_BETWEEN(buf[i], -32000, 32000);
    }
}

TEST(compressor_envelope_persists_across_calls) {
    /* Verify the envelope state survives between compressor_process
       calls. Approach: warm up the envelope past the compression
       threshold with a long ramp buffer, then check that the first
       sample of a subsequent loud buffer is already compressed
       (envelope state carried, gain reduction applied immediately). */
    reset_effects();
    /* 4800-sample warm-up (~100 ms) at full scale is long enough
       for the ~5 ms attack envelope to fully settle past threshold. */
    static int16_t warm[4800 * 2];
    fill_const(warm, 4800, 30000);
    compressor_process(warm, 4800);
    /* Now the envelope is at steady state (~30000). Process one
       more loud buffer: its first sample MUST be gain-reduced
       (output below brickwall ceiling, well below the input). */
    int16_t b[64 * 2];
    fill_const(b, 64, 30000);
    compressor_process(b, 64);
    ASSERT_TRUE(b[0] < 30000);
}

TEST(compressor_threshold_adjuster_clamps) {
    reset_effects();
    /* Drive far below the floor. */
    for (int i = 0; i < 100; i++) compressor_adjust_threshold(-1000);
    ASSERT_EQ(compressor_get_threshold(), 8000);
    /* Drive far above the ceiling. */
    for (int i = 0; i < 100; i++) compressor_adjust_threshold(+1000);
    ASSERT_EQ(compressor_get_threshold(), 30000);
}

int main(void) {
    return RUN_ALL();
}
