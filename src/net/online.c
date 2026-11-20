// Online co-op for the native builds (the Quest; the desktop build for tests). See online.h.
//
// Two players share one game as at the cabinet: the host (P1) runs it and every frame's input goes out as a bundle
// (netplay.c, the lockstep the web build also uses); the guest (P2) runs the same game from those bundles, and its
// own gun goes back to the host, which puts it in the next bundle as player 2's. Both games start at the game's boot
// with fresh save files, so the guest's is the host's, frame for frame (the Quest and the desktop build compute the
// same game: PORT_NET_RECORD on one, PORT_NET_REPLAY on the other, every checksum matches).
//
// So an online game begins with a restart: the ONLINE tab writes online-session.cfg and the app opens again into
// it (online_restart). A host plays at once and keeps every bundle; a guest may join at any time: the host saves
// its game as it is (a save state, packed to a couple of hundred KB against the ROMs both have: arcade_net.h) and
// sends it; the guest loads it and runs the frames since from the host's bundles, then plays along.
//
// Transport: the NMN2 lobby / relay protocol of namco22-decompile (src/net/net.c, as Time Crisis II VR uses it):
// rooms with four-letter codes on a lobby server (zonesync.net by default), or the built-in host on the local
// network (net_host.c, found by broadcast or joined by address). Inside a session each side sends FRAMEs of 256
// bytes, every few milliseconds when it has something new:
//   host 'H': its run's id, the first frame carried, how many, its newest frame, a voice block, then the bundles
//             (each against the one before it in the datagram, so one datagram decodes on its own);
//   host 'S': its run's id, the save state's frame, its size, the offset of the piece carried and its length, the
//             piece: a joining guest's start, sent before any bundle (a window ahead of what the guest has, from
//             there again when that doesn't move), several a round;
//   guest 'G': its run's id, the last frame it has without a gap (the host sends on from there and goes back to
//             it when nothing moves on), its gun (state, with counters of trigger pulls / reloads / skips so a quick
//             pull between two datagrams still counts once), how much of the save state it has, a voice block.
// Each side's guns and hands in VR (online_pose_out) ride along at the datagram's end: the host's length in its
// byte 11, there only when the bundles leave room; the guest's length in its last byte, the pose just before it.
// A datagram the receiver can't use (a gap before it) is dropped; the host's history makes every loss good.
#include "online.h"
#include "net.h"
#include "rooms.h"
#include "netplay.h"
#include "net_sock.h"
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__ANDROID__) || defined(HOTD_VOICE)
#include "voice.h"
#else
// the desktop test partner (tools/m2netguest) has no voice chat; the apps have (HOTD_VOICE: the flat ones)
typedef struct { int active, capturing, talking, partner_talking, mic_denied, mode, muted, level, partner_level; uint32_t sent, received, lost; } voice_status_t;
static void voice_set_active(int on) { (void)on; }
static void voice_set_mode(int m) { (void)m; }
static void voice_set_muted(int m) { (void)m; }
static void voice_set_volume(int v) { (void)v; }
static void voice_talk_key(int h) { (void)h; }
static int voice_pack(uint8_t *o, int max) { (void)o; (void)max; return 0; }
static void voice_unpack(const uint8_t *b, int n) { (void)b; (void)n; }
static void voice_get_status(voice_status_t *s) { memset(s, 0, sizeof *s); }
#endif

void host_log(const char *fmt, ...);

#define ONLINE_VER     3                // the link version: only equal versions share a room (3: save states)
#define FRAME_SIZE     256              // what a server gives a game it doesn't list
#define DEFAULT_SERVER "zonesync.net"
#define NAME_LEN       12
#define WINDOW         600              // frames the host sends ahead of the guest's acknowledgement
#define GONE_MS        6000             // a partner silent this long has left
#define DIK_SKIP       0x39
#define SNAP_PIECE     (FRAME_SIZE - 20)  // the save state's bytes in an 'S'
#define SNAP_WINDOW    49152            // its bytes sent ahead of the guest's acknowledgement

void (*online_restart)(void);

static const char *slot_label(int slot) { return slot == 0 ? "P1" : slot == 1 ? "P2" : ""; }
static void session_begin(int slot, int players);
static void session_end(void);
static int frame_out(uint8_t *buf);
static void frame_in(int slot, const uint8_t *buf, uint32_t seq);
static void net_hint(const char *text, int frames) { (void)frames; host_log("online: %s\n", text); }
static const eng_net_game game = { NET_GAME_ID, ONLINE_VER, 2, FRAME_SIZE, "HOTD_NET", "game", 0,
                                    session_begin, session_end, frame_out, frame_in, net_hint, slot_label };

// ---------------------------------------------------------------- settings and state
static char files[512];
static char name[NAME_LEN + 1] = "PLAYER", server[64] = DEFAULT_SERVER, join_ip[24];
static char platform[12];               // online_set_platform: this app's word in its room's name ("" none)
static int voice_mode = 1, volume = 100, muted;
static int volume_set;                  // the VR menu set the partner volume: online.cfg's doesn't override it

static int role;                        // this run: 0 alone, 1 host (P1), 2 guest (P2)
static int lan;                         // host: the built-in host; guest: conn is the host's address
static char code[5];                    // the room's code
static int room_id;                     // guest: the room to join (0: whichever, a LAN host has one)
static char conn[64];                   // the server (or the LAN host) this run talks to

static SDL_mutex *mu;                   // the view, the menu's requests, the guest's gun
static SDL_mutex *hmu;                  // the host's history
static SDL_Thread *thread;
static volatile int quitting;
static int booted;

// what the menu sees (under mu)
static int st_finding, st_failed;
static char message[96];
static char partner[24];
static int linked_view;
static unsigned toast_serial; static char toast_text[96]; static uint32_t toast_until;
static int desynced;

// requests from the menu, carried out on the network thread (under mu)
enum { RQ_NONE, RQ_JOIN_CODE, RQ_QUICK, RQ_JOIN_GAME, RQ_REFRESH };
static int rq; static char rq_code[8]; static int rq_index;
static int browsing; static uint32_t browse_t;
static int renamed;                     // the name changed: the network thread tells the server
static int server_changed;              // the lobby server changed: the network thread drops the old one's room list

static uint32_t now_ms(void) { return SDL_GetTicks(); }
static void lock(void) { SDL_LockMutex(mu); }
static void unlock(void) { SDL_UnlockMutex(mu); }
static void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lock();
    vsnprintf(toast_text, sizeof toast_text, fmt, ap);
    toast_serial++; toast_until = now_ms() + 5000;
    unlock();
    va_end(ap);
    host_log("online: %s\n", toast_text);
}
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lock();
    vsnprintf(message, sizeof message, fmt, ap);
    unlock();
    va_end(ap);
    host_log("online: %s\n", message);
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t run_id(void) {
    uint64_t s = SDL_GetPerformanceCounter() ^ ((uint64_t)time(NULL) << 20) ^ (uintptr_t)&s;
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    uint32_t v = (uint32_t)(s >> 32);
    return v ? v : 1;
}

