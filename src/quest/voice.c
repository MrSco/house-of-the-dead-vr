// voice.c -- see voice.h. Ported from the Time Crisis 2 VR project's quest/tc2_voice.c; this app has no Java, so the
// microphone is an AAudio input stream and its permission is asked for through JNI on the NativeActivity.
//
// Send: microphone (AAudio input callback: a queue) -> worker: echo cancellation against what the speakers played and
// noise suppression, 10 ms blocks (voice_dsp.c) -> 20 ms frames -> gate: open mic opens on speech above about
// -42 dBFS and holds 400 ms (stricter while the partner's voice plays); push to talk sends while the key is held and
// 200 ms after -> Opus VOIP 20 kbit/s with in-band FEC -> the send queue, which voice_pack empties.
// Receive: voice_unpack files frames by sequence number into a 64-slot jitter store -> the worker decodes them into a
// 48 kHz playback ring kept about 60 ms ahead, a missing frame rebuilt from the next frame's FEC or by Opus
// concealment -> voice_mix_out (the output callback) reads that ring at the output's rate.
//
// A block (one datagram): version and flags (0x10: talking, 0x20: the first frame starts a talk spurt, so the frames
// before it were not sent, not lost), the sender's stream id (new each time its voice starts, so a partner who started
// over is told apart from late packets), the first frame's sequence number (LE16), then per frame its length byte and
// its data. Usually one new frame, with the frame before it again when that fits: a lost datagram then costs nothing,
// and FEC covers the rest. Frames are encoded to fit the room voice_pack is given.
#include "voice.h"
#include "voice_dsp.h"
#include <opus.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#include <android/native_activity.h>
#include <jni.h>
#endif

#define RATE        48000
#define FRAME       960                   // 20 ms
#define MAX_FRAME   100                   // bytes an encoded frame may take (20 kbit/s averages ~50)
#define SLOTS       64                    // the jitter store, 1.28 s of frames
#define OUTQ        16
#define OUT_MAX_AGE 200                   // ms a frame may wait to be sent: older ones would only play late
#define TARGET      (FRAME * 3)           // playback kept ~60 ms ahead
#define PREBUF      (FRAME * 2)           // a talk spurt starts 40 ms late: room for the network's jitter
#define PLAY_RING   (1 << 15)             // 0.68 s
#define GATE_ON     260                   // RMS of int16 samples: ~ -42 dBFS
#define HANGOVER    20                    // frames the gate stays open after speech (400 ms)
#define PTT_TAIL    10                    // push to talk: frames still sent after the key is let go (200 ms)
#define SPEECH      128                   // RNNoise's speech probability, of 255, that opens the gate
#define PREROLL     2                     // frames from before the open-mic gate opened, sent with it (a word's start)
#define CATCHUP     15                    // frames playback may fall behind what arrived before it skips ahead
#define SPURT_END   250                   // ms without frames: the partner stopped talking
#define HDR         4
#define VERSION     1
#define MS          1000000ll             // in nanoseconds

// ---------------------------------------------------------------- clock, settings, status
#ifdef VOICE_TEST_CLOCK                   // tests (a host program including this file) run on their own clock
static int64_t now_ns(void) { return voice_test_now; }
#else
static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
#endif
static int64_t to48(int64_t ns) { return ns * 6 / 125000; }   // the 48 kHz clock voice_dsp.c works on

static atomic_int inited, want_active, mode = 1, muted, volume = 100, talk_key;
static atomic_int out_on;                 // a session runs: voice_mix_out and voice_unpack take part
static atomic_int budget = 120;           // the most room voice_pack was given lately: frames are encoded to fit it
static atomic_int st_talking, st_mic_denied, st_level, st_partner_level, dsp_on;
static atomic_uint n_sent, n_received, n_lost;
static _Atomic int64_t last_rx_ns, mic_seen_ns;
static int codec_ok;

static int level8(double rms) {           // meters, 0..8: 6 dB a step, 8 from -6 dBFS, 0 under -54 (the gate is ~2)
    if (rms < 1) return 0;
    const int l = (int)((20 * log10(rms / 32768) + 54) / 6);
    return l < 0 ? 0 : l > 8 ? 8 : l;
}

