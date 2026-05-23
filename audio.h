#ifndef AUDIO_H
#define AUDIO_H

/* Live-audio playback. Blocks until the user presses 'q'. The
   actual implementation is platform-specific: audio_pulse.c on
   Linux (PulseAudio via pa_threaded_mainloop), audio_winmm.c on
   Windows (Win32 waveOut). Only one is built per platform; the
   Makefile selects via _WIN32. */
void audio_play(void);

#endif
