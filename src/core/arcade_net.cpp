// Online play for the arcade game: see arcade_net.h. The C API (net_*) is main's src/netplay.h's, as online.c uses
// it; the lockstep follows netplay.c (whose bundle layout this keeps).
#include "core/arcade_net.h"

#include "runtime/board/cabinet_controls.h"

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" void host_log(const char *fmt, ...);

namespace {

// ---- netplay.h's layout (kept so online.c carries the bundles: NET_BUNDLE_HEAD 121, NET_EVENT_SIZE 76)
#pragma pack(push, 1)
struct NetInput { uint32_t flags; float wx, wy; uint8_t chr, key, pad[2]; };
struct NetEvent { uint8_t type, slot; char name[30]; uint8_t data[44]; };
struct Bundle {
    uint32_t frame;
    uint32_t crc;              // the game's state checksum at this frame (every kCrcEvery frames), else 0
    uint8_t input[64];         // the cabinet's input (Packed)
    NetInput guest[3];         // (as sent: player 2's gun)
    uint8_t nev;
    NetEvent ev[4];
};
// The cabinet's input for one frame, as every player's game applies it.
struct Packed { uint8_t in0, in1, pad[2]; uint16_t gun_x, gun_y, gun2_x, gun2_y; };
#pragma pack(pop)
constexpr size_t kHead = offsetof(Bundle, ev);
static_assert(kHead == 121 && sizeof(NetEvent) == 76, "netplay.h's bundle layout");
static_assert(sizeof(Packed) <= 64, "the input block");
constexpr uint32_t kCrcEvery = 60;
constexpr int kDikSkip = 0x39;  // online.c's DIK_SKIP: the guest's skip button
constexpr uint32_t kLatch = 3;  // trigger and reload: kept until a bundle carried them

std::mutex mu;
arcade_net::Mode mode_ = arcade_net::Off;
uint32_t tact = 0, checks = 0;
bool desync_ = false;
FILE *record = nullptr;

// host: the guest's latest gun (slot 1), presses latched until sent, its skip presses
NetInput latest{};
uint32_t latched = 0;
float latched_x = 0, latched_y = 0;
int guest_skips = 0;
bool guest_in = false;

// guest: bundles received, in order
std::vector<uint8_t> queue;
size_t qpos = 0;
int nqueued = 0;

// The skip buttons' coins and Start (made by the host, or alone): two coins (the cabinet's two credits a game) for
// each press, one pulse at a time, then that player's Start.
struct Coins {
    int pending = 0, on = 0, gap = 0;
    bool start_req[2] = {false, false};
    int start_on[2] = {0, 0};
    void press(int p) { pending += 2; start_req[p] = true; }
    void apply(rt::Inputs &in) {
        if (on > 0) {
            in.in0 &= uint8_t(~0x01);
            if (--on == 0) gap = 6;
        } else if (gap > 0) {
            gap--;
        } else if (pending > 0) {
            pending--;
            on = 5;
            in.in0 &= uint8_t(~0x01);
        } else {
            for (int p = 0; p < 2; p++)
                if (start_req[p]) start_req[p] = false, start_on[p] = 6;
        }
        for (int p = 0; p < 2; p++)
            if (start_on[p] > 0) {
                start_on[p]--;
                in.in0 &= uint8_t(~(p ? 0x20 : 0x10));
            }
    }
} coins;

void aim(bool trigger, bool reload, bool on_screen, float x, float y, uint16_t &gx, uint16_t &gy, bool &pulled) {
    const rt::GunRange &gun = rt::game().gun;
    const bool off = reload || !on_screen;
    const rt::GunAim a = rt::aim_gun(off ? -1.f : x, off ? -1.f : y, gun.x_min, gun.x_max, gun.y_min, gun.y_max);
    gx = a.x, gy = a.y;
    pulled = trigger || reload;
}

void pack(const rt::Inputs &in, Packed &p) {
    p = Packed{};
    p.in0 = in.in0, p.in1 = in.in1, p.gun_x = in.gun_x, p.gun_y = in.gun_y, p.gun2_x = in.gun2_x, p.gun2_y = in.gun2_y;
}
void unpack(const Packed &p, rt::Inputs &in) {
    in = rt::Inputs{};
    in.in0 = p.in0, in.in1 = p.in1, in.gun_x = p.gun_x, in.gun_y = p.gun_y, in.gun2_x = p.gun2_x, in.gun2_y = p.gun2_y;
}

} // namespace

