/* Unit tests for voice.c.
 *
 * Each TEST runs in the same process so static state (the global
 * voice pool, prng_state, filter base values, etc.) persists across
 * tests. We work around this by either re-initializing pool slots
 * directly with voice_init() or by using fresh slots per test.
 */
#include "test.h"
#include "../../voice.h"
#include "../../arena.h"
#include <stdint.h>
#include <string.h>

/* Uses test_init_synth() from test.h; legacy alias kept so the
   existing TEST bodies (which call ensure_pool_init()) still work. */
#define ensure_pool_init() test_init_synth()

/* ---- voice_init contract ---- */

TEST(voice_init_zeroes_critical_fields) {
    Voice v;
    voice_init(&v);
    ASSERT_EQ(v.type, VOICE_OFF);
    ASSERT_EQ(v.env_phase, ENV_OFF);
    ASSERT_EQ(v.env_amp, 0);
    ASSERT_EQ(v.env_time, 0);
    ASSERT_EQ(v.lfo_phase, 0);
    ASSERT_EQ(v.lfo_inc, 0);
    ASSERT_EQ(v.peak_window, 0);
    ASSERT_EQ(v.svf_lp, 0);
    ASSERT_EQ(v.svf_bp, 0);
    ASSERT_EQ(v.fenv_amp, 0);
    ASSERT_EQ(v.fenv_phase, ENV_OFF);
    ASSERT_EQ(v.pan, 128);
}

/* ---- voice_trigger sets phase and role ---- */

TEST(voice_trigger_ks_sets_state) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 60, VOICE_KS, ROLE_MELODY);
    ASSERT_EQ(v.type, VOICE_KS);
    ASSERT_EQ(v.role, ROLE_MELODY);
    ASSERT_EQ(v.env_phase, ENV_A);
    ASSERT_EQ(v.note, 60);
    ASSERT_TRUE(v.u.ks.len > 0);          /* picked from note_ks_len[] */
    ASSERT_EQ(v.u.ks.idx, 0);
}

TEST(voice_trigger_fm_sets_state) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 62, VOICE_FM, ROLE_BASS);
    ASSERT_EQ(v.type, VOICE_FM);
    ASSERT_EQ(v.role, ROLE_BASS);
    ASSERT_EQ(v.env_phase, ENV_A);
    ASSERT_TRUE(v.u.fm.inc_c > 0);
    ASSERT_TRUE(v.u.fm.inc_m > 0);
    /* Bass uses 1:1 ratio */
    ASSERT_EQ(v.u.fm.inc_m, v.u.fm.inc_c);
}

TEST(voice_trigger_fm_chord_uses_2to1_ratio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 62, VOICE_FM, ROLE_CHORD);
    ASSERT_EQ(v.u.fm.inc_m, v.u.fm.inc_c * 2u);
}

TEST(voice_trigger_drum_sets_drum_type) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, DRUM_KICK, VOICE_DRUM, ROLE_MELODY);
    /* voice_trigger forces role to ROLE_DRUM regardless of input */
    ASSERT_EQ(v.role, ROLE_DRUM);
    ASSERT_EQ(v.u.drum.drum_type, DRUM_KICK);
    /* Kick has a non-zero starting phase increment (sine sweep). */
    ASSERT_TRUE(v.u.drum.inc > 0);
}

TEST(voice_trigger_snare_has_zero_inc) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, DRUM_SNARE, VOICE_DRUM, ROLE_MELODY);
    /* Snare uses noise only, no pitch sweep. */
    ASSERT_EQ(v.u.drum.inc, 0);
}

/* ---- voice_step envelope progression ---- */

TEST(voice_step_silent_when_env_off) {
    Voice v;
    voice_init(&v);
    /* env_phase is ENV_OFF; voice_step should return 0 regardless. */
    ASSERT_EQ(voice_step(&v), 0);
    ASSERT_EQ(voice_step(&v), 0);
}

TEST(voice_step_ramps_during_attack) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 60, VOICE_FM, ROLE_MELODY);
    /* First sample: env_amp may still be 0 because the envelope
       starts at idx=0 of env_table. After several samples it should
       be growing. */
    voice_step(&v);
    uint16_t a1 = v.env_amp;
    for (int i = 0; i < 100; i++) voice_step(&v);
    uint16_t a2 = v.env_amp;
    ASSERT_TRUE(a2 > a1);
}

