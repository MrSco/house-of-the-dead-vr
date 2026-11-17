// platform.h's sound out and microphone on Windows: WASAPI in shared mode, each stream on a thread of its own woken
// by the engine's event. Windows converts our format (16-bit, the rate we ask for) to the device's
// (AUTOCONVERTPCM), so fill() and push() get exactly the rate asked for. A device that goes away (unplugged, the
// default changed under an exclusive app) marks the stream lost; the caller opens another on the new default.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>
#include "../platform.h"

static const GUID kClsidEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIidEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIidAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID kIidRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID kIidCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

typedef struct {
    int capture;                    // 0 sound out, 1 the microphone
    int rate, channels;
    plat_audio_fill fill;
    plat_mic_push push;
    HANDLE thread, ready, wake;
    volatile LONG stop, lost, ok;
    UINT32 frames;                  // the engine's buffer
    char err[160];
} Stream;

static void fail(Stream *s, const char *what, HRESULT hr) {
    snprintf(s->err, sizeof s->err, "WASAPI %s: 0x%08lx", what, (unsigned long)hr);
}

static DWORD WINAPI stream_main(void *p) {
    Stream *s = p;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    IAudioCaptureClient *cc = NULL;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const int com = SUCCEEDED(hr);
    hr = CoCreateInstance(&kClsidEnumerator, NULL, CLSCTX_ALL, &kIidEnumerator, (void **)&en);
    if (FAILED(hr)) { fail(s, "no device enumerator", hr); goto out; }
    // sound: the default console device; the microphone: the default communications device (a headset's)
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, s->capture ? eCapture : eRender, s->capture ? eCommunications : eConsole, &dev);
    if (FAILED(hr)) { fail(s, s->capture ? "no microphone" : "no output device", hr); goto out; }
    hr = IMMDevice_Activate(dev, &kIidAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { fail(s, "activate", hr); goto out; }
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)s->channels;
    wf.nSamplesPerSec = (DWORD)s->rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * 2);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    // 10 ms of buffer (the shared engine's period), event driven
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 s->capture ? 200000 : 100000, 0, &wf, NULL);
    if (FAILED(hr)) { fail(s, "initialize", hr); goto out; }
    IAudioClient_SetEventHandle(ac, s->wake);
    IAudioClient_GetBufferSize(ac, &s->frames);
    hr = s->capture ? IAudioClient_GetService(ac, &kIidCaptureClient, (void **)&cc) : IAudioClient_GetService(ac, &kIidRenderClient, (void **)&rc);
    if (FAILED(hr)) { fail(s, "service", hr); goto out; }
    if (rc) {   // silence first, so the stream starts without a click
        BYTE *buf;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, s->frames, &buf))) IAudioRenderClient_ReleaseBuffer(rc, s->frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    hr = IAudioClient_Start(ac);
    if (FAILED(hr)) { fail(s, "start", hr); goto out; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    InterlockedExchange(&s->ok, 1);
    SetEvent(s->ready);
    while (!s->stop) {
        WaitForSingleObject(s->wake, 200);
        if (s->stop) break;
        if (rc) {
            UINT32 pad = 0;
            hr = IAudioClient_GetCurrentPadding(ac, &pad);
            if (FAILED(hr)) break;
            const UINT32 n = s->frames - pad;
            if (!n) continue;
            BYTE *buf;
            hr = IAudioRenderClient_GetBuffer(rc, n, &buf);
            if (FAILED(hr)) break;
            s->fill((int16_t *)buf, (int)n, s->rate);
            hr = IAudioRenderClient_ReleaseBuffer(rc, n, 0);
            if (FAILED(hr)) break;
        } else {
            for (;;) {
                UINT32 n = 0;
                hr = IAudioCaptureClient_GetNextPacketSize(cc, &n);
                if (FAILED(hr) || !n) break;
                BYTE *buf;
                DWORD flags;
                hr = IAudioCaptureClient_GetBuffer(cc, &buf, &n, &flags, NULL, NULL);
                if (FAILED(hr)) break;
                static int16_t zero[4800];
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    for (UINT32 k = 0; k < n; k += 4800) s->push(zero, (int)(n - k < 4800 ? n - k : 4800), s->rate);
                } else {
                    s->push((const int16_t *)buf, (int)n, s->rate);
                }
                IAudioCaptureClient_ReleaseBuffer(cc, n);
            }
            if (FAILED(hr)) break;
        }
    }
    if (!s->stop) {   // the device went away
        plat_log(PLAT_LOG_ERROR, "%s: stream error 0x%08lx, reopening", s->capture ? "voice: microphone" : "audio", (unsigned long)hr);
        InterlockedExchange(&s->lost, 1);
    }
    IAudioClient_Stop(ac);
out:
    if (rc) IAudioRenderClient_Release(rc);
    if (cc) IAudioCaptureClient_Release(cc);
    if (ac) IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (com) CoUninitialize();
    SetEvent(s->ready);
    return 0;
}

static int stream_open(Stream *s) {
    s->stop = s->lost = s->ok = 0;
    s->err[0] = 0;
    s->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->thread = CreateThread(NULL, 0, stream_main, s, 0, NULL);
    if (!s->thread) { snprintf(s->err, sizeof s->err, "no audio thread"); return 0; }
    WaitForSingleObject(s->ready, 5000);
    if (!s->ok) {
        InterlockedExchange(&s->stop, 1);
        SetEvent(s->wake);
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread); CloseHandle(s->ready); CloseHandle(s->wake);
        s->thread = NULL;
        return 0;
    }
    return 1;
}
static void stream_close(Stream *s) {
    if (!s->thread) return;
    InterlockedExchange(&s->stop, 1);
    SetEvent(s->wake);
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread); CloseHandle(s->ready); CloseHandle(s->wake);
    s->thread = NULL;
}

// ---- sound out
static Stream out = { .capture = 0, .channels = 2 };
int plat_audio_open(int rate, plat_audio_fill fill, int *burst, char *err, int err_len) {
    out.rate = rate;
    out.fill = fill;
    if (!stream_open(&out)) { snprintf(err, (size_t)err_len, "%s", out.err); return 0; }
    if (burst) *burst = (int)out.frames;
    plat_log(PLAT_LOG_INFO, "audio: WASAPI shared, %d Hz, buffer %u frames", rate, out.frames);
    return 1;
}
void plat_audio_close(void) { stream_close(&out); }
int plat_audio_lost(void) { return out.lost != 0; }

// ---- the microphone (Windows asks the player itself, in its privacy settings: a refusal shows as no stream)
static Stream mic = { .capture = 1, .channels = 1 };
int plat_mic_permission(void) { return 1; }
int plat_mic_open(int rate, plat_mic_push push) {
    mic.rate = rate;
    mic.push = push;
    if (!stream_open(&mic)) { plat_log(PLAT_LOG_ERROR, "voice: no microphone: %s", mic.err); return 0; }
    plat_log(PLAT_LOG_INFO, "voice: microphone on, %d Hz (WASAPI, communications device)", rate);
    return 1;
}
void plat_mic_close(void) { stream_close(&mic); }
int plat_mic_failed(void) { return mic.lost != 0; }