// ---------------------------------------------------------------- files
static void path_of(char *out, size_t n, const char *leaf) { snprintf(out, n, "%s%s%s", files, files[0] ? "/" : "", leaf); }
static int save_cfg(void) {
    char p[600];
    path_of(p, sizeof p, "online.cfg");
    FILE *f = fopen(p, "w");
    if (!f) return 0;
    fprintf(f, "name=%s\nserver=%s\njoin=%s\nvoice=%d\nmic_muted=%d\nvoice_volume=%d\n", name, server, join_ip, voice_mode, muted, volume);
    const int ok = !ferror(f);
    return fclose(f) == 0 && ok;
}
static void load_cfg(void) {
    char p[600], line[256];
    path_of(p, sizeof p, "online.cfg");
    FILE *f = fopen(p, "r");
    if (!f) { save_cfg(); return; }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "name") && *v) snprintf(name, sizeof name, "%s", v);
        else if (!strcmp(line, "server") && *v) snprintf(server, sizeof server, "%s", v);
        else if (!strcmp(line, "join")) snprintf(join_ip, sizeof join_ip, "%s", v);
        else if (!strcmp(line, "voice")) voice_mode = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(line, "mic_muted")) muted = atoi(v) != 0;
        else if (!strcmp(line, "voice_volume") && !volume_set) volume = atoi(v) < 0 ? 0 : atoi(v) > 200 ? 200 : atoi(v);
    }
    fclose(f);
}
// the restart into an online game: what it is, for the next start of the app
static void write_session(int r, int l, const char *srv, int room, const char *c) {
    char p[600];
    path_of(p, sizeof p, "online-session.cfg");
    FILE *f = fopen(p, "w");
    if (!f) { say("CANNOT WRITE %s", p); return; }
    fprintf(f, "role=%s\nlan=%d\nserver=%s\nroom=%d\ncode=%s\nwritten=%ld\n", r == 1 ? "host" : "guest", l, srv ? srv : "", room, c ? c : "", (long)time(NULL));
    fclose(f);
}
static int read_session(void) {
    char p[600], line[256];
    path_of(p, sizeof p, "online-session.cfg");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    long written = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "role")) role = !strcmp(v, "host") ? 1 : !strcmp(v, "guest") ? 2 : 0;
        else if (!strcmp(line, "lan")) lan = atoi(v);
        else if (!strcmp(line, "server")) snprintf(conn, sizeof conn, "%s", v);
        else if (!strcmp(line, "room")) room_id = atoi(v);
        else if (!strcmp(line, "code")) snprintf(code, sizeof code, "%s", v);
        else if (!strcmp(line, "written")) written = atol(v);
    }
    fclose(f);
    remove(p);                          // once: a crash in the online game starts the next run alone
    if (written && labs((long)time(NULL) - written) > 600) { role = 0; host_log("online: an old online-session.cfg, ignored\n"); }
    return role;
}
static void restart(void) {
    if (online_restart) { host_log("online: restarting the app\n"); online_restart(); }
    else say("RESTART THE GAME TO PLAY ONLINE");
}

// ---------------------------------------------------------------- the host's history of bundles
static uint8_t *hbuf; static size_t hlen, hcap;
static uint32_t *hoff; static uint32_t hn, hcapn;      // bundle of frame k + 1 at hbuf + hoff[k]
static void host_bundle(const uint8_t *b, int len) {
    SDL_LockMutex(hmu);
    if (rd32(b) != hn + 1) host_log("online: bundle %u after %u\n", rd32(b), hn);
    if (hn + 1 >= hcapn) { hcapn = hcapn ? hcapn * 2 : 65536; hoff = realloc(hoff, hcapn * sizeof *hoff); }
    if (hlen + (size_t)len > hcap) { hcap = hcap ? hcap * 2 : (1u << 22); hbuf = realloc(hbuf, hcap); }
    hoff[hn++] = (uint32_t)hlen;
    memcpy(hbuf + hlen, b, (size_t)len);
    hlen += (size_t)len;
    SDL_UnlockMutex(hmu);
}
static const uint8_t *hist(uint32_t frame, int *len) {   // under hmu
    uint32_t k = frame - 1;
    *len = (int)((k + 1 < hn ? hoff[k + 1] : hlen) - hoff[k]);
    return hbuf + hoff[k];
}

// A bundle against another (or nothing): the number of events, the number of runs, then the runs (bytes unchanged,
// bytes changed, the changed bytes xor the other's) over everything after the frame number; the events are against
// nothing (mostly zeros: a name and a few bytes of data).
#define MAX_BUNDLE (NET_BUNDLE_HEAD + 4 * NET_EVENT_SIZE)
static int encode(uint8_t *o, int max, const uint8_t *cur, int len, const uint8_t *base) {
    int nev = cur[NET_BUNDLE_HEAD - 1], L = len - 4;
    if (nev > 4 || len != NET_BUNDLE_HEAD + nev * NET_EVENT_SIZE) return -1;
    uint8_t d[MAX_BUNDLE];
    for (int i = 0; i < L; i++) d[i] = cur[4 + i] ^ (base && 4 + i < NET_BUNDLE_HEAD - 1 ? base[4 + i] : 0);
    uint8_t tmp[2 + 2 * MAX_BUNDLE];
    int n = 2, ntok = 0, i = 0;
    while (i < L && ntok < 255) {
        int z = 0;
        while (i + z < L && !d[i + z] && z < 255) z++;
        if (i + z >= L) break;
        int l = 0;
        // a literal run takes in short gaps (a run costs two bytes)
        while (i + z + l < L && l < 255) {
            if (d[i + z + l]) { l++; continue; }
            int g = 0;
            while (i + z + l + g < L && !d[i + z + l + g] && g < 3) g++;
            if (g < 3 && i + z + l + g < L && l + g < 255) l += g; else break;
        }
        tmp[n++] = (uint8_t)z; tmp[n++] = (uint8_t)l;
        memcpy(tmp + n, d + i + z, (size_t)l); n += l;
        ntok++; i += z + l;
    }
    if (i < L && ntok == 255) return -1;
    tmp[0] = (uint8_t)nev; tmp[1] = (uint8_t)ntok;
    if (n > max) return 0;
    memcpy(o, tmp, (size_t)n);
    return n;
}
// out: the bundle (its frame number left 0); returns its length (-1: not a bundle), the bytes of p used in *used
static int decode(const uint8_t *p, int avail, uint8_t *out, const uint8_t *base, int *used) {
    if (avail < 2) return -1;
    int nev = p[0], ntok = p[1], k = 2, i = 0;
    if (nev > 4) return -1;
    int len = NET_BUNDLE_HEAD + nev * NET_EVENT_SIZE, L = len - 4;
    memset(out, 0, (size_t)len);
    if (base) memcpy(out + 4, base + 4, NET_BUNDLE_HEAD - 5);
    for (int t = 0; t < ntok; t++) {
        if (k + 2 > avail) return -1;
        int z = p[k], l = p[k + 1];
        k += 2; i += z;
        if (i + l > L || k + l > avail) return -1;
        for (int j = 0; j < l; j++) out[4 + i + j] ^= p[k + j];
        k += l; i += l;
    }
    if (out[NET_BUNDLE_HEAD - 1] != nev) return -1;
    *used = k;
    return len;
}