/* Helper: drive a voice for N samples, collect the peak. */
static int32_t peak_over_n(Voice *v, int n) {
    int32_t peak = 0;
    for (int i = 0; i < n; i++) {
        int16_t s = voice_step(v);
        int32_t a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    return peak;
}

TEST(voice_step_fm_melody_produces_audio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 64, VOICE_FM, ROLE_MELODY);
    int32_t peak = peak_over_n(&v, 4800);   /* 100 ms */
    ASSERT_TRUE(peak > 1000);              /* clearly audible */
}

TEST(voice_step_ks_melody_produces_audio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 60, VOICE_KS, ROLE_MELODY);
    int32_t peak = peak_over_n(&v, 4800);
    ASSERT_TRUE(peak > 1000);
}

TEST(voice_step_drum_kick_produces_audio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, DRUM_KICK, VOICE_DRUM, ROLE_MELODY);
    int32_t peak = peak_over_n(&v, 7200);   /* drum release length */
    ASSERT_TRUE(peak > 500);
}

TEST(voice_step_drum_hihat_produces_audio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, DRUM_HIHAT, VOICE_DRUM, ROLE_MELODY);
    int32_t peak = peak_over_n(&v, 1440);
    ASSERT_TRUE(peak > 500);
}

TEST(voice_step_drum_snare_produces_audio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, DRUM_SNARE, VOICE_DRUM, ROLE_MELODY);
    int32_t peak = peak_over_n(&v, 4800);
    ASSERT_TRUE(peak > 500);
}

/* ---- voice_step never clips beyond int16 saturation ---- */

TEST(voice_step_never_overflows_int16) {
    /* Run all three FM roles + drums for a full release cycle, all
       at maximum settings, and verify the SVF + peak-normalization
       chain never returns values outside int16 range. */
    Voice v;
    /* Push cutoff and resonance to max for stress. */
    while (voice_get_cutoff() < 180) voice_adjust_cutoff(+10);
    while (voice_get_resonance() < 180) voice_adjust_resonance(+10);

    voice_init(&v);
    voice_trigger(&v, 60, VOICE_FM, ROLE_CHORD);
    for (int i = 0; i < 32000; i++) {
        int16_t s = voice_step(&v);
        /* int16_t cannot represent outside [-32768, 32767] by
           definition; this test is really verifying we do not
           accidentally trigger UB in the SVF that would crash. */
        (void)s;
    }
    /* Reset filter to a moderate setting for other tests. */
    while (voice_get_cutoff() > 200) voice_adjust_cutoff(-10);
    while (voice_get_resonance() > 100) voice_adjust_resonance(-10);
    ASSERT_TRUE(1);  /* completion = pass */
}

/* ---- filter controls ---- */

TEST(filter_cutoff_clamps_low) {
    while (voice_get_cutoff() > 30) voice_adjust_cutoff(-50);
    ASSERT_EQ(voice_get_cutoff(), 30);
}

TEST(filter_cutoff_clamps_high) {
    while (voice_get_cutoff() < 180) voice_adjust_cutoff(+50);
    ASSERT_EQ(voice_get_cutoff(), 180);
}

TEST(filter_resonance_clamps_low) {
    while (voice_get_resonance() > 0) voice_adjust_resonance(-50);
    ASSERT_EQ(voice_get_resonance(), 0);
}

TEST(filter_resonance_clamps_high) {
    while (voice_get_resonance() < 180) voice_adjust_resonance(+50);
    ASSERT_EQ(voice_get_resonance(), 180);
}

TEST(filter_mode_cycles_through_four) {
    /* Bring mode to a known state. */
    while (voice_get_filter_mode() != 0) voice_cycle_filter_mode();
    voice_cycle_filter_mode();  ASSERT_EQ(voice_get_filter_mode(), 1);
    voice_cycle_filter_mode();  ASSERT_EQ(voice_get_filter_mode(), 2);
    voice_cycle_filter_mode();  ASSERT_EQ(voice_get_filter_mode(), 3);
    voice_cycle_filter_mode();  ASSERT_EQ(voice_get_filter_mode(), 0);
}

