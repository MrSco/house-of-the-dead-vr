// state_pack.h. The references are found as an LZ77 would against an outside dictionary: every 16th position of
// the dictionaries indexed by a hash of the 32 bytes there, each position of the state looked up by the same hash
// of its next 32 (rolled along), a hit checked and grown both ways. Any run of 47 bytes or more in a dictionary has
// an indexed position inside it, so is found. Windows of one byte value (zero fill) aren't indexed: LZMA takes those.
//
// Packed: "M2PK", a format byte (1: LZMA), the state's size and the tokens' size (u32 each), LZMA's 5 property
// bytes, the compressed tokens. Tokens, until the state is whole: literal count (varint), the literals, match
// length (varint; 0: none), and for a match its dictionary (byte) and offset (varint).
#include "runtime/board/state_pack.h"

#include "runtime/board/state.h"

#include <cstring>
#include <string>

#ifdef M2_HAVE_7Z
extern "C" {
#include "Alloc.h"
#include "LzmaDec.h"
#include "LzmaEnc.h"
}
#endif

namespace rt {

namespace {
constexpr unsigned kWin = 32, kStride = 16, kBits = 22;
constexpr uint32_t kPrime = 0x01000193u;
constexpr size_t kMinMatch = kWin;

uint32_t hash_at(const uint8_t *p) {
    uint32_t h = 0;
    for (unsigned i = 0; i < kWin; i++) h = h * kPrime + p[i];
    return h;
}
uint32_t slot(uint32_t h) { return (h * 0x9E3779B1u) >> (32 - kBits); }
bool uniform(const uint8_t *p) { return std::memcmp(p, p + 1, kWin - 1) == 0; }

void put_varint(std::vector<uint8_t> &o, uint64_t v) {
    while (v >= 0x80) o.push_back(uint8_t(v | 0x80)), v >>= 7;
    o.push_back(uint8_t(v));
}
uint64_t get_varint(const uint8_t *&p, const uint8_t *end) {
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (p >= end) throw State::Bad("packed state: cut short");
        const uint8_t b = *p++;
        v |= uint64_t(b & 0x7f) << shift;
        if (!(b & 0x80)) return v;
    }
    throw State::Bad("packed state: a bad number");
}
void put32(std::vector<uint8_t> &o, uint32_t v) {
    for (int i = 0; i < 4; i++) o.push_back(uint8_t(v >> (8 * i)));
}
uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::vector<uint8_t> tokens_of(const std::vector<uint8_t> &st, const std::vector<PackDict> &dicts) {
    // the index: global position + 1 (0: empty), positions numbered across the dictionaries in order
    std::vector<uint32_t> table(size_t(1) << kBits, 0);
    std::vector<uint64_t> base(dicts.size() + 1, 0);
    for (size_t d = 0; d < dicts.size(); d++) {
        base[d + 1] = base[d] + dicts[d].size;
        if (base[d + 1] >= 0xffffffffu) throw State::Bad("pack: dictionaries over 4 GB");
        const uint8_t *b = dicts[d].data;
        for (size_t p = 0; p + kWin <= dicts[d].size; p += kStride)
            if (!uniform(b + p)) table[slot(hash_at(b + p))] = uint32_t(base[d] + p + 1);
    }
    std::vector<uint8_t> out;
    out.reserve(st.size() / 4);
    const uint8_t *s = st.data();
    const size_t n = st.size();
    size_t lit = 0, p = 0;
    uint32_t pw = 1; // kPrime^kWin (rolling out the oldest byte)
    for (unsigned i = 0; i < kWin; i++) pw *= kPrime;
    uint32_t h = n >= kWin ? hash_at(s) : 0;
    unsigned same = 1; // bytes equal to the window's last, ending there
    if (n >= kWin)
        for (unsigned i = kWin - 1; i > 0 && s[i - 1] == s[kWin - 1]; i--) same++;
    while (p + kWin <= n) {
        if (same < kWin) {
            const uint32_t e = table[slot(h)];
            if (e) {
                const uint64_t g = e - 1;
                size_t d = 0;
                while (g >= base[d + 1]) d++;
                const uint8_t *db = dicts[d].data;
                size_t c = size_t(g - base[d]);
                if (std::memcmp(db + c, s + p, kWin) == 0) {
                    size_t at = p;
                    while (at > lit && c > 0 && s[at - 1] == db[c - 1]) at--, c--;
                    size_t len = p - at + kWin;
                    while (at + len < n && c + len < dicts[d].size && s[at + len] == db[c + len]) len++;
                    if (len >= kMinMatch) {
                        put_varint(out, at - lit);
                        out.insert(out.end(), s + lit, s + at);
                        put_varint(out, len);
                        out.push_back(uint8_t(d));
                        put_varint(out, c);
                        p = lit = at + len;
                        if (p + kWin <= n) {
                            h = hash_at(s + p);
                            same = 1;
                            for (unsigned i = kWin - 1; i > 0 && s[p + i - 1] == s[p + kWin - 1]; i--) same++;
                        }
                        continue;
                    }
                }
            }
        }
        if (p + kWin < n) {
            h = h * kPrime + s[p + kWin] - pw * s[p];
            same = s[p + kWin] == s[p + kWin - 1] ? (same < kWin ? same + 1 : kWin) : 1;
        }
        p++;
    }
    put_varint(out, n - lit);
    out.insert(out.end(), s + lit, s + n);
    put_varint(out, 0);
    return out;
}
} // namespace

