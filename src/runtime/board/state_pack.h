// A save state packed to travel (online play's drop-in, src/net/online.c): most of a state's bytes are copies of
// the ROMs both players have (texture and character RAM from the data ROM, sound RAM from the samples), so runs
// found in them go as references (dictionary, offset, length); those and the rest are then LZMA-compressed. The
// House of the Dead's 8.7 MB state packs to a few hundred KB.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rt {

// A dictionary: ROM bytes both sides have, the same, in the same order.
struct PackDict {
    const uint8_t *data;
    size_t size;
};

std::vector<uint8_t> pack_state(const std::vector<uint8_t> &state, const std::vector<PackDict> &dicts);
// The state again; State::Bad when the bytes aren't a packed state or don't match these dictionaries.
std::vector<uint8_t> unpack_state(const uint8_t *packed, size_t n, const std::vector<PackDict> &dicts);

} // namespace rt
