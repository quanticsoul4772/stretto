/* Unit tests for section.c. */
#include "test.h"
#include "../../section.h"
#include <string.h>

TEST(section_starts_at_intro) {
    section_init();
    ASSERT_EQ(section_current(), SEC_INTRO);
}

TEST(section_advances_through_cycle_at_correct_bars) {
    section_init();
    /* Bars 0..23 = INTRO; 24..47 = BODY; 48..71 = TENSION; 72..95 = RESOLVE. */
    section_step(0);   ASSERT_EQ(section_current(), SEC_INTRO);
    section_step(23);  ASSERT_EQ(section_current(), SEC_INTRO);
    section_step(24);  ASSERT_EQ(section_current(), SEC_BODY);
    section_step(47);  ASSERT_EQ(section_current(), SEC_BODY);
    section_step(48);  ASSERT_EQ(section_current(), SEC_TENSION);
    section_step(71);  ASSERT_EQ(section_current(), SEC_TENSION);
    section_step(72);  ASSERT_EQ(section_current(), SEC_RESOLVE);
    section_step(95);  ASSERT_EQ(section_current(), SEC_RESOLVE);
}

TEST(section_wraps_at_period) {
    section_init();
    section_step(96);
    ASSERT_EQ(section_current(), SEC_INTRO);
    section_step(120);    /* 96 + 24, BODY */
    ASSERT_EQ(section_current(), SEC_BODY);
}

TEST(section_kick_pattern_switches_per_section) {
    section_init();
    section_step(0);    ASSERT_EQ(section_kick_pattern(), 0);  /* INTRO sparse */
    section_step(24);   ASSERT_EQ(section_kick_pattern(), 0);  /* BODY */
    section_step(48);   ASSERT_EQ(section_kick_pattern(), 2);  /* TENSION 4-on-floor */
    section_step(72);   ASSERT_EQ(section_kick_pattern(), 0);  /* RESOLVE sparse */
}

TEST(section_lsystem_character_per_section) {
    section_init();
    section_step(0);    ASSERT_EQ(section_lsystem_character(), 2);  /* INTRO sparse */
    section_step(24);   ASSERT_EQ(section_lsystem_character(), 0);  /* BODY stepwise */
    section_step(48);   ASSERT_EQ(section_lsystem_character(), 1);  /* TENSION leaping */
    section_step(72);   ASSERT_EQ(section_lsystem_character(), 0);  /* RESOLVE stepwise */
}

TEST(section_bias_crossfades_near_boundary) {
    section_init();
    /* Mid-INTRO (bar 12): bias should equal the unblended INTRO value. */
    section_step(12);
    int8_t mid_intro = section_bias_gate();
    /* End of INTRO (bar 23): blending toward BODY. */
    section_step(23);
    int8_t end_intro = section_bias_gate();
    /* Start of BODY (bar 24): blending in from INTRO. */
    section_step(24);
    int8_t start_body = section_bias_gate();
    /* Mid-BODY (bar 36): unblended BODY value (0). */
    section_step(36);
    int8_t mid_body = section_bias_gate();

    /* Mid-INTRO is the pure INTRO gate bias (-64). Mid-BODY is 0.
       The transition values should fall between -64 and 0. */
    ASSERT_EQ(mid_intro, -64);
    ASSERT_EQ(mid_body, 0);
    ASSERT_TRUE(end_intro > -64 && end_intro <= 0);
    ASSERT_TRUE(start_body > -64 && start_body <= 0);
}

TEST(section_name_strings_are_set) {
    section_init();
    section_step(0);    ASSERT_TRUE(strcmp(section_name(), "intro") == 0);
    section_step(24);   ASSERT_TRUE(strcmp(section_name(), "body")  == 0);
    section_step(48);   ASSERT_TRUE(strcmp(section_name(), "tens")  == 0);
    section_step(72);   ASSERT_TRUE(strcmp(section_name(), "res")   == 0);
}

int main(void) {
    return RUN_ALL();
}