std::vector<uint8_t> pack_state(const std::vector<uint8_t> &state, const std::vector<PackDict> &dicts) {
    if (dicts.size() > 255 || state.size() >= 0xffffffffu) throw State::Bad("pack: too big");
    const std::vector<uint8_t> tok = tokens_of(state, dicts);
    std::vector<uint8_t> out = {'M', '2', 'P', 'K'};
#ifdef M2_HAVE_7Z
    out.push_back(1);
    put32(out, uint32_t(state.size()));
    put32(out, uint32_t(tok.size()));
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    props.level = 5;
    props.dictSize = 1u << 22;
    props.numThreads = 1;
    uint8_t prop[LZMA_PROPS_SIZE];
    SizeT prop_n = LZMA_PROPS_SIZE;
    std::vector<uint8_t> z(tok.size() + tok.size() / 3 + 1024);
    SizeT zn = z.size();
    const SRes r = LzmaEncode(z.data(), &zn, tok.data(), tok.size(), &props, prop, &prop_n, 0, nullptr, &g_Alloc, &g_Alloc);
    if (r != SZ_OK || prop_n != LZMA_PROPS_SIZE) throw State::Bad("pack: LZMA failed (" + std::to_string(r) + ")");
    out.insert(out.end(), prop, prop + LZMA_PROPS_SIZE);
    out.insert(out.end(), z.begin(), z.begin() + long(zn));
#else
    out.push_back(0); // (no LZMA in this build: the tokens as they are)
    put32(out, uint32_t(state.size()));
    put32(out, uint32_t(tok.size()));
    out.insert(out.end(), tok.begin(), tok.end());
#endif
    return out;
}

std::vector<uint8_t> unpack_state(const uint8_t *packed, size_t n, const std::vector<PackDict> &dicts) {
    if (n < 13 || std::memcmp(packed, "M2PK", 4) != 0) throw State::Bad("not a packed state");
    const uint8_t format = packed[4];
    const uint32_t size = get32(packed + 5), tok_n = get32(packed + 9);
    if (size > (256u << 20) || tok_n > (512u << 20)) throw State::Bad("packed state: too big");
    std::vector<uint8_t> tok;
    if (format == 0) {
        if (n - 13 != tok_n) throw State::Bad("packed state: the wrong size");
        tok.assign(packed + 13, packed + n);
    } else if (format == 1) {
#ifdef M2_HAVE_7Z
        if (n < 13 + LZMA_PROPS_SIZE) throw State::Bad("packed state: cut short");
        tok.resize(tok_n);
        SizeT out_n = tok_n, in_n = n - 13 - LZMA_PROPS_SIZE;
        ELzmaStatus status;
        const SRes r = LzmaDecode(tok.data(), &out_n, packed + 13 + LZMA_PROPS_SIZE, &in_n, packed + 13, LZMA_PROPS_SIZE,
                                  LZMA_FINISH_END, &status, &g_Alloc);
        if (r != SZ_OK || out_n != tok_n) throw State::Bad("packed state: LZMA data damaged");
#else
        throw State::Bad("packed state: LZMA, which this build hasn't");
#endif
    } else {
        throw State::Bad("packed state: an unknown format");
    }
    std::vector<uint8_t> st;
    st.reserve(size);
    const uint8_t *p = tok.data(), *end = p + tok.size();
    for (;;) {
        const uint64_t lit = get_varint(p, end);
        if (lit > uint64_t(end - p) || lit > size - st.size()) throw State::Bad("packed state: bad literals");
        st.insert(st.end(), p, p + lit);
        p += lit;
        const uint64_t len = get_varint(p, end);
        if (!len) break;
        if (p >= end) throw State::Bad("packed state: cut short");
        const uint8_t d = *p++;
        const uint64_t off = get_varint(p, end);
        if (d >= dicts.size() || off > dicts[d].size || len > dicts[d].size - off || len > size - st.size())
            throw State::Bad("packed state: a reference outside the ROMs (another ROM set?)");
        st.insert(st.end(), dicts[d].data + off, dicts[d].data + off + len);
    }
    if (st.size() != size || p != end) throw State::Bad("packed state: the wrong size");
    return st;
}

} // namespace rt