// ---------------------------------------------------------------- the audio callbacks' queues (one writer, one reader)
// Samples with the time they were handed over, from a callback (no locks, no allocation there) to the worker.
#define CHUNK  512
#define CHUNKS 128
typedef struct { int n, rate; int64_t t; int16_t pcm[CHUNK]; } Chunk;   // t: the last sample's arrival / the first's play
typedef struct { Chunk c[CHUNKS]; atomic_uint w, r; } ChunkQ;
static ChunkQ mic_q, ref_q;
static Chunk *cq_begin(ChunkQ *q) {
    const unsigned w = atomic_load_explicit(&q->w, memory_order_relaxed), r = atomic_load_explicit(&q->r, memory_order_acquire);
    return w - r < CHUNKS ? &q->c[w % CHUNKS] : NULL;     // full: the worker fell behind, this block is dropped
}
static void cq_commit(ChunkQ *q) { atomic_store_explicit(&q->w, atomic_load_explicit(&q->w, memory_order_relaxed) + 1, memory_order_release); }
static Chunk *cq_peek(ChunkQ *q) {
    const unsigned r = atomic_load_explicit(&q->r, memory_order_relaxed), w = atomic_load_explicit(&q->w, memory_order_acquire);
    return r != w ? &q->c[r % CHUNKS] : NULL;
}
static void cq_pop(ChunkQ *q) { atomic_store_explicit(&q->r, atomic_load_explicit(&q->r, memory_order_relaxed) + 1, memory_order_release); }
static void cq_clear(ChunkQ *q) { atomic_store_explicit(&q->r, atomic_load_explicit(&q->w, memory_order_acquire), memory_order_release); }

// the microphone's samples (the input callback; tests call it directly)
static void mic_push(const int16_t *pcm, int n, int rate) {
    const int64_t now = now_ns();
    atomic_store(&mic_seen_ns, now);
    for (int off = 0; off < n;) {
        const int k = n - off < CHUNK ? n - off : CHUNK;
        Chunk *c = cq_begin(&mic_q);
        if (!c) break;
        c->n = k; c->rate = rate; c->t = now - (int64_t)(n - off - k) * 1000000000ll / rate;
        memcpy(c->pcm, pcm + off, (size_t)k * 2);
        cq_commit(&mic_q);
        off += k;
    }
}

// The playback ring: the worker writes decoded 48 kHz audio, voice_mix_out reads it. A cut (the worker's write
// position when a session starts, announced by a new cut_gen) tells the reader to drop what an earlier session left.
static int16_t play_buf[PLAY_RING];
static atomic_uint play_w, play_r, play_cut, play_cut_gen;
static uint32_t play_frac, play_gen_seen; // the reader's: its position between two samples (16.16), the cut it took

static inline int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
void voice_mix_out(int16_t *st, int frames, int rate) {
    if (!atomic_load_explicit(&out_on, memory_order_acquire) || !st || frames <= 0 || rate <= 0) return;
    unsigned r = atomic_load_explicit(&play_r, memory_order_relaxed);
    const unsigned gen = atomic_load_explicit(&play_cut_gen, memory_order_acquire);
    if (gen != play_gen_seen) { play_gen_seen = gen; r = atomic_load_explicit(&play_cut, memory_order_relaxed); play_frac = 0; }
    const unsigned w = atomic_load_explicit(&play_w, memory_order_acquire);
    const int vol = atomic_load_explicit(&volume, memory_order_relaxed);
    const uint32_t step = (uint32_t)(((uint64_t)RATE << 16) / (uint32_t)rate);
    for (int i = 0; i < frames && w - r >= 2; i++) {      // the last sample waits for the next (interpolation)
        const int a = play_buf[r & (PLAY_RING - 1)], b = play_buf[(r + 1) & (PLAY_RING - 1)];
        const int v = (a + (int)(((int64_t)(b - a) * play_frac) >> 16)) * vol / 100;
        st[2 * i] = clamp16(st[2 * i] + v);
        st[2 * i + 1] = clamp16(st[2 * i + 1] + v);
        play_frac += step;
        r += play_frac >> 16;
        play_frac &= 0xFFFF;
    }
    atomic_store_explicit(&play_r, r, memory_order_release);
    if (!atomic_load_explicit(&dsp_on, memory_order_relaxed)) return;
    // what the speakers get, game and voice together: the echo canceller's reference
    const int64_t now = now_ns();
    for (int off = 0; off < frames;) {
        const int k = frames - off < CHUNK ? frames - off : CHUNK;
        Chunk *c = cq_begin(&ref_q);
        if (!c) break;
        c->n = k; c->rate = rate; c->t = now + (int64_t)off * 1000000000ll / rate;
        for (int i = 0; i < k; i++) c->pcm[i] = (int16_t)((st[2 * (off + i)] + st[2 * (off + i) + 1]) / 2);
        cq_commit(&ref_q);
        off += k;
    }
}