// ---------------------------------------------------------------- the link (network thread)
static uint32_t my_id;                  // this run's id
// host
static uint32_t guest_id, gack, cursor, ack_moved, last_tx, last_g_rx;
static int guest_in;                    // a guest is in the game (its join went into a bundle)
static uint8_t g_ct, g_cr, g_ck;        // the guest's counters, as last seen
// host: the save state a joining guest starts from, packed (under hmu): 0 none, 1 asked of the game, 2 sending
static int snap_state;
static uint8_t *snap;
static uint32_t snap_len, snap_frame, snap_acked, snap_cursor, snap_moved;
static uint32_t snap_t0, snap_pieces, snap_rewinds;   // (the log: when it was ready, pieces sent, times sent again)
// guest: the host's save state as it comes in
static uint8_t *gsnap, *gsnap_have;     // (gsnap_have: the pieces in, a byte each: kept after a lost one)
static uint32_t gsnap_len, gsnap_frame, gsnap_got, gsnap_got_sent;
static int gsnap_done;
// guest
static uint32_t host_id, last_h_rx, host_newest;
static volatile uint32_t ack;           // frames received without a gap (= pushed into netplay's queue)
static int host_gone;
static struct { int flags, x, y; uint8_t ct, cr, ck; } gun, gun_sent;   // the guest's gun (gun under mu)
static uint32_t ack_sent;
// the players' poses (under mu): ours to send (a serial for each new one), the partner's latest
static uint8_t pose_me[ONLINE_POSE_MAX], pose_them[ONLINE_POSE_MAX];
static int pose_me_n, pose_them_n;
static uint32_t pose_me_seq, pose_sent_seq, pose_them_t;
static int pose_take(uint8_t *out, int room) {   // ours, if it fits in room: its length (0: none or no room)
    lock();
    int n = pose_me_n <= room ? pose_me_n : 0;
    if (n) { memcpy(out, pose_me, (size_t)n); pose_sent_seq = pose_me_seq; }
    unlock();
    return n;
}
static int pose_fresh(void) { lock(); int f = pose_me_n && pose_me_seq != pose_sent_seq; unlock(); return f; }
static void pose_got(const uint8_t *p, int n) {
    if (n <= 0 || n > ONLINE_POSE_MAX) return;
    lock(); memcpy(pose_them, p, (size_t)n); pose_them_n = n; pose_them_t = now_ms(); unlock();
}
void online_pose_out(const uint8_t *pose, int len) {
    if (!role || len <= 0 || len > ONLINE_POSE_MAX) return;
    lock(); memcpy(pose_me, pose, (size_t)len); pose_me_n = len; pose_me_seq++; unlock();
}
int online_pose_in(uint8_t *pose, uint32_t *age_ms) {
    lock();
    int n = pose_them_n;
    if (n) { memcpy(pose, pose_them, (size_t)n); *age_ms = now_ms() - pose_them_t; }
    unlock();
    return n;
}
static uint32_t n_tx, n_rx;

static int rto_ms(void) { int p = eng_net_ping_ms(); return p > 0 ? 2 * p + 60 : 200; }

static int voice_into(uint8_t *at, int max) {
    if (max <= 0) return 0;
    int n = voice_pack(at, max > 120 ? 120 : max);
    return n > 0 ? n : 0;
}
// host, under hmu: the save state's next piece ('S'; 1), or while there is none to send (the game thread saving,
// the guest's acknowledgement awaited) an empty 'H' now and then to keep the link (1), else 0
static int snap_frame_out(uint8_t *buf, uint32_t now) {
    memset(buf, 0, FRAME_SIZE);
    if (snap_state == 2) {
        if (snap_cursor < snap_acked) snap_cursor = snap_acked;
        // when the guest's count stops moving, from there again (it keeps the pieces it has: its count jumps on as
        // the lost ones come; one lost piece at a time was slower, losses come many together on a busy link)
        if (snap_cursor > snap_acked && (int)(now - snap_moved) > rto_ms()) { snap_cursor = snap_acked; snap_moved = now; snap_rewinds++; }
        uint32_t at = snap_cursor;
        if (at < snap_len && snap_cursor - snap_acked < SNAP_WINDOW) {
            uint32_t n = snap_len - at < SNAP_PIECE ? snap_len - at : SNAP_PIECE;
            buf[0] = 'S'; buf[1] = ONLINE_VER;
            wr32(buf + 2, my_id); wr32(buf + 6, snap_frame); wr32(buf + 10, snap_len); wr32(buf + 14, at);
            buf[18] = (uint8_t)n; buf[19] = (uint8_t)(n >> 8);
            memcpy(buf + 20, snap + at, n);
            if (at == snap_cursor) snap_cursor += n;
            snap_pieces++;
            return 1;
        }
    }
    if (now - last_tx < 40) return 0;
    buf[0] = 'H'; buf[1] = ONLINE_VER;
    wr32(buf + 2, my_id);
    wr32(buf + 12, hn);
    return 1;
}
static int snap_sending(void) {   // (host) pieces to send now: the network thread sends a burst
    SDL_LockMutex(hmu);
    int s = snap_state == 2 && snap_cursor < snap_len && snap_cursor - snap_acked < SNAP_WINDOW;
    SDL_UnlockMutex(hmu);
    return s;
}
// host: the save state, packed (arcade_net's thread): sent if a guest still waits for it. Any save state of the
// host's game will do, with the bundles after its frame: a newer guest takes one asked for by an earlier.
static void snapshot_out(uint32_t frame, const uint8_t *data, int len) {
    SDL_LockMutex(hmu);
    if (snap_state == 1 && len > 0) {
        free(snap);
        snap = malloc((size_t)len);
        if (snap) {
            memcpy(snap, data, (size_t)len);
            snap_len = (uint32_t)len; snap_frame = frame; snap_acked = snap_cursor = 0; snap_moved = snap_t0 = now_ms(); snap_pieces = snap_rewinds = 0;
            snap_state = 2;
        }
    }
    SDL_UnlockMutex(hmu);
    host_log("online: the game after frame %u for the guest: %d bytes to send\n", frame, len);
}

