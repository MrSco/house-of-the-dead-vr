// voice_dsp.c -- see voice_dsp.h.
//
// Measured in the Time Crisis 2 project on synthetic speech (a partner's voice played back 60-150 ms late through a
// small room, 250 ms filter): the echo falls by 13-17 dB within the first seconds and by about 30 dB once the filter
// has learned the room; near speech keeps its level (0.95) during double talk; fan noise and gunshot bursts score as
// speech in under 1% of blocks. About 0.15 ms per block on a desktop core. SpeexDSP's residual-echo stage changed
// nothing measurable with its own denoiser off, so it is not used.
#include "voice_dsp.h"
#include <rnnoise.h>
#include <speex/speex_echo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define REF_RING (1 << 17)               // 2.7 s: the output's lead, the tail and the microphone's lag behind
#define RESYNC   (VDSP_RATE / 40)        // an estimate 25 ms off the running position: a gap or a stall, start again

static SpeexEchoState *echo;
static DenoiseState *noise;
static int use_echo = 1, use_noise = 1;

// what was played, by its time on the shared clock; a slot whose tag is not its time is silence
static int16_t ref[REF_RING];
static int64_t tag[REF_RING];

// A running position on the shared clock. Blocks follow each other exactly; the times measured, averaged, only steer
// that position, so their jitter (the callbacks come in bursts) does not shake the echo path the filter has learned:
// a lasting offset over `slack` (the two clocks drift) is taken up a little at a time, one over 25 ms at once (a
// gap: the output stopped while the game was paused, or a stall).
typedef struct { int64_t next; double err; int set; } Track;
static Track out_track, mic_track;
static int64_t track(Track *t, int64_t est, int n, int slack, double rate) {
    if (!t->set || llabs(est - t->next) > RESYNC) { t->next = est; t->err = 0; t->set = 1; }
    else {
        t->err = t->err * (1 - rate) + (double)(est - t->next) * rate;
        if (fabs(t->err) > slack) { const int64_t step = (int64_t)(t->err / 8); t->next += step; t->err -= (double)step; }
    }
    const int64_t at = t->next;
    t->next += n;
    return at;
}

// any rate -> 48 kHz: linear between the samples (only the filter's reference; the player hears the original)
static double phase;
static int last_in;
void vdsp_played(const int16_t *s, int n, int rate, int64_t play_at) {
    if (n <= 0 || rate <= 0) return;
    if (rate == VDSP_RATE) {
        int64_t at = track(&out_track, play_at, n, VDSP_RATE * 6 / 1000, .01);
        for (int i = 0; i < n; i++, at++) { ref[at & (REF_RING - 1)] = s[i]; tag[at & (REF_RING - 1)] = at; }
        last_in = s[n - 1];
        return;
    }
    const double step = (double)rate / VDSP_RATE;
    int count = 0;
    for (double p = phase; p < n; p += step) count++;
    int64_t at = track(&out_track, play_at, count, VDSP_RATE * 6 / 1000, .01);
    for (double p = phase; p < n; p += step, at++) {
        const int i = (int)p;
        const double f = p - i;
        const int a = i == 0 ? last_in : s[i - 1], b = s[i];
        ref[at & (REF_RING - 1)] = (int16_t)(a + (b - a) * f);
        tag[at & (REF_RING - 1)] = at;
    }
    phase += count * step - n;
    last_in = s[n - 1];
}
int64_t vdsp_played_until(void) { return out_track.set ? out_track.next : INT64_MIN; }
static void played_block(int64_t at, int16_t *out, int n) {
    for (int i = 0; i < n; i++) { const int64_t t = at + i; out[i] = tag[t & (REF_RING - 1)] == t ? ref[t & (REF_RING - 1)] : 0; }
}

static int16_t pend[VDSP_BLOCK];
static int pend_n;
static int64_t pend_at;
static void process(vdsp_out out) {
    int16_t play[VDSP_BLOCK], clean[VDSP_BLOCK];
    played_block(pend_at, play, VDSP_BLOCK);
    if (use_echo && echo) speex_echo_cancellation(echo, pend, play, clean);
    else memcpy(clean, pend, sizeof clean);
    float speech = 1;
    if (use_noise && noise) {
        float in[VDSP_BLOCK], o[VDSP_BLOCK];
        for (int i = 0; i < VDSP_BLOCK; i++) in[i] = clean[i];
        speech = rnnoise_process_frame(noise, o, in);
        for (int i = 0; i < VDSP_BLOCK; i++) { const float v = o[i]; clean[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v)); }
    }
    out(clean, VDSP_BLOCK, speech);
}
void vdsp_capture(const int16_t *mic, int n, int64_t now, vdsp_out out) {
    if (n <= 0) return;
    int64_t at = track(&mic_track, now - n, n, VDSP_RATE * 2 / 1000, .05);
    for (int i = 0; i < n; i++, at++) {
        if (!pend_n) pend_at = at;
        pend[pend_n++] = mic[i];
        if (pend_n == VDSP_BLOCK) { process(out); pend_n = 0; }
    }
}

int vdsp_init(void) {
    if (!echo) {
        echo = speex_echo_state_init(VDSP_BLOCK, VDSP_TAIL_MS * VDSP_RATE / 1000);
        if (echo) { int rate = VDSP_RATE; speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate); }
    }
    if (!noise) noise = rnnoise_create(NULL);
    return echo && noise;
}
void vdsp_reset(void) {
    if (echo) speex_echo_state_reset(echo);
    if (noise) rnnoise_init(noise, NULL);
    out_track.set = mic_track.set = 0;
    pend_n = 0;
    phase = 0;
    last_in = 0;
}
void vdsp_set_options(int echo_cancel, int denoise) { use_echo = echo_cancel; use_noise = denoise; }