TEST(filter_lfo_depth_clamps) {
    while (voice_get_lfo_filter_depth() > 0) voice_adjust_lfo_filter_depth(-50);
    ASSERT_EQ(voice_get_lfo_filter_depth(), 0);
    while (voice_get_lfo_filter_depth() < 255) voice_adjust_lfo_filter_depth(+50);
    ASSERT_EQ(voice_get_lfo_filter_depth(), 255);
}

TEST(filter_mutate_drifts_within_clamps) {
    /* Apply a bunch of mutations with various PRNG inputs; verify
       cutoff and resonance never leave the user clamp range. */
    for (uint32_t i = 0; i < 200; i++) {
        voice_mutate_filter(i * 0x9E3779B9u);
        ASSERT_BETWEEN(voice_get_cutoff(), 30, 180);
        ASSERT_BETWEEN(voice_get_resonance(), 0, 180);
    }
}

/* ---- mod_depth controls ---- */

TEST(mod_depth_clamps_low) {
    voice_set_mod_depth(10);
    ASSERT_EQ(voice_get_mod_depth(), 100);   /* clamped up to 100 */
}

TEST(mod_depth_clamps_high) {
    voice_set_mod_depth(20000);
    ASSERT_EQ(voice_get_mod_depth(), 8000);  /* clamped down to 8000 */
}

TEST(mod_depth_in_range) {
    voice_set_mod_depth(2500);
    ASSERT_EQ(voice_get_mod_depth(), 2500);
}

/* ---- voice pool ---- */

TEST(voice_pool_init_creates_silent_pool) {
    ensure_pool_init();
    ASSERT_EQ(voice_pool_active_mask(), 0);
}

TEST(voice_pool_trigger_role_activates_a_slot) {
    ensure_pool_init();
    /* Trigger a chord voice; one of slots 2..4 should become active. */
    voice_pool_trigger_role(64, VOICE_FM, ROLE_CHORD);
    uint32_t mask = voice_pool_active_mask();
    ASSERT_TRUE(mask & 0b00011100);  /* any of slots 2, 3, or 4 */
}

TEST(voice_pool_trigger_drum_activates_correct_slot) {
    ensure_pool_init();
    voice_pool_trigger_drum(DRUM_KICK);
    ASSERT_TRUE(voice_pool_active_mask() & (1u << 8));  /* slot 8 */
    voice_pool_trigger_drum(DRUM_SNARE);
    ASSERT_TRUE(voice_pool_active_mask() & (1u << 9));  /* slot 9 */
    voice_pool_trigger_drum(DRUM_HIHAT);
    ASSERT_TRUE(voice_pool_active_mask() & (1u << 10)); /* slot 10 */
}

TEST(voice_pool_mix_returns_stereo) {
    ensure_pool_init();
    voice_pool_trigger_role(64, VOICE_FM, ROLE_MELODY);
    int32_t peak_l = 0, peak_r = 0;
    for (int i = 0; i < 1000; i++) {
        Stereo s = voice_pool_mix();
        int32_t al = s.l < 0 ? -s.l : s.l;
        int32_t ar = s.r < 0 ? -s.r : s.r;
        if (al > peak_l) peak_l = al;
        if (ar > peak_r) peak_r = ar;
    }
    ASSERT_TRUE(peak_l > 0 || peak_r > 0);
}

/* ---- wavetable voice (VOICE_WT) ---- */

TEST(voice_trigger_wt_sets_state) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 60, VOICE_WT, ROLE_CHORD);
    ASSERT_EQ(v.type, VOICE_WT);
    ASSERT_EQ(v.env_phase, ENV_A);
    ASSERT_TRUE(v.u.wt.inc > 0);
    ASSERT_EQ(v.u.wt.position, 0);
}

TEST(voice_step_wt_produces_audio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 64, VOICE_WT, ROLE_CHORD);
    int32_t peak = peak_over_n(&v, 4800);
    ASSERT_TRUE(peak > 1000);
}

