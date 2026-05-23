#ifndef MOTIF_H
#define MOTIF_H

#include <stdint.h>

/* Long-term motif memory: capture the last few 4-bar main-melody
 * phrases into a ring buffer; every ~30 bars, replay one of them
 * (verbatim or with a small diatonic transposition) instead of
 * generating new material via the L-system. The listener notices
 * recurrence and reads it as intent.
 *
 * Pure function of bar count + caller-supplied PRNG values; module
 * holds no PRNG state of its own. */

#define MOTIF_PHRASE_BARS     4u    /* one phrase = 4 bars (~8 s) */
#define MOTIF_STEPS_PER_BAR  16u    /* matches the Euclidean grid */
#define MOTIF_PHRASE_SLOTS   64u    /* 16 * 4 */
#define MOTIF_RING_SIZE       8u    /* keep the last 8 phrases */
#define MOTIF_NO_NOTE       0xFFu   /* sentinel: empty slot */

/* Reset all state. Called from gen_init. */
void    motif_init(void);

/* Advance per-bar state machine. Called once per bar from
   schedule_bar_boundary. rng is consumed only when deciding whether
   to start a new replay and to pick the phrase + transposition;
   if no decision happens this bar, rng is ignored. */
void    motif_bar_step(uint32_t bar, uint32_t rng);

/* Capture: record a degree the main melody just fired at the given
   step_in_bar (0..15). Slot is computed internally from the
   current bar-in-phrase. No-op when in replay mode. */
void    motif_record(uint8_t step_in_bar, uint8_t degree);

/* Replay: return the recorded degree (with transposition applied)
   for the given step_in_bar of the currently-replaying phrase, or
   MOTIF_NO_NOTE if that slot was empty in the original capture.
   Returns MOTIF_NO_NOTE when not in replay mode. Caller is expected
   to snap to the active mask. */
uint8_t motif_replay_at(uint8_t step_in_bar);

/* True while a captured phrase is being replayed back. */
int     motif_in_replay(void);

#endif
