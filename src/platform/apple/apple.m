// platform.h on Apple's systems (iOS and iPadOS; macOS): the log (the unified log and stderr, which the app sends to
// port.log), sound out through CoreAudio's I/O units, the microphone (iOS: RemoteIO's input; macOS: AVAudioEngine's
// input node, at the device's own rate), its permission (AVAudioSession on iOS, AVCaptureDevice on macOS). Time,
// threads and files are posix.c's.
#import <AVFoundation/AVFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <os/log.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "../platform.h"

// ---- the log
void plat_vlog(int level, const char *fmt, va_list a) {
    char line[2048];
    vsnprintf(line, sizeof line, fmt, a);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    os_log_with_type(OS_LOG_DEFAULT, level == PLAT_LOG_ERROR ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT, "hotdarcade: %{public}s", line);
    fprintf(stderr, "%s%s\n", level == PLAT_LOG_ERROR ? "error: " : "", line);
}

// ---- I/O units: RemoteIO on iOS (it converts our format and rate to the hardware's), the default output on macOS
static OSType out_subtype(void) {
#if TARGET_OS_OSX
    return kAudioUnitSubType_DefaultOutput;
#else
    return kAudioUnitSubType_RemoteIO;
#endif
}
static AudioUnit new_unit(OSType subtype) {
    AudioComponentDescription d = { kAudioUnitType_Output, subtype, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    AudioUnit u = NULL;
    if (!c || AudioComponentInstanceNew(c, &u) != noErr) return NULL;
    return u;
}
static AudioStreamBasicDescription s16(int rate, int channels) {
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = (UInt32)channels;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = f.mBytesPerPacket = (UInt32)(2 * channels);
    f.mFramesPerPacket = 1;
    return f;
}

// ---- sound out
static AudioUnit out_unit;
static plat_audio_fill out_fill;
static int out_rate;
static atomic_int out_lost;
static OSStatus out_cb(void *ud, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                       AudioBufferList *io) {
    (void)ud; (void)flags; (void)ts; (void)bus;
    out_fill((int16_t *)io->mBuffers[0].mData, (int)frames, out_rate);
    return noErr;
}
int plat_audio_open(int rate, plat_audio_fill fill, int *burst, char *err, int err_len) {
    out_unit = new_unit(out_subtype());
    if (!out_unit) { snprintf(err, (size_t)err_len, "CoreAudio: no output unit"); return 0; }
    out_fill = fill; out_rate = rate; atomic_store(&out_lost, 0);
    AudioStreamBasicDescription f = s16(rate, 2);
    AudioUnitSetProperty(out_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = { out_cb, NULL };
    AudioUnitSetProperty(out_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    OSStatus s = AudioUnitInitialize(out_unit);
    if (s == noErr) s = AudioOutputUnitStart(out_unit);
    if (s != noErr) {
        snprintf(err, (size_t)err_len, "CoreAudio: output error %d", (int)s);
        AudioComponentInstanceDispose(out_unit);
        out_unit = NULL;
        return 0;
    }
    if (burst) *burst = 512;
    plat_log(PLAT_LOG_INFO, "audio: CoreAudio, %d Hz", rate);
    return 1;
}
void plat_audio_close(void) {
    if (!out_unit) return;
    AudioOutputUnitStop(out_unit);
    AudioUnitUninitialize(out_unit);
    AudioComponentInstanceDispose(out_unit);
    out_unit = NULL;
}
int plat_audio_lost(void) { return atomic_load(&out_lost); }

// ---- the microphone (iOS: RemoteIO's input bus; the app's audio session is playAndRecord)
static AudioUnit mic_unit;
static plat_mic_push mic_push_fn;
static int mic_rate;
static atomic_int mic_lost;
static int16_t mic_buf[4096];
static OSStatus mic_cb(void *ud, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                       AudioBufferList *io) {
    (void)ud; (void)io;
    if (frames > sizeof mic_buf / 2) frames = sizeof mic_buf / 2;
    AudioBufferList l;
    l.mNumberBuffers = 1;
    l.mBuffers[0].mNumberChannels = 1;
    l.mBuffers[0].mDataByteSize = frames * 2;
    l.mBuffers[0].mData = mic_buf;
    if (AudioUnitRender(mic_unit, flags, ts, bus, frames, &l) == noErr) mic_push_fn(mic_buf, (int)frames, mic_rate);
    return noErr;
}
int plat_mic_permission(void) {
#if TARGET_OS_OSX
    static atomic_int asked;
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized: return 1;
    case AVAuthorizationStatusDenied: case AVAuthorizationStatusRestricted: return -1;
    default:
        if (!atomic_exchange(&asked, 1)) [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) { (void)granted; }];
        return 0;
    }
#else
    static atomic_int asked;
    switch ([AVAudioSession sharedInstance].recordPermission) {
    case AVAudioSessionRecordPermissionGranted: return 1;
    case AVAudioSessionRecordPermissionDenied: return -1;
    default:
        if (!atomic_exchange(&asked, 1)) [[AVAudioSession sharedInstance] requestRecordPermission:^(BOOL granted) { (void)granted; }];
        return 0;
    }
#endif
}
#if TARGET_OS_OSX
static AVAudioEngine *mac_engine;
static id mac_change;
#endif
int plat_mic_open(int rate, plat_mic_push push) {
#if TARGET_OS_OSX
    (void)rate;   // (the device's own rate: voice.c converts it)
    mic_push_fn = push;
    atomic_store(&mic_lost, 0);
    mac_engine = [[AVAudioEngine alloc] init];
    AVAudioInputNode *in = mac_engine.inputNode;
    AVAudioFormat *hw = [in outputFormatForBus:0];
    if (hw.sampleRate <= 0 || hw.channelCount == 0) {
        plat_log(PLAT_LOG_ERROR, "voice: no microphone (no input device)");
        mac_engine = nil;
        return 0;
    }
    const int dev_rate = (int)hw.sampleRate;
    [in installTapOnBus:0 bufferSize:1024 format:hw block:^(AVAudioPCMBuffer *b, AVAudioTime *when) {
        (void)when;
        const float *c = b.floatChannelData ? b.floatChannelData[0] : NULL;
        if (!c) return;
        int16_t out[1024];
        for (AVAudioFrameCount off = 0; off < b.frameLength;) {
            const AVAudioFrameCount n = b.frameLength - off < 1024 ? b.frameLength - off : 1024;
            for (AVAudioFrameCount i = 0; i < n; i++) {
                const float v = c[off + i] * 32767.0f;
                out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
            }
            mic_push_fn(out, (int)n, dev_rate);
            off += n;
        }
    }];
    // the input device changed (unplugged, another chosen): reopened by the caller
    mac_change = [NSNotificationCenter.defaultCenter addObserverForName:AVAudioEngineConfigurationChangeNotification object:mac_engine
        queue:nil usingBlock:^(NSNotification *n) { (void)n; atomic_store(&mic_lost, 1); }];
    NSError *err = nil;
    if (![mac_engine startAndReturnError:&err]) {
        plat_log(PLAT_LOG_ERROR, "voice: no microphone: %s", err.localizedDescription.UTF8String);
        [in removeTapOnBus:0];
        mac_engine = nil;
        return 0;
    }
    plat_log(PLAT_LOG_INFO, "voice: microphone on, %d Hz (AVAudioEngine)", dev_rate);
    return 1;
#else
    mic_unit = new_unit(kAudioUnitSubType_RemoteIO);
    if (!mic_unit) return 0;
    mic_push_fn = push; mic_rate = rate; atomic_store(&mic_lost, 0);
    const UInt32 on = 1, off = 0;
    AudioUnitSetProperty(mic_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof on);
    AudioUnitSetProperty(mic_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof off);
    AudioStreamBasicDescription f = s16(rate, 1);
    AudioUnitSetProperty(mic_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);
    AURenderCallbackStruct cb = { mic_cb, NULL };
    AudioUnitSetProperty(mic_unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
    OSStatus s = AudioUnitInitialize(mic_unit);
    if (s == noErr) s = AudioOutputUnitStart(mic_unit);
    if (s != noErr) {
        plat_log(PLAT_LOG_ERROR, "voice: no microphone (CoreAudio %d)", (int)s);
        AudioComponentInstanceDispose(mic_unit);
        mic_unit = NULL;
        return 0;
    }
    plat_log(PLAT_LOG_INFO, "voice: microphone on, %d Hz (RemoteIO)", rate);
    return 1;
#endif
}
void plat_mic_close(void) {
#if TARGET_OS_OSX
    if (!mac_engine) return;
    if (mac_change) { [NSNotificationCenter.defaultCenter removeObserver:mac_change]; mac_change = nil; }
    [mac_engine.inputNode removeTapOnBus:0];
    [mac_engine stop];
    mac_engine = nil;
    return;
#endif
    if (!mic_unit) return;
    AudioOutputUnitStop(mic_unit);
    AudioUnitUninitialize(mic_unit);
    AudioComponentInstanceDispose(mic_unit);
    mic_unit = NULL;
}
int plat_mic_failed(void) { return atomic_load(&mic_lost); }