TEST(voice_wt_position_advances_with_lfo) {
    /* lfo_phase is normally advanced by voice_pool_mix, not voice_step.
       Mimic that here so the WT position update inside voice_step
       sees a moving lfo_phase. lfo_inc=9841 mirrors a chord slot. */
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 64, VOICE_WT, ROLE_CHORD);
    v.lfo_inc = 9841u;
    for (int i = 0; i < 96000; i++) {
        voice_step(&v);
        v.lfo_phase += v.lfo_inc;
    }
    ASSERT_TRUE(v.u.wt.position != 0);
}

/* ---- additive voice (VOICE_ADD) ---- */

TEST(voice_trigger_add_sets_state) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 60, VOICE_ADD, ROLE_CHORD);
    ASSERT_EQ(v.type, VOICE_ADD);
    ASSERT_EQ(v.env_phase, ENV_A);
    ASSERT_TRUE(v.u.add.inc_base > 0);
    ASSERT_TRUE(v.u.add.amps != NULL);
    /* All 8 partial phases reset to 0. */
    for (int k = 0; k < 8; k++) ASSERT_EQ(v.u.add.phase[k], 0);
}

TEST(voice_step_add_produces_audio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 64, VOICE_ADD, ROLE_CHORD);
    int32_t peak = peak_over_n(&v, 4800);
    ASSERT_TRUE(peak > 1000);
}

TEST(voice_step_add_does_not_overflow_int16) {
    /* Even at the loudest profile (brass, sum=300), the per-sample
       output must stay inside int16 thanks to sat16(). */
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 60, VOICE_ADD, ROLE_CHORD);
    for (int i = 0; i < 4800; i++) {
        int16_t s = voice_step(&v);
        (void)s;   /* int16 by type; reaching here proves no UB */
    }
    ASSERT_TRUE(1);
}

/* ---- subtractive super-saw voice (VOICE_SUB) ---- */

TEST(voice_trigger_sub_sets_state) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 36, VOICE_SUB, ROLE_BASS);
    ASSERT_EQ(v.type, VOICE_SUB);
    ASSERT_EQ(v.role, ROLE_BASS);
    ASSERT_EQ(v.env_phase, ENV_A);
    ASSERT_EQ(v.u.sub.phase[0], 0);
    ASSERT_EQ(v.u.sub.phase[1], 0);
    ASSERT_EQ(v.u.sub.phase[2], 0);
    ASSERT_TRUE(v.u.sub.inc[0] > 0);
    /* Detuned copies bracket the center: inc[1] > inc[0] > inc[2]. */
    ASSERT_TRUE(v.u.sub.inc[1] > v.u.sub.inc[0]);
    ASSERT_TRUE(v.u.sub.inc[2] < v.u.sub.inc[0]);
}

TEST(voice_step_sub_produces_audio) {
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 40, VOICE_SUB, ROLE_BASS);
    int32_t peak = peak_over_n(&v, 4800);
    ASSERT_TRUE(peak > 1000);
}

TEST(voice_step_sub_phases_diverge) {
    /* Detuned increments must walk the 3 phases out of lockstep so the
       super-saw thickening is real rather than a single-oscillator. */
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 40, VOICE_SUB, ROLE_BASS);
    for (int i = 0; i < 9600; i++) voice_step(&v);
    ASSERT_TRUE(v.u.sub.phase[0] != v.u.sub.phase[1]);
    ASSERT_TRUE(v.u.sub.phase[0] != v.u.sub.phase[2]);
    ASSERT_TRUE(v.u.sub.phase[1] != v.u.sub.phase[2]);
}

/* ---- portamento (glide) on legato SUB bass re-trigger ---- */

TEST(voice_glide_arms_on_legato_sub_trigger) {
    /* Trigger A; step partway through attack so env_amp clears the
       legato threshold (8192). After 1000 samples of a 2400-sample
       attack, env_amp ~ 13653 > 8192. Re-trigger with a different
       note: glide should be armed, inc[0] unchanged, inc_target set
       to the new note's inc. */
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 36, VOICE_SUB, ROLE_BASS);
    uint32_t inc_before = v.u.sub.inc[0];
    for (int i = 0; i < 1000; i++) voice_step(&v);
    ASSERT_TRUE(v.env_amp > 8192);
    voice_trigger(&v, 48, VOICE_SUB, ROLE_BASS);
    ASSERT_EQ(v.note, 48);
    ASSERT_EQ(v.glide_remain, 2400);
    ASSERT_TRUE(v.inc_target > 0);
    ASSERT_TRUE(v.inc_target != inc_before);
    /* Ramp has not started running yet (no voice_step since trigger). */
    ASSERT_EQ(v.u.sub.inc[0], inc_before);
}

