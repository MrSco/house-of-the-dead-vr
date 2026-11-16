// The microphone's clean-up before voice chat encodes it (voice.c; ported from the Time Crisis 2 VR project's
// quest/tc2_voice_dsp.c): echo cancellation (SpeexDSP's MDF filter) against what this app plays, then noise
// suppression with a speech probability (RNNoise). 48 kHz mono, 10 ms blocks. Everything here runs on voice.c's
// worker thread: the audio callbacks only queue samples for it.
//
// Both sides are placed on one clock, in 48 kHz samples: what is played by when its block was handed to the speakers,
// the microphone by when it arrived. The echo of a played sample reaches the microphone some time later (the output's
// buffers, the air, the input's buffers); the filter learns that delay and the room, up to VDSP_TAIL_MS.
#pragma once
#include <stdint.h>

#define VDSP_RATE    48000
#define VDSP_BLOCK   480                 // 10 ms
#define VDSP_TAIL_MS 250

typedef void (*vdsp_out)(const int16_t *block, int n, float speech);

int  vdsp_init(void);                    // 1: both stages are there
void vdsp_reset(void);                   // a new session: forget the echo path and both timelines
void vdsp_set_options(int echo_cancel, int denoise);
// What the speakers play: mono samples at rate (any), the first of them handed over at play_at.
void vdsp_played(const int16_t *mono, int n, int rate, int64_t play_at);
// How far the played timeline reaches: a microphone block is best cleaned once what played meanwhile is in.
int64_t vdsp_played_until(void);
// Microphone samples (48 kHz), the last of them arriving at now. Each cleaned block goes to out with its speech
// probability (0..1; 1 with the denoiser off).
void vdsp_capture(const int16_t *mic, int n, int64_t now, vdsp_out out);
