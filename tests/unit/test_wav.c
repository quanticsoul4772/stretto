/* Unit tests for wav.c (render_wav + WAV header). */
#include "test.h"
#include "../../wav.h"
#include "../../mixer.h"
#include "../../gen.h"
#include "../../voice.h"
#include "../../effects.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int env_ready = 0;
static void ensure_init(void) {
    if (!env_ready) {
        voice_pool_init();
        effects_init();
        gen_seed(0);
        gen_init();
        env_ready = 1;
    }
}

/* Helpers to read little-endian fields from the WAV header. */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

TEST(wav_header_has_correct_fields) {
    ensure_init();
    const char *path = "/tmp/test_wav_unit.wav";
    render_wav(1, path);

    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    uint8_t hdr[44];
    size_t n = fread(hdr, 1, 44, f);
    fclose(f);
    unlink(path);

    ASSERT_EQ(n, 44);
    /* "RIFF" magic */
    ASSERT_EQ(memcmp(hdr,      "RIFF", 4), 0);
    /* "WAVE" + "fmt " chunk header */
    ASSERT_EQ(memcmp(hdr + 8,  "WAVE", 4), 0);
    ASSERT_EQ(memcmp(hdr + 12, "fmt ", 4), 0);
    /* fmt chunk size = 16 (PCM) */
    ASSERT_EQ(rd32(hdr + 16), 16u);
    /* audio format = 1 (PCM uncompressed) */
    ASSERT_EQ(rd16(hdr + 20), 1u);
    /* channels = 2 */
    ASSERT_EQ(rd16(hdr + 22), 2u);
    /* sample rate */
    ASSERT_EQ(rd32(hdr + 24), (uint32_t)SAMPLE_RATE);
    /* byte rate = sample_rate * channels * bits/8 = 48000 * 2 * 2 = 192000 */
    ASSERT_EQ(rd32(hdr + 28), (uint32_t)SAMPLE_RATE * 4u);
    /* block align = channels * bits/8 = 4 */
    ASSERT_EQ(rd16(hdr + 32), 4u);
    /* bits per sample = 16 */
    ASSERT_EQ(rd16(hdr + 34), 16u);
    /* "data" chunk header */
    ASSERT_EQ(memcmp(hdr + 36, "data", 4), 0);
    /* data size = 1 second * 48000 frames * 4 bytes/frame */
    ASSERT_EQ(rd32(hdr + 40), (uint32_t)SAMPLE_RATE * 4u);
}

TEST(wav_file_size_matches_seconds) {
    ensure_init();
    const char *path = "/tmp/test_wav_size.wav";
    int seconds = 2;
    render_wav(seconds, path);

    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    unlink(path);

    /* 44-byte header + (seconds * SAMPLE_RATE) frames * 4 bytes/frame. */
    long expected = 44 + (long)seconds * SAMPLE_RATE * 4;
    ASSERT_EQ(sz, expected);
}

int main(void) {
    return RUN_ALL();
}