// ---------------------------------------------------------------- send side
static OpusEncoder *enc;
static OpusDecoder *dec;
typedef struct { uint16_t seq; uint8_t n, start; uint8_t b[MAX_FRAME]; int64_t t; } Frame;   // start: a spurt's first
static pthread_mutex_t tx_mu = PTHREAD_MUTEX_INITIALIZER;
static Frame outq[OUTQ];                  // tx_mu
static unsigned out_r, out_w;
static Frame sent_last;                   // tx_mu: sent again in the next block when it fits
static int have_sent_last;
static int stream_id;                     // tx_mu
static int budget_peak;                   // tx_mu: the largest max of the last half second, since budget_t
static int64_t budget_t;

// worker only
static int active;
static int64_t active_since;
static uint16_t tx_seq;
static int gate_hold, sending;
static Frame preroll[PREROLL];
static int16_t cap[FRAME];
static int cap_n, cap_speech;
static int16_t mic48[CHUNK * 6 + 8];     // a microphone that would not give 48 kHz, converted (down to 8 kHz)
static double mic_phase;
static int mic_last;

static void queue_frame(const Frame *f) {
    pthread_mutex_lock(&tx_mu);
    if (out_w - out_r == OUTQ) out_r++;   // never let a stalled link build up old speech
    outq[out_w++ % OUTQ] = *f;
    pthread_mutex_unlock(&tx_mu);
}
static unsigned play_fill(void) { return atomic_load_explicit(&play_w, memory_order_relaxed) - atomic_load_explicit(&play_r, memory_order_acquire); }
static void encode_frame(const int16_t *pcm, int speech, int64_t now) {
    double sum = 0;
    for (int i = 0; i < FRAME; i++) sum += (double)pcm[i] * pcm[i];
    const double rms = sqrt(sum / FRAME);
    atomic_store(&st_level, level8(rms));
    const int ptt = atomic_load(&mode) == 2;
    if (ptt) { if (atomic_load(&talk_key)) gate_hold = PTT_TAIL; else if (gate_hold > 0) gate_hold--; }
    else {
        // Echo safeguard (speakerphone-style): while the partner's voice plays, the microphone must be louder to open
        // the gate, so what the echo canceller leaves of their voice is not sent back to them: twice as loud once it
        // has learned the room (~20 dB off), four times while it learns (the session's first 10 s) or without it.
        // Measured in the Time Crisis 2 project (the speaker heard at 0.8): 1514 frames of the partner's own voice sent
        // back without the canceller, 131 with it, all in the first 8 s, before this. Gunfire and fans score low as
        // speech. Quest 3's voice-communication input already attenuates its own speakers somewhat.
        const int64_t rx = atomic_load(&last_rx_ns);
        const int settled = atomic_load(&dsp_on) && now - active_since > 10000 * MS;
        const int partner = (rx && now - rx < 300 * MS) || play_fill() > FRAME;
        const double threshold = partner ? GATE_ON * (settled ? 2 : 4) : GATE_ON;
        if (rms > threshold && speech >= SPEECH) gate_hold = HANGOVER;
        else if (gate_hold > 0) gate_hold--;
    }
    const int send = gate_hold > 0 && (ptt || !atomic_load(&muted));
    Frame f;
    const int room = atomic_load(&budget) - HDR - 1;   // (a frame that does not fit would wait and age out)
    const int n = opus_encode(enc, pcm, FRAME, f.b, room < 20 ? 20 : room > MAX_FRAME ? MAX_FRAME : room);   // every frame:
    f.seq = tx_seq++; f.n = (uint8_t)(n > 0 ? n : 0); f.t = now; f.start = 0;   // the encoder's state stays continuous
    int first = send && !sending;
    if (first && !ptt)                    // the gate just opened: the frames before it, which hold the word's start
        for (int i = PREROLL; i >= 1; i--) {
            Frame p = preroll[(uint16_t)(f.seq - i) % PREROLL];
            if (p.n && p.seq == (uint16_t)(f.seq - i)) { p.start = (uint8_t)first; first = 0; queue_frame(&p); }
        }
    if (send && f.n) { f.start = (uint8_t)first; queue_frame(&f); f.start = 0; }
    preroll[f.seq % PREROLL] = f;
    sending = send;
    atomic_store(&st_talking, send);
}
// a cleaned 10 ms block (voice_dsp.c), gathered into 20 ms frames with the block's highest speech probability
static void cleaned(const int16_t *pcm, int n, float speech) {
    const int sp = (int)(speech < 0 ? 0 : speech > 1 ? 255 : speech * 255);
    for (int i = 0; i < n; i++) {
        if (!cap_n) cap_speech = 0;
        cap[cap_n++] = pcm[i];
        if (sp > cap_speech) cap_speech = sp;
        if (cap_n == FRAME) { encode_frame(cap, cap_speech, now_ns()); cap_n = 0; }
    }
}

