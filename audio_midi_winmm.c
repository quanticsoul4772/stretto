/* audio_midi_winmm.c - Win32 midiInProc backend (stub).
 *
 * Phase 1+2 stub. Real implementation lands in US1 (T023):
 *   midiInOpen(&handle, dev=device_index, (DWORD_PTR)&MidiInProc,
 *              instance, CALLBACK_FUNCTION) + midiInStart(handle).
 *   MidiInProc parses MIM_DATA messages:
 *     - status byte param1 (low byte): high nibble = event type (NoteOn
 *       0x90 / NoteOff 0x80 / CC 0xB0), low nibble = channel (0..15 -> +1).
 *     - data1 (param1>>8 & 0xFF): key/CC#, data2 (param1>>16 & 0xFF): value.
 *   MIM_CLOSE signals FR-034 disconnect.
 *
 * Compile guard: this file is only built on Windows cross-compiles
 * (Makefile lists audio_midi_winmm.win.o in WIN_OBJS). winmm is already
 * linked for waveOut so the synth link step finds mmi* symbols.
 */
#include "audio_midi.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <mmsystem.h>  /* midiInOpen / midiInProc / midiInGetDevCaps */
#endif

int audio_midi_winmm_init(int device_index) {
    (void)device_index;
    /* US1 (T023) replaces with: midiInOpen + midiInStart. */
    return -1;
}

void audio_midi_winmm_close(void) {
    /* US1 (T023) replaces with: midiInStop + midiInClose. */
}

int audio_midi_winmm_list_devices(midi_input_device_t *out, int32_t *count) {
    (void)out; (void)count;
    /* US1 (T035) replaces with: midiInGetNumDevs + midiInGetDevCaps. */
    return -1;
}