static int frame_out(uint8_t *buf) {
    uint32_t now = now_ms();
    if (role == 1 && guest_id) {
        SDL_LockMutex(hmu);
        int r = snap_state ? snap_frame_out(buf, now) : -1;
        SDL_UnlockMutex(hmu);
        if (r >= 0) {
            if (r) { last_tx = now; n_tx++; }
            return r;
        }
    }
    if (now - last_tx < 4) return 0;
    memset(buf, 0, FRAME_SIZE);
    if (role == 1) {
        if (!guest_id) return 0;                       // nobody to send to yet
        const int pf = pose_fresh();                   // (mu, never taken under hmu)
        SDL_LockMutex(hmu);
        uint32_t newest = hn;
        if (cursor <= gack) cursor = gack + 1;
        if (cursor > gack + 1 && (int)(now - ack_moved) > rto_ms()) { cursor = gack + 1; ack_moved = now; }   // lost: again
        int fresh = cursor <= newest && cursor - gack <= WINDOW;
        // (a new pose of ours goes out at up to ~80 Hz on its own)
        if (!fresh && !(pf && now - last_tx >= 12) && now - last_tx < 40) { SDL_UnlockMutex(hmu); return 0; }
        buf[0] = 'H'; buf[1] = ONLINE_VER;
        wr32(buf + 2, my_id);
        wr32(buf + 6, cursor);
        wr32(buf + 12, newest);
        int v = voice_into(buf + 17, 120);   // an Opus frame or two; the bundles take the rest
        buf[16] = (uint8_t)v;
        int at = 17 + v, n = 0;
        const uint8_t *prev = NULL;
        while (fresh && cursor + n <= newest && cursor + n - gack <= WINDOW && n < 255) {
            int len;
            const uint8_t *b = hist(cursor + n, &len);
            int k = encode(buf + at, FRAME_SIZE - at, b, len, prev);
            if (k <= 0) break;
            at += k; n++; prev = b;
        }
        buf[10] = (uint8_t)n;
        cursor += (uint32_t)n;
        SDL_UnlockMutex(hmu);
        {   // our pose, at the end, where the bundles left room (a guest catching up fills it: none then)
            uint8_t pose[ONLINE_POSE_MAX];
            int p = pose_take(pose, FRAME_SIZE - at);
            if (p) { memcpy(buf + FRAME_SIZE - p, pose, (size_t)p); buf[11] = (uint8_t)p; }
        }
        last_tx = now; n_tx++;
        return 1;
    }
    if (role == 2) {
        lock();
        int changed = memcmp(&gun, &gun_sent, sizeof gun) != 0;
        gun_sent = gun;
        unlock();
        uint32_t a = ack;
        if (!changed && a == ack_sent && gsnap_got == gsnap_got_sent && !(pose_fresh() && now - last_tx >= 12) &&
            now - last_tx < 30) return 0;
        buf[0] = 'G'; buf[1] = ONLINE_VER;
        wr32(buf + 2, my_id);
        wr32(buf + 6, a);
        buf[10] = (uint8_t)gun_sent.flags;
        buf[11] = (uint8_t)gun_sent.x; buf[12] = (uint8_t)(gun_sent.x >> 8);
        buf[13] = (uint8_t)gun_sent.y; buf[14] = (uint8_t)(gun_sent.y >> 8);
        buf[15] = gun_sent.ct; buf[16] = gun_sent.cr; buf[17] = gun_sent.ck;
        wr32(buf + 18, gsnap_got);
        gsnap_got_sent = gsnap_got;
        int v = voice_into(buf + 23, 120);
        buf[22] = (uint8_t)v;
        {   // our pose, just before the last byte (its length)
            uint8_t pose[ONLINE_POSE_MAX];
            int p = pose_take(pose, FRAME_SIZE - 1 - (23 + v));
            if (p) { memcpy(buf + FRAME_SIZE - 1 - p, pose, (size_t)p); buf[FRAME_SIZE - 1] = (uint8_t)p; }
        }
        ack_sent = a; last_tx = now; n_tx++;
        return 1;
    }
    return 0;
}
static void partner_name(void) {
    char nm[24]; int ready, self;
    for (int i = 0; i < eng_net_roster_count(); i++)
        if (eng_net_roster(i, nm, sizeof nm, &ready, &self) && !self) { lock(); snprintf(partner, sizeof partner, "%s", nm); unlock(); return; }
}
static void frame_in(int slot, const uint8_t *buf, uint32_t seq) {
    (void)slot; (void)seq;
    uint32_t now = now_ms();
    if (buf[1] != ONLINE_VER) return;
    n_rx++;
    if (role == 1 && buf[0] == 'G') {
        uint32_t id = rd32(buf + 2);
        last_g_rx = now;
        if (id != guest_id) {                          // a guest (again: a new run of it starts from the first frame)
            partner_name();
            net_lock();
            if (guest_in) net_host_leave(1);
            net_host_join(1, partner, "");
            net_snapshot_request();                    // its start: the game as it is after the next frame
            net_unlock();
            SDL_LockMutex(hmu);
            snap_state = 1; free(snap); snap = NULL;
            SDL_UnlockMutex(hmu);
            guest_id = id; guest_in = 1; gack = 0; cursor = 1; ack_moved = now;
            g_ct = buf[15]; g_cr = buf[16]; g_ck = buf[17];
            lock(); linked_view = 1; unlock();
            toast("%s JOINED AS P2", partner[0] ? partner : "A PLAYER");
            voice_set_active(1);
        }
        uint32_t a = rd32(buf + 6);
        SDL_LockMutex(hmu);
        if (snap_state == 2) {
            uint32_t got = rd32(buf + 18);
            if (got > snap_acked && got <= snap_len) { snap_acked = got; snap_moved = now; }
            if (a >= snap_frame && a <= hn) {             // loaded: the bundles after it from here
                host_log("online: the guest has the game (%u ms: %u pieces, sent again %u times, ping %d): its bundles from frame %u\n",
                         now - snap_t0, snap_pieces, snap_rewinds, eng_net_ping_ms(), snap_frame + 1);
                snap_state = 0; free(snap); snap = NULL;
            }
        }
        if (a > gack && a <= hn && !snap_state) { gack = a; ack_moved = now; }
        SDL_UnlockMutex(hmu);
        int flags = buf[10], x = (int16_t)(buf[11] | buf[12] << 8), y = (int16_t)(buf[13] | buf[14] << 8), pressed = 0, key = 0;
        if (buf[15] != g_ct) { pressed |= 1; g_ct = buf[15]; }
        if (buf[16] != g_cr) { pressed |= 2; g_cr = buf[16]; }
        if (buf[17] != g_ck) { key = DIK_SKIP; g_ck = buf[17]; }
        net_lock();
        net_host_input(1, flags | pressed, (float)x, (float)y, 0, key);
        net_unlock();
        if (buf[22]) voice_unpack(buf + 23, buf[22]);
        {
            int p = buf[FRAME_SIZE - 1];
            if (p && 23 + buf[22] + p <= FRAME_SIZE - 1) pose_got(buf + FRAME_SIZE - 1 - p, p);
        }
        return;
    }
    if (role == 2 && buf[0] == 'S') {                 // the host's game, piece by piece
        uint32_t id = rd32(buf + 2);
        if (!host_id) { host_id = id; partner_name(); toast("JOINING %s'S GAME", partner[0] ? partner : "THE HOST"); voice_set_active(1); }
        if (id != host_id || gsnap_done) return;
        last_h_rx = now;
        if (host_gone) { host_gone = 0; toast("THE HOST IS BACK"); }
        uint32_t f = rd32(buf + 6), len = rd32(buf + 10), off = rd32(buf + 14), n = buf[18] | (uint32_t)buf[19] << 8;
        if (!len || len > (64u << 20) || n > SNAP_PIECE || off > len || n > len - off || off % SNAP_PIECE) return;
        if (!gsnap || len != gsnap_len || f != gsnap_frame) {   // (a newer one: from its start)
            free(gsnap); free(gsnap_have);
            gsnap = malloc(len);
            gsnap_have = calloc(len / SNAP_PIECE + 1, 1);
            if (!gsnap || !gsnap_have) { free(gsnap); free(gsnap_have); gsnap = gsnap_have = NULL; return; }
            gsnap_len = len; gsnap_frame = f; gsnap_got = 0;
        }
        // any piece is kept; what is acknowledged is how far they run without a gap (the host goes back to there)
        if (n == (len - off < SNAP_PIECE ? len - off : SNAP_PIECE) && !gsnap_have[off / SNAP_PIECE]) {
            memcpy(gsnap + off, buf + 20, n);
            gsnap_have[off / SNAP_PIECE] = 1;
            while (gsnap_got < gsnap_len && gsnap_have[gsnap_got / SNAP_PIECE])
                gsnap_got += gsnap_len - gsnap_got < SNAP_PIECE ? gsnap_len - gsnap_got : SNAP_PIECE;
        }
        if (gsnap_got == gsnap_len) {
            net_lock();
            net_snapshot_in(f, gsnap, (int)len);
            net_unlock();
            ack = f;                                   // the bundles after it: the host sends them next
            gsnap_done = 1;
            free(gsnap); free(gsnap_have); gsnap = gsnap_have = NULL;
            host_log("online: the host's game after frame %u came (%u bytes)\n", f, len);
            toast("CONNECTED TO %s: PRESS START (A / X, SPACE) TO PLAY", partner[0] ? partner : "THE HOST");
        }
        return;
    }
    if (role == 2 && buf[0] == 'H') {
        uint32_t id = rd32(buf + 2);
        if (!host_id) { host_id = id; partner_name(); toast("JOINING %s'S GAME", partner[0] ? partner : "THE HOST"); voice_set_active(1); }
        if (id != host_id) {                           // the host's game started again: this one can't follow it
            if (!host_gone) { host_gone = 1; toast("THE HOST RESTARTED - LEAVE AND JOIN AGAIN"); }
            return;
        }
        last_h_rx = now;
        if (host_gone) { host_gone = 0; toast("THE HOST IS BACK"); }
        uint32_t f0 = rd32(buf + 6);
        int n = buf[10], v = buf[16], at = 17 + v;
        host_newest = rd32(buf + 12);
        if (v) voice_unpack(buf + 17, v);
        if (buf[11] && 17 + v + buf[11] <= FRAME_SIZE) pose_got(buf + FRAME_SIZE - buf[11], buf[11]);
        uint8_t cur[MAX_BUNDLE], prev[MAX_BUNDLE];
        int have_prev = 0;
        for (int i = 0; i < n; i++) {
            int used, len = decode(buf + at, FRAME_SIZE - at, cur, have_prev ? prev : NULL, &used);
            if (len < 0) { host_log("online: a bad datagram from the host\n"); break; }
            at += used;
            uint32_t f = f0 + (uint32_t)i;
            if (f == ack + 1) {
                wr32(cur, f);
                net_lock();
                net_push_bundles(cur, len);
                net_unlock();
                ack = f;
            } else if (f > ack + 1) break;             // a gap: the host sends it again
            memcpy(prev, cur, (size_t)len); have_prev = 1;
        }
    }
}
static void session_begin(int slot, int players) {
    host_log("online: session begins, slot %d of %d\n", slot, players);
    if (eng_net_my_room() > 0) eng_net_room_request = eng_net_my_room();   // reconnects come back to this room
}
static void session_end(void) { host_log("online: session over\n"); }