int voice_pack(uint8_t *out, int max) {
    if (!atomic_load(&inited) || !out || max < HDR + 2) return 0;
    const int64_t now = now_ns();
    int off = 0, count = 0;
    pthread_mutex_lock(&tx_mu);
    if (max >= budget_peak || now - budget_t > 500 * MS) { budget_peak = max; budget_t = now; atomic_store(&budget, max); }
    // too old to be worth playing, or (encoded for a roomier call) too big for this one for 40 ms: dropped
    while (out_r != out_w && (now - outq[out_r % OUTQ].t > OUT_MAX_AGE * MS
                              || (HDR + 1 + outq[out_r % OUTQ].n > max && now - outq[out_r % OUTQ].t > 40 * MS))) out_r++;
    if (out_r != out_w && HDR + 1 + outq[out_r % OUTQ].n <= max) {
        const Frame *head = &outq[out_r % OUTQ];
        uint16_t first = head->seq;
        int spurt = head->start;
        off = HDR;
        if (have_sent_last && sent_last.seq == (uint16_t)(head->seq - 1) && HDR + 2 + sent_last.n + head->n <= max) {
            out[off++] = sent_last.n;
            memcpy(out + off, sent_last.b, sent_last.n);
            off += sent_last.n;
            first = sent_last.seq;
            spurt = sent_last.start;
        }
        for (uint16_t expect = head->seq; out_r != out_w; expect++) {
            const Frame *f = &outq[out_r % OUTQ];
            if (f->seq != expect || off + 1 + f->n > max) break;
            out[off++] = f->n;
            memcpy(out + off, f->b, f->n);
            off += f->n;
            sent_last = *f; have_sent_last = 1;
            out_r++; count++;
        }
        out[0] = (uint8_t)(VERSION | (atomic_load(&st_talking) ? 0x10 : 0) | (spurt ? 0x20 : 0));
        out[1] = (uint8_t)stream_id;
        out[2] = (uint8_t)first; out[3] = (uint8_t)(first >> 8);
    }
    pthread_mutex_unlock(&tx_mu);
    atomic_fetch_add(&n_sent, (unsigned)count);
    return off;
}

// ---------------------------------------------------------------- receive side
typedef struct { int full, start; uint16_t seq; uint8_t n; uint8_t b[MAX_FRAME]; } Slot;
static pthread_mutex_t rx_mu = PTHREAD_MUTEX_INITIALIZER;
// all rx_mu
static Slot slot[SLOTS];
static int rx_stream = -1, rx_restart;
static int playing, spurt_new, decoded_any, have_last, have_newest;
static uint16_t next_seq, last_played, newest;

