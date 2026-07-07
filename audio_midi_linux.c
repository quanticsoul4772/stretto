/* audio_midi_linux.c - libasound ALSA sequencer backend (stub).
 *
 * Phase 1+2 stub: returns -1 from audio_midi_linux_init so audio_play()
 * in live mode does not spawn any pthread. US1 (T022) replaces the body
 * with the real pthread_create worker looping on blocking
 * snd_seq_event_input(), per the preflight-corrected ALSA threading
 * pattern (commit d41d76a): snd_seq_create_thread() is NOT a stock
 * libasound API; the standard pthread + blocking-input pattern is
 * used instead.
 *
 * Compile guard: this file is only built on Linux (Makefile lists
 * audio_midi_linux.o in OBJS but never in WIN_OBJS). No cross-platform
 * #ifdef needed - the linker never sees these symbols on Windows.
 */
#include "audio_midi.h"

/* Drag in libasound's symbol references so the synth link step finds
 * libasound2 at link time (matches existing -lasound pattern in
 * Makefile). The functions below are stubs returning -1 so nothing
 * actually fires until US1 lands a real implementation.
 */
#include <alsa/seq.h>  /* snd_seq_open / snd_seq_event_input / etc. - real impl uses these */

int audio_midi_linux_init(int device_index) {
    (void)device_index;
    /* US1 (T022) replaces with: snd_seq_open + subscribe + pthread_create */
    return -1;
}

void audio_midi_linux_close(void) {
    /* US1 (T022) replaces with: signal-flag + pthread_join */
}

int audio_midi_linux_list_devices(midi_input_device_t *out, int32_t *count) {
    (void)out; (void)count;
    /* US1 (T034) replaces with: snd_seq_query_next_client + snd_seq_query_get_port_info */
    return -1;
}
