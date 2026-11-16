// Voice chat between linked players (ported from the Time Crisis 2 VR project's quest/tc2_voice.c): 48 kHz mono,
// Opus in 20 ms frames (~20 kbit/s, in-band FEC), the microphone cleaned up first (SpeexDSP's echo canceller against
// what the speakers play, RNNoise noise suppression), sent while a voice-activity gate or the push-to-talk key is
// open, and mixed into the game's own audio output (host_sdl.c au_fill: the app has one output stream).
//
// Threads: the shell's thread sets things up and reads the status, the network thread moves the encoded blocks, the
// two audio callbacks (speakers, microphone: platform.h) only copy samples. The heavy work (clean-up, encoding, decoding) runs on a
// worker thread of voice.c's own while voice is active.
#pragma once
#include <stdint.h>

void voice_init(void);                    // once at startup (the shell's thread); inert until active
void voice_set_active(int linked);        // 1 while linked to a partner: capture and playback run only then
void voice_set_mode(int mode);            // 0 off (neither direction), 1 open mic (default), 2 push to talk
void voice_set_muted(int muted);          // open mic sends nothing (push to talk ignores it: holding the key is asking)
void voice_set_volume(int percent);       // the partner's voice, 0..200
void voice_talk_key(int held);            // every VR frame: the push-to-talk key (a thumbstick click held)

// Network side (the network/game thread, ~60-120 times a second).
// The next voice block to send, at most max bytes (0: nothing now). Each block is whole and goes in one datagram.
int  voice_pack(uint8_t *out, int max);
// A block the partner's voice_pack produced. Blocks may be lost, duplicated or reordered: each carries the sequence
// numbers of its frames, old and repeated frames are dropped, gaps are filled from Opus FEC or its concealment.
void voice_unpack(const uint8_t *block, int len);

typedef struct {
    int active, capturing, talking, partner_talking, mic_denied, mode, muted, level, partner_level;   // levels 0..8
    uint32_t sent, received, lost;                                                                   // Opus frames
} voice_status_t;
void voice_get_status(voice_status_t *s);

// Audio side (host_sdl.c au_fill, the output callback): adds the partner's voice to what is about to play (16-bit
// stereo at rate), then keeps the result as the echo canceller's reference. Lock-free, no allocation.
void voice_mix_out(int16_t *stereo, int frames, int rate);