static void rx_clear(void) {
    memset(slot, 0, sizeof slot);
    playing = spurt_new = decoded_any = have_last = have_newest = 0;
    rx_restart = 1;
}
void voice_unpack(const uint8_t *b, int len) {
    if (!atomic_load(&inited) || !b || len < HDR + 2 || (b[0] & 0x0F) != VERSION) return;
    if (!atomic_load_explicit(&out_on, memory_order_acquire)) return;   // no session here: nothing would play it
    uint16_t seq = (uint16_t)(b[2] | b[3] << 8);
    const int64_t now = now_ns();
    unsigned got = 0;
    pthread_mutex_lock(&rx_mu);
    if (b[1] != rx_stream) { rx_clear(); rx_stream = b[1]; }   // the partner's voice started over
    for (int off = HDR, spurt = b[0] & 0x20; off < len; seq++, spurt = 0) {
        const int n = b[off++];
        if (!n || n > MAX_FRAME || off + n > len) break;
        const uint8_t *data = b + off;
        off += n;
        if (playing && (int16_t)(seq - next_seq) < 0) {
            // played or skipped already; unless the spurt has not started playing and this came ahead of its first
            if (decoded_any || (int16_t)(seq - next_seq) < -3) continue;
            next_seq = seq;
        } else if (!playing && have_last && (int16_t)(seq - last_played) <= 0) {
            if ((int16_t)(seq - last_played) > -50) continue;   // a late repeat
            rx_clear();                   // a second behind what played: they started over under the same id
        }
        Slot *s = &slot[seq % SLOTS];
        if (s->full && s->seq == seq) continue;   // a repeat (the sender's second copy, or the network's)
        s->full = 1; s->start = spurt; s->seq = seq; s->n = (uint8_t)n;
        memcpy(s->b, data, (size_t)n);
        got++;
        if (!have_newest || (int16_t)(seq - newest) > 0) { newest = seq; have_newest = 1; }
        if (!playing) { playing = 1; spurt_new = 1; decoded_any = 0; next_seq = seq; }   // a talk spurt starts
    }
    pthread_mutex_unlock(&rx_mu);
    if (got) { atomic_fetch_add(&n_received, got); atomic_store(&last_rx_ns, now); }
}

static void play_push(const int16_t *pcm, int n, int meter) {
    unsigned w = atomic_load_explicit(&play_w, memory_order_relaxed);
    if (w + (unsigned)n - atomic_load_explicit(&play_r, memory_order_acquire) > PLAY_RING) return;   // the reader stopped
    double sum = 0;
    for (int i = 0; i < n; i++, w++) { play_buf[w & (PLAY_RING - 1)] = pcm[i]; sum += (double)pcm[i] * pcm[i]; }
    atomic_store_explicit(&play_w, w, memory_order_release);
    if (meter && n) atomic_store(&st_partner_level, level8(sqrt(sum / n)));
}
static int have(uint16_t seq) { const Slot *s = &slot[seq % SLOTS]; return s->full && s->seq == seq; }
static void decode_ahead(int64_t now) {
    int16_t pcm[FRAME];
    for (int guard = 0; guard < 8 && play_fill() < TARGET; guard++) {
        uint8_t b[MAX_FRAME];
        int n = 0, how = 0, reset = 0, prebuf = 0;   // how: 0 nothing, 1 the frame, 2 the next frame's FEC, 3 concealment
        pthread_mutex_lock(&rx_mu);
        if (rx_restart) { rx_restart = 0; reset = 1; }
        if (playing) {
            if (have_newest && (int16_t)(newest - next_seq) > CATCHUP) {   // fell behind (the output stopped a while)
                next_seq = (uint16_t)(newest - 2);
                reset = 1;
            }
            if (spurt_new) { spurt_new = 0; prebuf = 1; }
            Slot *s = &slot[next_seq % SLOTS];
            if (have(next_seq)) { n = s->n; memcpy(b, s->b, (size_t)n); s->full = 0; how = 1; }
            else {
                int later = 0;
                for (int d = 1; d <= 6 && !later; d++) if (have((uint16_t)(next_seq + d))) later = d;
                if (later && slot[(uint16_t)(next_seq + later) % SLOTS].start) {   // a new spurt: those were never sent
                    next_seq = (uint16_t)(next_seq + later);
                    s = &slot[next_seq % SLOTS];
                    n = s->n; memcpy(b, s->b, (size_t)n); s->full = 0; how = 1;
                } else if (!later) {
                    if (now - atomic_load(&last_rx_ns) > SPURT_END * MS) { playing = 0; reset = 1; }   // the spurt ended
                } else if (play_fill() <= FRAME) {   // (with more queued, a late frame may still come: wait)
                    if (later == 1) { const Slot *t = &slot[(uint16_t)(next_seq + 1) % SLOTS]; n = t->n; memcpy(b, t->b, (size_t)n); how = 2; }
                    else how = 3;
                }
            }
            if (how) { last_played = next_seq++; have_last = 1; decoded_any = 1; }
        }
        pthread_mutex_unlock(&rx_mu);
        if (reset) opus_decoder_ctl(dec, OPUS_RESET_STATE);
        if (prebuf) { static const int16_t quiet[PREBUF]; play_push(quiet, PREBUF, 0); }
        if (!how) break;
        const int got = how == 1 ? opus_decode(dec, b, n, pcm, FRAME, 0) : how == 2 ? opus_decode(dec, b, n, pcm, FRAME, 1)
                                                                       : opus_decode(dec, NULL, 0, pcm, FRAME, 0);
        if (how > 1) atomic_fetch_add(&n_lost, 1);
        if (got > 0) play_push(pcm, got, how == 1);
    }
}