// ---------------------------------------------------------------- the game thread's side
static void guest_gun(int flags, int x, int y, int key) {
    lock();
    if ((flags & 1) && !(gun.flags & 1)) gun.ct++;
    if ((flags & 2) && !(gun.flags & 2)) gun.cr++;
    if (key == DIK_SKIP) gun.ck++;
    gun.flags = flags; gun.x = x; gun.y = y;
    unlock();
}
static int bundle_wait(void) {
    SDL_Delay(2);
    return !quitting;
}
static void on_desync(uint32_t frame) {
    lock(); desynced = 1; unlock();
    toast("OUT OF STEP WITH THE HOST (FRAME %u) - LEAVE AND JOIN AGAIN", frame);
}

// ---------------------------------------------------------------- the host's platform, in its game's name
// A room on the lobby server is named "NAME PLATFORM CODE" and a LAN host answers discovery as "NAME PLATFORM": free text
// the server and the beacon only pass along, so 0.4.x apps show it as it is and still join. The code stays the last
// word: 0.4.x (rooms.c's code_from) finds a room's code there, for JOIN BY CODE and QUICK JOIN.
static const char *const platform_words[] = { "QUEST", "FRAME", "ANDROID", "PC", "PCVR", "MAC", "IPHONE", "IPAD", "APPLETV", "VISIONPRO" };
void online_set_platform(const char *word) {
    int k = 0;
    for (int i = 0; word && word[i] && k < (int)sizeof platform - 1; i++)
        if (word[i] >= 'A' && word[i] <= 'Z') platform[k++] = word[i];
        else if (word[i] >= 'a' && word[i] <= 'z') platform[k++] = (char)(word[i] - 32);
    platform[k] = 0;
}
// "NAME PLATFORM CODE" (no code: "NAME PLATFORM") in at most max characters: the name gives way, never the code
static void tagged_name(char *out, size_t n, int max, const char *code_) {
    const int pl = platform[0] ? 1 + (int)strlen(platform) : 0, cl = code_[0] ? 1 + (int)strlen(code_) : 0;
    int room = max - pl - cl;
    if (room < 1) room = 1;
    snprintf(out, n, "%.*s%s%s%s%s", room, name, pl ? " " : "", platform, cl ? " " : "", code_);
}
// the browser's side: a known platform word taken out of a game's name: the word before the code (a room) or the last
// word (code_last 0: a LAN host). "ANNA QUEST ACDE" -> "ANNA ACDE" and QUEST; anything else stays as it is.
static void untag(const char *in, int code_last, char *nm, size_t nn, char *plat, size_t pn) {
    snprintf(nm, nn, "%s", in); plat[0] = 0;
    const char *end = in + strlen(in);
    if (code_last) { const char *sp = strrchr(in, ' '); if (!sp) return; end = sp; }
    const char *w = end;
    while (w > in && w[-1] != ' ') w--;
    if (w == in) return;                               // the only word: the name
    for (size_t i = 0; i < sizeof platform_words / sizeof *platform_words; i++) {
        const size_t len = strlen(platform_words[i]);
        if ((size_t)(end - w) != len || strncmp(w, platform_words[i], len)) continue;
        snprintf(plat, pn, "%s", platform_words[i]);
        snprintf(nm, nn, "%.*s%s", (int)(w - 1 - in), in, end);
        return;
    }
}

