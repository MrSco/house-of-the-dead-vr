// Save states (online play's drop-in: the host sends the game as it is, the joiner loads it, instead of replaying
// every frame since the host's start). One function per class lists its state, s(field) after s(field), and serves
// both directions: State saves into a byte buffer or loads from one. Fields go one by one, as their own sizes, never
// a struct's bytes (padding and layout differ between compilers): the bytes are the same on every platform the apps
// run on (all little-endian: x64 and arm64), so a PC's state loads on a Quest. Tags between the parts catch a load
// whose fields don't line up (another build): State::Bad. Fields are never `long` (4 bytes on Windows, 8 elsewhere).
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace rt {

static_assert(std::endian::native == std::endian::little, "save states are little-endian");

class State {
public:
    // saving into out
    explicit State(std::vector<uint8_t> &out) : out_(&out) {}
    // loading from in (n bytes)
    State(const uint8_t *in, size_t n) : in_(in), n_(n) {}

    bool loading() const { return in_ != nullptr; }
    struct Bad : std::runtime_error { using std::runtime_error::runtime_error; };

    template <typename T> State &operator()(T &v) {
        if constexpr (std::is_enum_v<T>) {
            auto u = static_cast<std::underlying_type_t<T>>(v);
            (*this)(u);
            v = static_cast<T>(u);
        } else {
            static_assert(std::is_arithmetic_v<T>, "State: arithmetic fields only (list a struct's fields)");
            bytes(&v, sizeof v);
        }
        return *this;
    }
    template <typename T, size_t N> State &operator()(T (&a)[N]) {
        if constexpr (std::is_arithmetic_v<T>) bytes(a, sizeof a);
        else for (auto &x : a) (*this)(x);
        return *this;
    }
    template <typename T, size_t N> State &operator()(std::array<T, N> &a) {
        if constexpr (std::is_arithmetic_v<T>) bytes(a.data(), sizeof(T) * N);
        else for (auto &x : a) (*this)(x);
        return *this;
    }
    template <typename T> State &operator()(std::vector<T> &v) {
        static_assert(std::is_arithmetic_v<T>, "State: vectors of arithmetic values");
        uint64_t n = v.size();
        (*this)(n);
        if (loading()) {
            if (n > (n_ - pos_) / sizeof(T)) throw Bad("save state: a vector longer than the state");
            v.resize(size_t(n));
        }
        bytes(v.data(), size_t(n) * sizeof(T));
        return *this;
    }
    State &operator()(std::string &t) {
        std::vector<uint8_t> v(t.begin(), t.end());
        (*this)(v);
        if (loading()) t.assign(v.begin(), v.end());
        return *this;
    }
    State &operator()(std::deque<uint8_t> &d) {
        std::vector<uint8_t> v(d.begin(), d.end());
        (*this)(v);
        if (loading()) d.assign(v.begin(), v.end());
        return *this;
    }
    // a fixed-size block (a buffer whose size the load already has: its size is checked, not changed)
    State &block(void *p, size_t n) {
        uint64_t m = n;
        (*this)(m);
        if (m != n) throw Bad("save state: a block of another size");
        bytes(p, n);
        return *this;
    }
    // a marker between parts: a load stops at the first one out of place
    State &tag(const char (&t)[5]) {
        uint32_t v = 0, want = 0;
        std::memcpy(&want, t, 4);
        v = want;
        (*this)(v);
        if (v != want) throw Bad(std::string("save state: no ") + t + " where it should be (another build's state?)");
        return *this;
    }
    size_t size() const { return out_ ? out_->size() : n_; }
    bool at_end() const { return pos_ == n_; }

private:
    void bytes(void *p, size_t n) {
        if (out_) {
            const uint8_t *b = static_cast<const uint8_t *>(p);
            out_->insert(out_->end(), b, b + n);
        } else {
            if (n > n_ - pos_) throw Bad("save state: shorter than its fields");
            std::memcpy(p, in_ + pos_, n);
            pos_ += n;
        }
    }
    std::vector<uint8_t> *out_ = nullptr;
    const uint8_t *in_ = nullptr;
    size_t n_ = 0, pos_ = 0;
};

} // namespace rt