// ---------------------------------------------------------------- the microphone (Android: AAudio, JNI permission)
#ifdef __ANDROID__
#define PERMISSION "android.permission.RECORD_AUDIO"
static ANativeActivity *activity;
static JNIEnv *jenv;                      // the worker's own, attached once
static AAudioStream *mic;
static atomic_int mic_failed;
static int mic_rate, granted, asked;
static int64_t perm_checked, mic_retry;

// Context.checkSelfPermission / Activity.requestPermissions on the NativeActivity object. NativeActivity ignores the
// answer (onRequestPermissionsResult), so the worker asks again with checkSelfPermission once a second.
#define J(x) x; if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); goto done; }
static int perm_granted(void) {
    JNIEnv *e = jenv;
    int ok = 0;
    if (!e || !activity || (*e)->PushLocalFrame(e, 8)) return 0;
    J(jclass cls = (*e)->GetObjectClass(e, activity->clazz))
    J(jmethodID check = (*e)->GetMethodID(e, cls, "checkSelfPermission", "(Ljava/lang/String;)I"))
    J(jstring p = (*e)->NewStringUTF(e, PERMISSION))
    J(ok = (*e)->CallIntMethod(e, activity->clazz, check, p) == 0)   // PackageManager.PERMISSION_GRANTED
done:
    (*e)->PopLocalFrame(e, NULL);
    return ok;
}
static void perm_request(void) {
    JNIEnv *e = jenv;
    if (!e || !activity || (*e)->PushLocalFrame(e, 8)) return;
    J(jclass cls = (*e)->GetObjectClass(e, activity->clazz))
    J(jmethodID req = (*e)->GetMethodID(e, cls, "requestPermissions", "([Ljava/lang/String;I)V"))
    J(jclass str = (*e)->FindClass(e, "java/lang/String"))
    J(jstring p = (*e)->NewStringUTF(e, PERMISSION))
    J(jobjectArray list = (*e)->NewObjectArray(e, 1, str, p))
    J((*e)->CallVoidMethod(e, activity->clazz, req, list, (jint)0x766f))
    fprintf(stderr, "voice: asking for the microphone\n");
done:
    (*e)->PopLocalFrame(e, NULL);
}
#undef J

static aaudio_data_callback_result_t mic_data(AAudioStream *s, void *ud, void *data, int32_t frames) {
    (void)s; (void)ud;
    mic_push((const int16_t *)data, frames, mic_rate);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void mic_error(AAudioStream *s, void *ud, aaudio_result_t e) {
    (void)s; (void)ud;
    fprintf(stderr, "voice: microphone error %d (%s)\n", e, AAudio_convertResultToText(e));
    atomic_store(&mic_failed, 1);         // reopened by the worker (not from this callback)
}
static void mic_close(void) {
    if (!mic) return;
    AAudioStream_requestStop(mic);
    AAudioStream_close(mic);
    mic = NULL;
    fprintf(stderr, "voice: microphone off\n");
}
static int mic_open(void) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return 0;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, RATE);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    // voice communication: the platform's own capture path for calls (its echo and noise handling for the
    // headset's speakers). Not the low-latency mode: that may take the MMAP path, which skips those effects.
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDataCallback(b, mic_data, NULL);
    AAudioStreamBuilder_setErrorCallback(b, mic_error, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &mic);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { fprintf(stderr, "voice: no microphone: %s\n", AAudio_convertResultToText(r)); mic = NULL; return 0; }
    mic_rate = AAudioStream_getSampleRate(mic);
    atomic_store(&mic_failed, 0);
    r = AAudioStream_requestStart(mic);
    fprintf(stderr, "voice: microphone on, %d Hz, burst %d frames, start %s\n", mic_rate, AAudioStream_getFramesPerBurst(mic),
            AAudio_convertResultToText(r));
    if (r != AAUDIO_OK) { mic_close(); return 0; }
    return 1;
}
// while a session runs: the permission (asked once per run), then the stream, reopened after an error
static void mic_service(int64_t now) {
    if (!active) { mic_close(); return; }
    if (!granted && now - perm_checked >= 1000 * MS) {
        perm_checked = now;
        granted = perm_granted();
        if (!granted && !asked) { asked = 1; perm_request(); }
        if (granted) fprintf(stderr, "voice: microphone permission granted\n");
        atomic_store(&st_mic_denied, asked && !granted);
    }
    if (!granted) return;
    if (atomic_load(&mic_failed)) { mic_close(); atomic_store(&mic_failed, 0); mic_retry = now + 1000 * MS; }
    if (!mic && now >= mic_retry && !mic_open()) mic_retry = now + 3000 * MS;
}
#else
static void mic_service(int64_t now) { (void)now; }   // host builds (tests): samples come through mic_push
#endif