// ---------------------------------------------------------------- the network thread
static int game_frame_of(void) { return (int)net_tact(); }
static void connect_now(void) {
    if (role == 1 && lan) {
        tagged_name(eng_net_host_label, sizeof eng_net_host_label, 16, "");   // (the beacon's name: 16 characters)
        if (!eng_net_hosting() && !eng_net_host_start()) { say("CANNOT HOST: %s", eng_net_note()); return; }
        if (!eng_net_connected()) eng_net_connect();
        return;
    }
    if (!eng_net_set_server(conn)) { say("CANNOT FIND %s", conn); return; }
    if (role == 1 && eng_net_room_request <= 0) {
        eng_net_room_request = -1;
        tagged_name(eng_net_room_request_name, sizeof eng_net_room_request_name, 24, code);   // (HELLO sends 24 characters)
    }
    if (role == 2) eng_net_room_request = room_id;
    eng_net_connect();
}
static void find_room(const char *want, int quick) {
    if (!net_rooms_query(server, ONLINE_VER)) { lock(); st_finding = 0; st_failed = 1; unlock(); say("CANNOT FIND THE SERVER %s", server); return; }
    uint32_t t0 = now_ms();
    while (!quitting && now_ms() - t0 < 3500 && !net_rooms_answered()) { net_rooms_poll(); SDL_Delay(10); }
    lock(); st_finding = 0; unlock();
    if (!net_rooms_answered()) { lock(); st_failed = 1; unlock(); say("NO ANSWER FROM %s", server); return; }
    for (int i = 0; i < net_rooms_count(); i++) {
        const net_room *r = net_rooms_get(i);
        if (quick ? (r->code[0] && r->players < r->max_players) : !strcmp(r->code, want)) {
            if (r->version != ONLINE_VER) { lock(); st_failed = 1; unlock(); say("THAT ROOM RUNS ANOTHER VERSION"); return; }
            if (r->players >= r->max_players) { lock(); st_failed = 1; unlock(); say("ROOM %s IS FULL", r->code); return; }
            write_session(2, 0, server, r->id, r->code);
            restart();
            return;
        }
    }
    lock(); st_failed = 1; unlock();
    say(quick ? "NOBODY IS WAITING ON %s" : "NO ROOM %s ON THE SERVER", quick ? server : want);
}
// the browser's list: the server's rooms of this game, then the hosts found on the local network
static online_game games[40]; static int ngames; static int game_room[40]; static char game_addr[40][64];
static void list_games(void) {
    online_game g[40]; int room[40]; char addr[40][64]; int n = 0;
    for (int i = 0; i < net_rooms_count() && n < 32; i++, n++) {
        const net_room *r = net_rooms_get(i);
        memset(&g[n], 0, sizeof g[n]);
        untag(r->name, r->code[0] != 0, g[n].name, sizeof g[n].name, g[n].platform, sizeof g[n].platform);
        g[n].players = r->players; g[n].max_players = r->max_players;
        g[n].status = r->version != ONLINE_VER ? OG_OLD : r->players >= r->max_players ? OG_FULL : OG_JOIN;
        snprintf(g[n].detail, sizeof g[n].detail, "%d / %d PLAYERS%s", r->players, r->max_players, r->state ? " - PLAYING" : "");
        room[n] = r->id; addr[n][0] = 0;
    }
    for (int i = 0; i < eng_net_found_count() && n < 40; i++) {
        char label[64], a[64];
        if (!eng_net_found(i, label, sizeof label, a, sizeof a)) continue;
        memset(&g[n], 0, sizeof g[n]);
        // the label is "NAME  ADDRESS  N player(s)": the host's name (its platform taken out), then the rest as the detail
        const char *host = eng_net_found_name(i);
        const size_t hl = strlen(host);
        untag(host, 0, g[n].name, sizeof g[n].name, g[n].platform, sizeof g[n].platform);
        if (!strncmp(label, host, hl)) { const char *rest = label + hl; while (*rest == ' ') rest++; snprintf(g[n].detail, sizeof g[n].detail, "%s", rest); }
        else { snprintf(g[n].name, sizeof g[n].name, "%s", label); snprintf(g[n].detail, sizeof g[n].detail, "%s", a); }
        g[n].lan = 1; g[n].max_players = 2; g[n].status = OG_JOIN;
        room[n] = 0; snprintf(addr[n], sizeof addr[n], "%s", a);
        n++;
    }
    {   // PORT_ONLINE_LOG=1: the browser's rows, when they change (tests)
        static int on = -1; static uint32_t last;
        if (on < 0) on = getenv("PORT_ONLINE_LOG") != NULL;
        uint32_t h = 2166136261u;
        for (int i = 0; on && i < n; i++)
            for (const uint8_t *p = (const uint8_t *)&g[i], *e = p + sizeof g[i]; p < e; p++) h = (h ^ *p) * 16777619u;
        if (on && h != last) {
            last = h;
            for (int i = 0; i < n; i++)
                host_log("online: browser row %d: [%s] %s | %s | %s\n", i, g[i].platform[0] ? g[i].platform : "-", g[i].name, g[i].detail,
                         g[i].status == OG_JOIN ? "JOIN" : g[i].status == OG_FULL ? "FULL" : g[i].status == OG_OLD ? "OLD" : "PLAYING");
        }
    }
    lock();
    memcpy(games, g, sizeof g); memcpy(game_room, room, sizeof room); memcpy(game_addr, addr, sizeof addr); ngames = n;
    unlock();
}
static int thread_main(void *arg) {
    (void)arg;
    uint32_t last_try = 0, gone_said = 0;
    while (!quitting) {
        uint32_t now = now_ms();
        int r; char c[8]; int idx;
        lock(); r = rq; rq = RQ_NONE; snprintf(c, sizeof c, "%s", rq_code); idx = rq_index; unlock();
        if (r == RQ_JOIN_CODE || r == RQ_QUICK) find_room(c, r == RQ_QUICK);
        if (renamed) { char nm[NAME_LEN + 1]; lock(); renamed = 0; snprintf(nm, sizeof nm, "%s", name); unlock(); eng_net_set_name(nm); }
        if (r == RQ_JOIN_GAME) {
            lock();
            int ok = idx >= 0 && idx < ngames && games[idx].status == OG_JOIN, l = ok && games[idx].lan, room = ok ? game_room[idx] : 0;
            char a[64], cd[8] = "";
            snprintf(a, sizeof a, "%s", ok ? game_addr[idx] : "");
            if (ok && !l) { const char *sp = strrchr(games[idx].name, ' '); if (sp && strlen(sp + 1) == 4) snprintf(cd, sizeof cd, "%s", sp + 1); }
            unlock();
            if (ok) { write_session(2, l, l ? a : server, room, cd); restart(); }
        }
        if (server_changed) {                          // (online_set_server) the old server's socket and rooms go
            lock(); server_changed = 0; ngames = 0; unlock();
            net_rooms_close();
            browse_t = 0;
        }
        if (browsing) {
            if (r == RQ_REFRESH || !browse_t || now - browse_t > 4000) {
                browse_t = now;
                char srv[sizeof server];
                lock(); snprintf(srv, sizeof srv, "%s", server); unlock();
                net_rooms_query(srv, ONLINE_VER);
                if (!eng_net_discovering()) eng_net_discover();
            }
            net_rooms_poll();
            list_games();
        }
        if (role) {
            int s = eng_net_state();
            if (s == 0 && (!last_try || now - last_try > 2500)) { last_try = now; connect_now(); }
            eng_net_poll();
            if (role == 1) {   // a save state going out: more pieces a round (HOTD_SNAP_BURST: tests)
                static int burst = -1;
                if (burst < 0) { const char *e = getenv("HOTD_SNAP_BURST"); burst = e ? atoi(e) : 0; }
                for (int k = 0; k < burst && snap_sending(); k++) eng_net_send_more();
            }
            // the partner left mid-session: a running session turns joins away, so the room opens again (back in its
            // lobby, where a partner, or the same one back, starts a new session with us)
            {
                static uint32_t alone_since;
                if (eng_net_session_active() && eng_net_roster_count() < 2) {
                    if (!alone_since) alone_since = now;
                    else if (now - alone_since > 1200) {
                        alone_since = 0;
                        host_log("online: alone in the session: reopening the room\n");
                        if (role == 1 && lan) { eng_net_host_stop(); eng_net_host_start(); }
                        else { eng_net_disconnect(); connect_now(); }
                        last_try = now;
                    }
                } else alone_since = 0;
            }
            // the partner's comings and goings
            if (role == 1 && guest_in && (int32_t)(now_ms() - last_g_rx) > GONE_MS) {
                net_lock(); net_host_leave(1); net_unlock();
                guest_in = 0; guest_id = 0;
                SDL_LockMutex(hmu); snap_state = 0; free(snap); snap = NULL; SDL_UnlockMutex(hmu);
                lock(); linked_view = 0; unlock();
                toast("%s LEFT - YOU PLAY ON", partner[0] ? partner : "P2");
                voice_set_active(0);
            }
            if (role == 2 && host_id && !host_gone && (int32_t)(now_ms() - last_h_rx) > GONE_MS) {
                host_gone = 1; gone_said = now;
                toast("THE HOST IS NOT ANSWERING - WAITING");
            }
            if (role == 2 && host_gone && now - gone_said > 20000 && gone_said) { gone_said = now; toast("STILL NO HOST - LEAVE ONLINE GAME ON THE ONLINE TAB"); }
        } else {
            eng_net_poll();                            // the LAN search, while browsing
        }
        {   // PORT_ONLINE_LOG=1: the link every 2 s
            static int on = -1; static uint32_t t;
            if (on < 0) on = getenv("PORT_ONLINE_LOG") != NULL;
            if (on && role && now - t > 2000) {
                t = now;
                host_log("online: state %d, game frame %u, %s %u, tx %u rx %u, ping %d\n", eng_net_state(), net_tact(),
                         role == 1 ? "history / guest has / cursor" : "received", role == 1 ? hn : ack, n_tx, n_rx, eng_net_ping_ms());
                if (role == 1) host_log("online:   guest has %u, cursor %u\n", gack, cursor);
                if (role == 1 && snap_state) host_log("online:   save state %d: sent to %u, guest has %u of %u (%u pieces, %u again)\n",
                                                      snap_state, snap_cursor, snap_acked, snap_len, snap_pieces, snap_rewinds);
                if (role == 2 && !gsnap_done) host_log("online:   save state: %u of %u\n", gsnap_got, gsnap_len);
            }
        }
        // 2 ms between rounds; the host's backlog for a guest catching up goes out as fast as the server takes it
        SDL_Delay(2);
    }
    return 0;
}

