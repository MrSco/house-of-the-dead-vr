// platform.h's sound out on Android: one AAudio output stream (16-bit stereo, low latency, shared, usage GAME). An
// error (the output device changed: headphones, Link) marks it lost; the game thread then opens another.
#include "../platform.h"
#include <aaudio/AAudio.h>
#include <stdio.h>

static AAudioStream *st;
static plat_audio_fill fill_fn;
static int dev_rate;
static volatile int lost;

static aaudio_data_callback_result_t on_data(AAudioStream *s, void *ud, void *data, int32_t frames) {
    (void)s; (void)ud;
    fill_fn((int16_t *)data, frames, dev_rate);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void on_error(AAudioStream *s, void *ud, aaudio_result_t e) {
    (void)s; (void)ud;
    plat_log(PLAT_LOG_ERROR, "audio: stream error %d (%s), reopening", e, AAudio_convertResultToText(e));
    lost = 1;
}

int plat_audio_open(int rate, plat_audio_fill fill, int *burst, char *err, int err_len) {
    AAudioStreamBuilder *b;
    lost = 0;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { snprintf(err, (size_t)err_len, "AAudio: no stream builder"); return 0; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, on_data, NULL);
    AAudioStreamBuilder_setErrorCallback(b, on_error, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { snprintf(err, (size_t)err_len, "AAudio: %s", AAudio_convertResultToText(r)); st = NULL; return 0; }
    fill_fn = fill;
    dev_rate = AAudioStream_getSampleRate(st);
    if (burst) *burst = AAudioStream_getFramesPerBurst(st);
    r = AAudioStream_requestStart(st);
    plat_log(PLAT_LOG_INFO, "audio: AAudio %d Hz (game mixes at %d Hz), burst %d frames, start %s", dev_rate, rate,
             AAudioStream_getFramesPerBurst(st), AAudio_convertResultToText(r));
    return 1;
}

void plat_audio_close(void) {
    if (!st) return;
    AAudioStream_requestStop(st);
    AAudioStream_close(st);
    st = NULL;
}

int plat_audio_lost(void) { return lost; }