// ---------------------------------------------------------------- the worker
static int wanted(void) { return codec_ok && atomic_load(&want_active) && atomic_load(&mode) != 0; }
static void start(int64_t now) {
    active = 1;
    active_since = now;
    opus_encoder_ctl(enc, OPUS_RESET_STATE);
    opus_decoder_ctl(dec, OPUS_RESET_STATE);
    vdsp_reset();
    gate_hold = sending = cap_n = 0;
    mic_phase = 0; mic_last = 0;
    memset(preroll, 0, sizeof preroll);
    pthread_mutex_lock(&tx_mu);
    out_r = out_w;
    have_sent_last = 0;
    stream_id = (stream_id + 1 + (int)(now % 251)) & 0xFF;
    pthread_mutex_unlock(&tx_mu);
    pthread_mutex_lock(&rx_mu);
    rx_clear();
    rx_stream = -1;
    pthread_mutex_unlock(&rx_mu);
    atomic_store(&last_rx_ns, 0);
    cq_clear(&mic_q);
    cq_clear(&ref_q);
    atomic_store_explicit(&play_cut, atomic_load(&play_w), memory_order_relaxed);
    atomic_fetch_add_explicit(&play_cut_gen, 1, memory_order_release);
    atomic_store_explicit(&out_on, 1, memory_order_release);
    fprintf(stderr, "voice: on\n");
}
static void stop(void) {
    active = 0;
    atomic_store_explicit(&out_on, 0, memory_order_release);
    atomic_store(&st_talking, 0);
    atomic_store(&st_level, 0);
    atomic_store(&st_partner_level, 0);
    fprintf(stderr, "voice: off\n");
}

static int64_t busy_ns, busy_since;
// one round: the session's state, the reference, the microphone, the partner's voice
static void service(void) {
    const int64_t t0 = now_ns();
    const int want = wanted();
    if (want != active) { if (want) start(t0); else stop(); }
    mic_service(t0);
    if (!active) return;
    const int dsp = atomic_load(&dsp_on);
    for (Chunk *c; (c = cq_peek(&ref_q)); cq_pop(&ref_q))
        if (dsp) vdsp_played(c->pcm, c->n, c->rate, to48(c->t));
    for (Chunk *c; (c = cq_peek(&mic_q)); cq_pop(&mic_q)) {
        const int64_t at = to48(c->t);
        // what the speakers played meanwhile is not in yet: wait for it (40 ms at most: the game's sound may be off)
        if (dsp && at > vdsp_played_until() && to48(t0) - at < RATE / 25) break;
        const int16_t *pcm = c->pcm;
        int n = c->n;
        if (c->rate != RATE && c->rate > 0) {   // a microphone that would not give 48 kHz: linear, as the reference
            const double step = (double)c->rate / RATE;
            int k = 0;
            for (double p = mic_phase; p < c->n && k < (int)(sizeof mic48 / 2); p += step, k++) {
                const int i = (int)p;
                const int a = i == 0 ? mic_last : c->pcm[i - 1], b = c->pcm[i];
                mic48[k] = (int16_t)(a + (b - a) * (p - i));
            }
            mic_phase += k * step - c->n;
            mic_last = c->pcm[c->n - 1];
            pcm = mic48; n = k;
        }
        if (dsp) vdsp_capture(pcm, n, at, cleaned);
        else cleaned(pcm, n, 1);
    }
    decode_ahead(t0);
    // the worker's cost, in the log once a minute while voice runs
    const int64_t t1 = now_ns();
    busy_ns += t1 - t0;
    if (!busy_since) busy_since = t1;
    if (t1 - busy_since >= 60000 * MS) {
        fprintf(stderr, "voice: worker %.2f%% of a core; sent %u, received %u, lost %u frames\n", 100.0 * busy_ns / (t1 - busy_since),
                atomic_load(&n_sent), atomic_load(&n_received), atomic_load(&n_lost));
        busy_ns = 0; busy_since = t1;
    }
}