// ---------------------------------------------------------------- start-up
int online_boot(const char *files_dir) {
    if (booted) return role != 0;
    booted = 1;
    snprintf(files, sizeof files, "%s", files_dir ? files_dir : "");
    if (!mu) mu = SDL_CreateMutex();
    if (!hmu) hmu = SDL_CreateMutex();
    load_cfg();
    eng_net_init(&game);
    eng_net_set_name(name);
    const char *e = getenv("PORT_ONLINE"), *es = getenv("PORT_ONLINE_SERVER");
    if (es && *es) snprintf(server, sizeof server, "%s", es);
    if (e && *e) {                                     // tests: host | host-lan | join:<code> | join-ip:<address> | join-room:<id>
        snprintf(conn, sizeof conn, "%s", server);
        if (!strcmp(e, "host")) { role = 1; net_room_code_make(code); }
        else if (!strcmp(e, "host-lan")) { role = 1; lan = 1; }
        else if (!strncmp(e, "join-ip:", 8)) { role = 2; lan = 1; snprintf(conn, sizeof conn, "%s", e + 8); }
        else if (!strncmp(e, "join-room:", 10)) { role = 2; room_id = atoi(e + 10); }
        else if (!strncmp(e, "join:", 5)) {
            // a code: find its room first (the menu does this before its restart)
            snprintf(code, sizeof code, "%s", e + 5);
            if (net_rooms_query(server, ONLINE_VER)) {
                uint32_t t0 = now_ms();
                while (now_ms() - t0 < 4000 && !net_rooms_answered()) { net_rooms_poll(); SDL_Delay(10); }
                for (int i = 0; i < net_rooms_count(); i++) if (!strcmp(net_rooms_get(i)->code, code)) room_id = net_rooms_get(i)->id;
                net_rooms_close();
            }
            role = room_id ? 2 : 0;
            if (!role) host_log("online: no room %s on %s\n", code, server);
        }
    } else {
        read_session();
    }
    if (!role) return 0;
    my_id = run_id();
    net_configure(role == 1 ? 1 : 2, role == 1 ? 0 : 1);
    net_bundle_out = host_bundle;
    net_bundle_wait = bundle_wait;
    net_guest_out = guest_gun;
    net_desync_out = on_desync;
    net_snapshot_out = snapshot_out;
    eng_net_set_autostart(1);                          // in a room together: the session starts by itself
    voice_set_mode(voice_mode); voice_set_muted(muted); voice_set_volume(volume);
    host_log("online: %s %s%s%s (room %d, code %s)\n", role == 1 ? "hosting" : "joining", lan ? "on the local network " : "on ",
             lan && role == 1 ? "" : conn, "", room_id, code[0] ? code : "-");
    if (role == 1) toast(lan ? "HOSTING ON YOUR NETWORK - P2 JOINS ANY TIME" : "ROOM %s - FRIENDS JOIN ANY TIME", code);
    else toast("JOINING THE HOST'S GAME...");
    return 1;
}
static void start_thread(void) {
    if (thread) return;
    if (!mu) mu = SDL_CreateMutex();
    if (!hmu) hmu = SDL_CreateMutex();
    if (!booted) online_boot(".");
    thread = SDL_CreateThread(thread_main, "online", NULL);
}
void online_start(void) { if (role) start_thread(); }
bool online_session(void) { return role != 0; }
void online_quit(void) {
    quitting = 1;
    if (thread) { SDL_WaitThread(thread, NULL); thread = NULL; }
    if (role) eng_net_disconnect();
}

