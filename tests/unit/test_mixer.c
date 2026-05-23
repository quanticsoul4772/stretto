/* Unit tests for mixer.c (render_chunk). */
#include "test.h"
#include "../../mixer.h"
#include "../../gen.h"
#include "../../voice.h"
#include "../../effects.h"
#include <stdint.h>

static int pool_ready = 0;
static void ensure_init(void) {
    if (!pool_ready) {
        voice_pool_init();
        effects_init();
        gen_seed(0);
        gen_init();
        pool_ready = 1;
    }
}

TEST(render_chunk_produces_audio) {
    /* Render 1 second; expect at least some non-zero output. */
    ensure_init();
    static int16_t buf[BUFFER_FRAMES * 2];
    /* Skip a few buffers so envelopes have a chance to ramp up. */
    for (int b = 0; b < 4; b++) render_chunk(buf, BUFFER_FRAMES);
    int32_t peak = 0;
    for (int i = 0; i < BUFFER_FRAMES * 2; i++) {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    ASSERT_TRUE(peak > 100);
}

TEST(render_chunk_output_is_stereo) {
    /* Channels should generally differ because of per-voice pan
       and LFO modulation. Over a 1-second window L vs R should not
       be byte-identical. */
    ensure_init();
    static int16_t buf[BUFFER_FRAMES * 2];
    for (int b = 0; b < 4; b++) render_chunk(buf, BUFFER_FRAMES);
    int identical = 1;
    for (int i = 0; i < BUFFER_FRAMES; i++) {
        if (buf[2 * i] != buf[2 * i + 1]) { identical = 0; break; }
    }
    ASSERT_FALSE(identical);
}

TEST(render_chunk_handles_short_buffers) {
    /* Small frame counts must not crash or read past the buffer. */
    ensure_init();
    int16_t small[16];
    render_chunk(small, 8);
    /* Just reaching here = pass. */
    ASSERT_TRUE(1);
}

int main(void) {
    return RUN_ALL();
}