static pthread_mutex_t wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake_cv = PTHREAD_COND_INITIALIZER;
static void wake(void) { pthread_mutex_lock(&wake_mu); pthread_cond_signal(&wake_cv); pthread_mutex_unlock(&wake_mu); }
static void *worker(void *arg) {
    (void)arg;
#ifdef __ANDROID__
    if (activity && activity->vm && (*activity->vm)->AttachCurrentThread(activity->vm, (void *)&jenv, NULL) != JNI_OK) jenv = NULL;
#endif
    for (;;) {
        service();
        if (active) {                     // 5 ms rounds: 10 ms microphone blocks, 20 ms frames, a 60 ms playback lead
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);
        } else {                          // asleep until a session starts (and once a second regardless)
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_mutex_lock(&wake_mu);
            if (!wanted()) pthread_cond_timedwait(&wake_cv, &wake_mu, &ts);
            pthread_mutex_unlock(&wake_mu);
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- the interface
void voice_init(void *native_activity) {
    static atomic_int once;
    if (atomic_exchange(&once, 1)) return;
    int err = 0;
    enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (enc) {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(20000));
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }
    dec = opus_decoder_create(RATE, 1, &err);
    codec_ok = enc && dec;
    // QUEST_VOICE_DSP (env.txt): 0 the microphone as it comes, 1 echo cancellation only, 2 noise suppression only;
    // both by default
    const char *d = getenv("QUEST_VOICE_DSP");
    const int stages = d ? atoi(d) & 3 : 3;
    if (stages) vdsp_init();
    vdsp_set_options(stages & 1, stages & 2);
    atomic_store(&dsp_on, stages != 0);
    const int64_t now = now_ns();
    tx_seq = (uint16_t)(now / 1000);
    stream_id = (int)((now / 1000000) & 0xFF);
#ifdef __ANDROID__
    activity = native_activity;
#else
    (void)native_activity;
#endif
    fprintf(stderr, "voice: %s (Opus %s; echo cancellation %s, noise suppression %s)\n", codec_ok ? "ready" : "codec unavailable",
            opus_get_version_string(), stages & 1 ? "on" : "off", stages & 2 ? "on" : "off");
    atomic_store(&inited, 1);
#ifndef VOICE_TEST_MANUAL                 // (tests run the worker's rounds themselves)
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, worker, NULL)) fprintf(stderr, "voice: no worker thread\n");
    pthread_attr_destroy(&at);
#endif
}
void voice_set_active(int linked) { atomic_store(&want_active, linked != 0); wake(); }
void voice_set_mode(int m) { atomic_store(&mode, m < 0 ? 0 : m > 2 ? 2 : m); wake(); }
void voice_set_muted(int m) { atomic_store(&muted, m != 0); }
void voice_set_volume(int percent) { atomic_store(&volume, percent < 0 ? 0 : percent > 200 ? 200 : percent); }
void voice_talk_key(int held) { atomic_store(&talk_key, held != 0); }

void voice_get_status(voice_status_t *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    const int64_t now = now_ns(), seen = atomic_load(&mic_seen_ns), rx = atomic_load(&last_rx_ns);
    s->active = atomic_load(&out_on);
    s->capturing = s->active && seen && now - seen < 500 * MS;
    s->talking = s->active && atomic_load(&st_talking);
    s->partner_talking = s->active && rx && now - rx < 300 * MS;
    s->mic_denied = atomic_load(&st_mic_denied);
    s->mode = atomic_load(&mode);
    s->muted = atomic_load(&muted);
    s->level = s->capturing ? atomic_load(&st_level) : 0;
    s->partner_level = s->partner_talking ? atomic_load(&st_partner_level) : 0;
    s->sent = atomic_load(&n_sent);
    s->received = atomic_load(&n_received);
    s->lost = atomic_load(&n_lost);
}