// ---------------------------------------------------------------- the menu
void online_get_view(online_view *v) {
    memset(v, 0, sizeof *v);
    lock();
    v->host = role == 1; v->lan = lan != 0;
    snprintf(v->name, sizeof v->name, "%s", name);
    snprintf(v->server, sizeof v->server, "%s", server);
    snprintf(v->code, sizeof v->code, "%s", code);
    snprintf(v->partner, sizeof v->partner, "%s", partner);
    snprintf(v->message, sizeof v->message, "%s", message);
    snprintf(v->join_ip, sizeof v->join_ip, "%s", join_ip);
    v->desync = desynced != 0;
    int linked = linked_view, finding = st_finding, failed = st_failed;
    unlock();
    v->ping_ms = role ? eng_net_ping_ms() : -1;
    if (!role) v->state = finding ? OV_FINDING : failed ? OV_FAILED : OV_OFFLINE;
    else if (role == 1) v->state = eng_net_state() < 2 && !linked ? OV_CONNECTING : linked ? OV_LINKED : OV_WAITING;
    else {
        uint32_t newest = host_newest, at = (uint32_t)game_frame_of();
        if (!host_id) v->state = OV_CONNECTING;
        else if (host_gone) v->state = OV_PARTNER_GONE;
        else if (newest > at + 120) { v->state = OV_CATCHING_UP; v->catch_up_pct = newest ? (int)((uint64_t)at * 100 / newest) : 0; }
        else v->state = OV_LINKED;
    }
    v->browse_answered = net_rooms_answered(); v->browse_busy = net_rooms_busy(); v->discovering = eng_net_discovering();
    snprintf(v->server_name, sizeof v->server_name, "%s", net_rooms_server_name());
    v->server_rooms = net_rooms_count(); v->server_players = net_rooms_server_players();
    voice_status_t vs;
    voice_get_status(&vs);
    v->voice_mode = voice_mode; v->muted = muted != 0; v->volume = volume;
    v->mic_denied = vs.mic_denied != 0; v->talking = vs.talking != 0; v->partner_talking = vs.partner_talking != 0;
    v->level = vs.level; v->partner_level = vs.partner_level;
    v->address_count = 0;
#ifndef _WIN32
    if (role == 1 && lan) {                            // where a partner joins by address
        struct ifaddrs *ifs = NULL;
        if (!getifaddrs(&ifs)) {
            for (struct ifaddrs *i = ifs; i && v->address_count < 3; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
                char a[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a);
                snprintf(v->address[v->address_count++], sizeof v->address[0], "%s", a);
            }
            freeifaddrs(ifs);
        }
    }
#endif
}
int online_games(online_game *out, int max) {
    lock();
    int n = ngames < max ? ngames : max;
    memcpy(out, games, (size_t)n * sizeof *out);
    unlock();
    return n;
}
void online_browse(bool open) {
    start_thread();
    browsing = open;
    browse_t = 0;
    if (!open) { lock(); ngames = 0; unlock(); }
}
void online_refresh(void) { lock(); rq = RQ_REFRESH; unlock(); }
void online_join_game(int index) { start_thread(); lock(); rq = RQ_JOIN_GAME; rq_index = index; message[0] = 0; unlock(); }
void online_host_room(void) {
    if (role) return;
    char c[5];
    net_room_code_make(c);
    write_session(1, 0, server, 0, c);
    restart();
}
void online_host_lan(void) {
    if (role) return;
    write_session(1, 1, "", 0, "");
    restart();
}
void online_join_code(const char *c) {
    if (role) return;
    start_thread();
    lock();
    snprintf(rq_code, sizeof rq_code, "%s", c);
    rq = RQ_JOIN_CODE; st_finding = 1; st_failed = 0; message[0] = 0;
    unlock();
}
void online_quick(void) { if (role) return; start_thread(); lock(); rq = RQ_QUICK; st_finding = 1; st_failed = 0; message[0] = 0; unlock(); }
void online_set_join_ip(const char *a) { lock(); snprintf(join_ip, sizeof join_ip, "%s", a); unlock(); save_cfg(); }
void online_join_ip(void) {
    if (role || !join_ip[0]) return;
    char a[64];
    snprintf(a, sizeof a, "%s", join_ip);
    write_session(2, 1, a, 0, "");
    restart();
}
void online_leave(void) {
    if (!role) return;
    char p[600];
    path_of(p, sizeof p, "online-session.cfg");
    remove(p);
    online_quit();                                     // the partner hears us go (LEAVE)
    restart();
}
const char *online_default_server(void) { return DEFAULT_SERVER; }
// host[:port]: a hostname (letters, digits, hyphens, dots; lowercased) or an IPv4 address (normalised), no scheme or path
const char *online_set_server(const char *address) {
    if (role) return "Leave the online game first.";
    if (!address || !address[0]) return "Enter a hostname or IPv4 address.";
    if (strlen(address) >= sizeof server) return "The server address must be at most 63 characters.";
    char next[sizeof server];
    snprintf(next, sizeof next, "%s", address);
    char *colon = strchr(next, ':');
    unsigned port = 0;
    if (colon) {
        const char *p = colon + 1;
        if (!*p) return "Enter a port from 1 to 65535 after the colon.";
        for (; *p; p++) {
            if (*p < '0' || *p > '9') return "Use host:port, without https:// or a path.";
            port = port * 10 + (unsigned)(*p - '0');
            if (port > 65535) return "The port must be from 1 to 65535.";
        }
        if (!port) return "The port must be from 1 to 65535.";
        *colon = 0;
    }
    const size_t hostlen = strlen(next);
    if (!hostlen) return "Enter a hostname or IPv4 address.";
    int numeric = 1, label = 0;
    for (size_t i = 0; i < hostlen; i++) {
        char c = next[i];
        if (c >= 'A' && c <= 'Z') c = next[i] = (char)(c - 'A' + 'a');
        if (c == '.') {
            if (!label || next[i - 1] == '-') return "Use a valid hostname or IPv4 address.";
            label = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') || (!label && c == '-'))
            return "Use letters, digits, dots and hyphens for the host.";
        if (c < '0' || c > '9') numeric = 0;
        label++;
    }
    if (next[hostlen - 1] == '-') return "A hostname cannot end with a hyphen.";
    if (numeric) {
        unsigned o[4]; int parts = 0; const char *p = next;
        while (*p) {
            unsigned value = 0; int digits = 0;
            while (*p >= '0' && *p <= '9') {
                value = value * 10 + (unsigned)(*p++ - '0'); digits++;
                if (value > 255 || digits > 3) return "IPv4 addresses need four numbers from 0 to 255.";
            }
            if (!digits || parts >= 4 || (*p && (*p != '.' || !p[1]))) return "Enter a complete IPv4 address, such as 192.168.1.20.";
            o[parts++] = value;
            if (*p) p++;
        }
        if (parts != 4) return "Enter a complete IPv4 address, such as 192.168.1.20.";
        snprintf(next, sizeof next, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
    }
    if (colon) { const size_t n = strlen(next); snprintf(next + n, sizeof next - n, ":%u", port); }
    lock();
    if (st_finding) { unlock(); return "Wait until joining finishes."; }
    if (!strcmp(next, server)) { unlock(); return NULL; }
    char previous[sizeof server];
    memcpy(previous, server, sizeof server);
    snprintf(server, sizeof server, "%s", next);
    unlock();
    if (!save_cfg()) {
        lock(); memcpy(server, previous, sizeof server); unlock();
        return "Could not save online.cfg.";
    }
    lock(); server_changed = 1; message[0] = 0; st_failed = 0; unlock();
    toast("LOBBY SERVER: %s", next);
    return NULL;
}
void online_set_name(const char *n) {
    char b[NAME_LEN + 1]; int k = 0;
    for (int i = 0; n[i] && k < NAME_LEN; i++) {
        char ch = n[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') b[k++] = ch;
        else if (ch >= 'a' && ch <= 'z') b[k++] = (char)(ch - 32);
    }
    b[k] = 0;
    if (!k) return;
    lock(); snprintf(name, sizeof name, "%s", b); renamed = 1; unlock();
    save_cfg();
}
void online_set_voice_mode(int m) { voice_mode = m < 0 ? 0 : m > 2 ? 2 : m; voice_set_mode(voice_mode); save_cfg(); }
// the VR menu's Voice chat volume (its setting; also before online_boot, which then keeps it)
void online_set_volume(int p) { volume = p < 0 ? 0 : p > 200 ? 200 : p; volume_set = 1; voice_set_volume(volume); if (booted) save_cfg(); }
void online_toggle_mute(void) { muted = !muted; voice_set_muted(muted); save_cfg(); toast(muted ? "MICROPHONE MUTED" : "MICROPHONE ON"); }
void online_talk_key(bool held) { voice_talk_key(held); }
bool online_push_to_talk(void) { return role && voice_mode == 2; }
int online_lag_ms(void) { int p = eng_net_ping_ms(); return role == 2 ? (p > 0 ? 2 * p : 100) + 40 : 0; }
bool online_toast(char *text, size_t n, unsigned *serial) {
    lock();
    int on = toast_serial && (int32_t)(toast_until - now_ms()) > 0;
    if (on) snprintf(text, n, "%s", toast_text);
    if (serial) *serial = toast_serial;
    unlock();
    return on;
}