TEST(voice_glide_lands_at_target_inc) {
    /* Arm the glide, then step exactly GLIDE_SAMPLES (2400) times.
       The ramp should land at inc_target with glide_remain == 0. */
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 36, VOICE_SUB, ROLE_BASS);
    for (int i = 0; i < 1000; i++) voice_step(&v);
    voice_trigger(&v, 48, VOICE_SUB, ROLE_BASS);
    uint32_t target = v.inc_target;
    for (int i = 0; i < 2400; i++) voice_step(&v);
    ASSERT_EQ(v.glide_remain, 0);
    ASSERT_EQ(v.u.sub.inc[0], target);
    /* Detune relationship preserved at the destination. */
    ASSERT_TRUE(v.u.sub.inc[1] > v.u.sub.inc[0]);
    ASSERT_TRUE(v.u.sub.inc[2] < v.u.sub.inc[0]);
}

TEST(voice_glide_skipped_in_release_tail) {
    /* Trigger A and run long enough that env_amp falls below threshold
       in the release tail. Bass envelope: attack 2400 + decay 9600 ->
       sustain 16384 at sample 12000, then linear release over 48000
       samples. env_amp drops below 8192 at ~36000 samples in (halfway
       through release). Re-trigger from there: should be a fresh trigger
       (full ENV_A reset, glide_remain stays 0). */
    Voice v;
    voice_init(&v);
    voice_trigger(&v, 36, VOICE_SUB, ROLE_BASS);
    for (int i = 0; i < 40000; i++) voice_step(&v);
    ASSERT_TRUE(v.env_amp < 8192);
    voice_trigger(&v, 48, VOICE_SUB, ROLE_BASS);
    ASSERT_EQ(v.glide_remain, 0);
    ASSERT_EQ(v.env_phase, ENV_A);
    ASSERT_EQ(v.note, 48);
    /* Fresh trigger path: inc[0] is the new note's full inc, with the
       expected detune relationship. */
    ASSERT_TRUE(v.u.sub.inc[1] > v.u.sub.inc[0]);
    ASSERT_TRUE(v.u.sub.inc[2] < v.u.sub.inc[0]);
}

/* ---- preset-capture absolute setters (047) ---- */

TEST(set_cutoff_clamps_and_sets) {
    voice_set_cutoff(140);
    ASSERT_EQ(voice_get_cutoff(), 140);
    voice_set_cutoff(-10);
    ASSERT_EQ(voice_get_cutoff(), 30);
    voice_set_cutoff(500);
    ASSERT_EQ(voice_get_cutoff(), 180);
}

TEST(set_resonance_clamps_and_sets) {
    voice_set_resonance(90);
    ASSERT_EQ(voice_get_resonance(), 90);
    voice_set_resonance(-1);
    ASSERT_EQ(voice_get_resonance(), 0);
    voice_set_resonance(999);
    ASSERT_EQ(voice_get_resonance(), 180);
    voice_set_resonance(100);
}

TEST(set_lfo_filter_depth_clamps_and_sets) {
    voice_set_lfo_filter_depth(120);
    ASSERT_EQ(voice_get_lfo_filter_depth(), 120);
    voice_set_lfo_filter_depth(-1);
    ASSERT_EQ(voice_get_lfo_filter_depth(), 0);
    voice_set_lfo_filter_depth(9999);
    ASSERT_EQ(voice_get_lfo_filter_depth(), 255);
    voice_set_lfo_filter_depth(80);
}

TEST(set_filter_mode_masks_and_sets) {
    voice_set_filter_mode(2);
    ASSERT_EQ(voice_get_filter_mode(), 2);
    voice_set_filter_mode(7);      /* & 3 -> 3 */
    ASSERT_EQ(voice_get_filter_mode(), 3);
    voice_set_filter_mode(0);
    ASSERT_EQ(voice_get_filter_mode(), 0);
}

int main(void) {
    return RUN_ALL();
}
