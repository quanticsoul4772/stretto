#include "motif.h"
#include <stdint.h>
#include <string.h>

/* Ring of captured phrases. Each slot is a 64-element array of
   degrees with MOTIF_NO_NOTE marking empty positions (Euclidean
   hits that the gate suppressed). */
static uint8_t ring[MOTIF_RING_SIZE][MOTIF_PHRASE_SLOTS];

/* Capture state. */
static uint8_t  capture_phrase_idx = 0;
static uint8_t  bar_in_phrase      = 0;
static uint32_t bars_since_replay  = 0;

/* Replay state. */
static uint8_t  in_replay        = 0;
static uint8_t  replay_phrase    = 0;
static int8_t   replay_transpose = 0;
static uint8_t  replay_bar       = 0;

/* Replay-trigger threshold: a new replay can only start after this
   many bars since the last one. Combined with the per-bar chance
   below, average replay cadence works out near once every 30-50
   bars. */
#define MOTIF_REPLAY_MIN_GAP    30u
#define MOTIF_REPLAY_CHANCE_256 64u   /* 64/256 = 25% per bar after the gap */

static void clear_phrase(uint8_t idx) {
    memset(ring[idx], MOTIF_NO_NOTE, MOTIF_PHRASE_SLOTS);
}

void motif_init(void) {
    for (uint8_t i = 0; i < MOTIF_RING_SIZE; i++) clear_phrase(i);
    capture_phrase_idx = 0;
    bar_in_phrase      = 0;
    bars_since_replay  = 0;
    in_replay          = 0;
    replay_phrase      = 0;
    replay_transpose   = 0;
    replay_bar         = 0;
}

void motif_record(uint8_t step_in_bar, uint8_t degree) {
    if (in_replay) return;
    if (step_in_bar >= MOTIF_STEPS_PER_BAR) return;
    if (bar_in_phrase >= MOTIF_PHRASE_BARS) return;
    uint8_t slot = (uint8_t)(bar_in_phrase * MOTIF_STEPS_PER_BAR + step_in_bar);
    ring[capture_phrase_idx][slot] = degree;
}

uint8_t motif_replay_at(uint8_t step_in_bar) {
    if (!in_replay) return MOTIF_NO_NOTE;
    if (step_in_bar >= MOTIF_STEPS_PER_BAR) return MOTIF_NO_NOTE;
    if (replay_bar >= MOTIF_PHRASE_BARS) return MOTIF_NO_NOTE;
    uint8_t slot = (uint8_t)(replay_bar * MOTIF_STEPS_PER_BAR + step_in_bar);
    uint8_t d = ring[replay_phrase][slot];
    if (d == MOTIF_NO_NOTE) return MOTIF_NO_NOTE;
    /* Apply transposition mod 7 (degrees wrap in scale). */
    int td = (int)d + (int)replay_transpose;
    while (td < 0)  td += 7;
    while (td >= 7) td -= 7;
    return (uint8_t)td;
}

int motif_in_replay(void) {
    return in_replay;
}

/* Pick a transposition from a small weighted palette favoring
   verbatim (50%) with +/-2 the only non-zero options (25% each).
   Modest range keeps the replay recognizable as the original
   phrase. */
static int8_t pick_transpose(uint32_t rng) {
    uint32_t r = rng & 3u;
    if (r == 0u || r == 1u) return 0;
    if (r == 2u) return  2;
    return -2;
}

void motif_bar_step(uint32_t bar, uint32_t rng) {
    (void)bar;       /* state is internally maintained; bar is informational */

    if (in_replay) {
        replay_bar++;
        if (replay_bar >= MOTIF_PHRASE_BARS) {
            /* Replay complete - return to capture in a fresh phrase
               slot. Overwrites the oldest entry next time around. */
            in_replay = 0;
            capture_phrase_idx = (uint8_t)((capture_phrase_idx + 1u) % MOTIF_RING_SIZE);
            clear_phrase(capture_phrase_idx);
            bar_in_phrase      = 0;
            bars_since_replay  = 0;
        }
        return;
    }

    /* Capture mode: tick into the next bar of the current phrase. */
    bar_in_phrase++;
    if (bar_in_phrase >= MOTIF_PHRASE_BARS) {
        capture_phrase_idx = (uint8_t)((capture_phrase_idx + 1u) % MOTIF_RING_SIZE);
        clear_phrase(capture_phrase_idx);
        bar_in_phrase = 0;
    }
    bars_since_replay++;

    /* Replay-trigger gate. */
    if (bars_since_replay >= MOTIF_REPLAY_MIN_GAP
        && (rng & 0xFFu) < MOTIF_REPLAY_CHANCE_256) {
        uint8_t pick = (uint8_t)((rng >> 8) % MOTIF_RING_SIZE);
        /* Avoid replaying the currently-capturing slot (it may be
           partially filled, or about to be overwritten). */
        if (pick == capture_phrase_idx) {
            pick = (uint8_t)((pick + MOTIF_RING_SIZE - 1u) % MOTIF_RING_SIZE);
        }
        replay_phrase    = pick;
        replay_transpose = pick_transpose(rng >> 16);
        replay_bar       = 0;
        in_replay        = 1;
    }
}