// ---- the C API online.c uses (netplay.h)
extern "C" {
void (*net_bundle_out)(const uint8_t *bundle, int len) = nullptr;
int (*net_bundle_wait)(void) = nullptr;
void (*net_desync_out)(uint32_t frame) = nullptr;
void (*net_guest_out)(int flags, int x, int y, int key) = nullptr;
int net_local = 0;

void net_lock(void) { mu.lock(); }
void net_unlock(void) { mu.unlock(); }
uint32_t net_tact(void) { return tact; }

// mode 1 = host, 2 = guest; before the game starts
void net_configure(int mode, int slot) {
    mode_ = mode == 1 ? arcade_net::Host : mode == 2 ? arcade_net::Guest : arcade_net::Off;
    net_local = slot;
    host_log("arcade online: %s, player %d\n", mode_ == arcade_net::Host ? "hosting" : mode_ == arcade_net::Guest ? "guest" : "off",
             slot + 1);
}
// (net_lock held by the caller)
void net_host_join(int slot, const char *name, const char *data_hex) {
    (void)data_hex;
    if (slot != 1) return;
    latest = NetInput{};
    latched = 0;
    guest_in = true;
    host_log("arcade online: %s is player 2\n", name ? name : "?");
}
void net_host_leave(int slot) {
    if (slot != 1) return;
    latest = NetInput{};
    latched = 0;
    guest_in = false;
}
void net_host_input(int slot, int flags, float wx, float wy, int chr, int key) {
    (void)chr;
    if (slot != 1) return;
    const uint32_t pressed = uint32_t(flags) & kLatch & ~latched;
    if (pressed) latched |= pressed, latched_x = wx, latched_y = wy;
    latest.flags = uint32_t(flags), latest.wx = wx, latest.wy = wy;
    if (key == kDikSkip) guest_skips++;
}
// guest: bundles from the host, in order (several back to back)
void net_push_bundles(const uint8_t *data, int len) {
    if (len <= 0) return;
    if (qpos && qpos == queue.size()) queue.clear(), qpos = 0;
    queue.insert(queue.end(), data, data + len);
    for (int off = 0; off + int(kHead) <= len;) {
        off += int(kHead) + data[off + kHead - 1] * int(sizeof(NetEvent));
        nqueued++;
    }
}
}

namespace arcade_net {

Mode mode() { return mode_; }
int local_slot() { return net_local; }
uint32_t frames() { return tact; }
bool desynced() { return desync_; }
uint32_t checks_ok() { return checks; }
int catching_up() {
    if (mode_ != Guest) return 0;
    std::lock_guard<std::mutex> lock(mu);
    return nqueued > 30 ? 2 : nqueued > 3 ? 1 : 0;
}

bool frame(const Local &local, rt::Inputs &out, uint32_t (*state_crc)()) {
    if (mode_ == Guest) {
        // this player's gun to the host (online.c counts the presses)
        if (net_guest_out)
            net_guest_out((local.fire ? 1 : 0) | (local.reload ? 2 : 0) | (local.on_screen ? 4 : 0), local.x, local.y,
                          local.skip ? kDikSkip : 0);
        Bundle b{};
        std::unique_lock<std::mutex> lock(mu);
        while (qpos + kHead > queue.size()) {
            if (!net_bundle_wait) return false;
            lock.unlock();
            const int again = net_bundle_wait();
            lock.lock();
            if (!again) return false;
        }
        std::memcpy(&b, queue.data() + qpos, kHead);
        const size_t len = kHead + size_t(b.nev) * sizeof(NetEvent);
        std::memcpy(&b, queue.data() + qpos, len);
        qpos += len;
        nqueued--;
        lock.unlock();
        tact++;
        if (b.frame != tact) host_log("arcade online: bundle for frame %u at frame %u\n", b.frame, tact);
        if (b.crc && state_crc) {
            const uint32_t mine = state_crc() | 1; // (as the host makes it: never 0)
            if (mine != b.crc) {
                if (!desync_) {
                    host_log("arcade online: DESYNC at frame %u: host %08x, here %08x\n", tact, b.crc, mine);
                    if (net_desync_out) net_desync_out(tact);
                }
                desync_ = true;
            } else if (++checks % 20 == 0) {
                host_log("arcade online: frame %u, %u checksums matched\n", tact, checks);
            }
        }
        Packed p;
        std::memcpy(&p, b.input, sizeof p);
        unpack(p, out);
        return true;
    }

    // host, or alone: player 1 is this player; player 2 the guest's gun
    tact++;
    Bundle b{};
    b.frame = tact;
    if (tact % kCrcEvery == 0 && state_crc) b.crc = state_crc() | 1;
    rt::Inputs in;
    bool pulled = false;
    aim(local.fire, local.reload, local.on_screen, float(local.x), float(local.y), in.gun_x, in.gun_y, pulled);
    if (pulled) in.in1 &= uint8_t(~0x01);
    if (local.skip) coins.press(0);
    {
        std::lock_guard<std::mutex> lock(mu);
        if (guest_in) {
            NetInput g = latest;
            if (latched) { // a press since the last bundle: this frame has it, where it happened
                g.flags |= latched;
                if (!(latest.flags & latched)) g.wx = latched_x, g.wy = latched_y;
                latched = 0;
            }
            bool p2 = false;
            aim(g.flags & 1, g.flags & 2, g.flags & 4, g.wx, g.wy, in.gun2_x, in.gun2_y, p2);
            if (p2) in.in1 &= uint8_t(~0x02);
            b.guest[0] = g;
            for (; guest_skips > 0; guest_skips--) coins.press(1);
        }
    }
    coins.apply(in);
    Packed p;
    pack(in, p);
    std::memcpy(b.input, &p, sizeof p);
    const int len = int(kHead);
    if (mode_ == Host && net_bundle_out) net_bundle_out(reinterpret_cast<const uint8_t *>(&b), len);
    if (record) std::fwrite(&b, 1, size_t(len), record);
    out = in;
    return true;
}

void record_to(const std::string &path) { record = std::fopen(path.c_str(), "wb"); }

bool replay_from(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    mode_ = Guest;
    net_local = 1;
    net_push_bundles(d.data(), int(d.size()));
    return true;
}

} // namespace arcade_net
