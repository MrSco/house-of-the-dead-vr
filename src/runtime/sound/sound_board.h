// A sound board as the game loop and the frontends see it, whichever board
// the game has (Game::sound): the Model 1 board (snd::SoundBoard) or the
// SCSP board (snd::ScspBoard). The board receives the i960's UART bytes,
// runs a video frame at a time, and hands back its audio as two stereo
// streams at their own rates (the Model 1 board's YM3438 and MultiPCMs; the
// SCSP board's output on the second, the first silent), plus the bytes it
// sends back to the i960's UART.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rt { class State; }

namespace snd {

class Board {
public:
    virtual ~Board() = default;
    // Command bytes the i960 wrote to its UART; they arrive one line-time
    // apart from now on.
    virtual void send(const uint8_t *bytes, size_t n) = 0;
    // Run the board for `seconds` of board time.
    virtual void advance(double seconds) = 0;
    // Rendered audio since the last take: interleaved stereo floats.
    virtual double fm_rate() const = 0;
    virtual double pcm_rate() const = 0;
    virtual std::vector<float> take_fm() = 0;
    virtual std::vector<float> take_pcm() = 0;
    // Bytes the board sent back on the serial line since the last take (the
    // SCSP's MIDI output; none on the Model 1 board).
    virtual std::vector<uint8_t> take_replies() { return {}; }
    virtual uint64_t instructions() const = 0;   // 68000 instructions so far
    virtual size_t bytes_received() const = 0;
    // The launcher's music and effects volumes (0..1; both 1: unchanged).
    virtual void set_volumes(float, float) {}
    // This player's ear only (never the game's state, so online play stays the same): the sample ROM's bytes
    // [from, to) read as silence from now on (from == to: none), and those bytes as they are. Boards without a
    // sample ROM the program reads do nothing.
    virtual void mute_samples(uint32_t, uint32_t) {}
    virtual std::vector<uint8_t> sample_bytes(uint32_t, uint32_t) const { return {}; }
    // Save states (between frames); a board without them throws.
    virtual void state(rt::State &s);
};

// The two output rates of the board the game this build is for has (the
// frontends open their audio streams before the board exists).
double board_fm_rate();
double board_pcm_rate();

} // namespace snd
