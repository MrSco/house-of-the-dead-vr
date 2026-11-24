// iCloud sync for the Apple apps (src/core/hotd_icloud.h, docs/icloud-sync.md): iPhone and iPad, Apple TV and Vision
// Pro share one bundle id, so one key-value store and one CloudKit container reach all three. GoldenEye VR's
// port/ios/ios_icloud.mm, with what its first week taught built in from the start (the joining rule, files.txt per
// CloudKit database, a device's own ROM set kept on its first sync, uploads on Wi-Fi only).
//
// Settings and saves: NSUbiquitousKeyValueStore.
//   "opt1" / "opt1@<family>"   options.cfg's and vr-options.cfg's keys (shared / this kind of device's)
//   "onl1" / "onl1@<family>"   online.cfg's
//   "save1"                    save/'s files, whole: { f: { name: bytes }, t: seconds }
// Each setting is merged three ways against the copy last synced (.hotd-icloud/last-*): what changed here since then
// goes up, what changed elsewhere comes down. A device joining takes the cloud's values. A change on another device
// while this one runs applies at its next start (the game has read its files).
//
// The ROM set: CloudKit's private database, zone "HotdFiles". One item, "rom" (arcade/hotd.zip or hotd.7z, whichever
// this device plays): a HotdItem record (its file name, stamp, size, SHA-256, chunk count, generation) and its bytes
// as HotdChunk records of 32 MB, one asset each. Chunks first, the item last, so a reader never sees half an upload;
// a new upload is a new generation and the old chunks go after. A download is checked against its SHA-256 before it
// is used. One that would replace a set already here (the game may be reading it) waits in .hotd-icloud/ready/ and is
// put in place at the next start. Nothing is deleted from iCloud when a device removes its set.
#import <Foundation/Foundation.h>
#import <CloudKit/CloudKit.h>
#import <UIKit/UIKit.h>
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "platform/platform.h"
}
#include "hotd_icloud.h"

#if TARGET_OS_VISION
static const char *const kFamily = "vision";
#elif TARGET_OS_TV
static const char *const kFamily = "tv";
#else
static const char *const kFamily = "ios";
#endif

static const char *const kStateDir = ".hotd-icloud";
static const size_t kChunkBytes = 32u << 20;
static NSString *const kZoneName = @"HotdFiles";
static NSString *const kItemType = @"HotdItem";
static NSString *const kChunkType = @"HotdChunk";
static NSString *const kJoinedType = @"HotdJoined";
// this device takes part in the key-value sync (the app's defaults: on Apple TV they outlive tvOS emptying the caches)
static NSString *const kJoinedDefault = @"hotd.icloud.joined1";
static NSString *const kRomItem = @"rom";

namespace {

std::mutex g_lock;                    // the options, the status, the ROM state
bool g_launched = false;
bool g_workerStarted = false;
bool g_launchDone = false;
bool g_joined = false;
bool g_sync = true;
std::string g_root;                   // the files folder, absolute, no trailing slash
std::string g_status;
int g_rom = HOTD_CLOUD_ROM_NONE;      // HOTD_CLOUD_ROM_*
float g_romDone = 0;

std::mutex g_kvLock;                  // the key-value merges: the launch and the watch

std::mutex g_workLock;
std::condition_variable g_workWake;
bool g_workKick = false;

dispatch_queue_t g_watchQueue;
dispatch_source_t g_watchTimer;

void cloudLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cloudLog(const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    plat_log(PLAT_LOG_INFO, "icloud: %s", line);
}

void setStatus(const std::string &s)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_status = s;
}

void setRom(int state, float done)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_rom = state;
    g_romDone = done < 0 ? 0 : done > 1 ? 1 : done;
}

/* ------------------------------------------------------------------ files */

std::string statePath(const std::string &name) { return g_root + "/" + kStateDir + "/" + name; }

bool readFile(const std::string &path, std::string *out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

bool writeFileAtomic(const std::string &path, const std::string &data)
{
    const std::string tmp = path + ".icloud-tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

void makeDirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    }
}

void removeTree(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = opendir(path.c_str())) {
            while (struct dirent *e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                removeTree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string hexOf(const unsigned char *p, size_t n)
{
    static const char *h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += h[p[i] >> 4];
        s += h[p[i] & 15];
    }
    return s;
}

std::string sha256Text(const std::string &data)
{
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.data(), (CC_LONG)data.size(), md);
    return hexOf(md, sizeof(md));
}

std::string sha256File(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    std::vector<char> buf(1 << 20);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) CC_SHA256_Update(&c, buf.data(), (CC_LONG)n);
    fclose(f);
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &c);
    return hexOf(md, sizeof(md));
}

/* -------------------------------------------------------- settings (KVS) */

/* The keys every device shares; every other key belongs to one kind of device. A new key stays on its device unless
 * it is named here: a phone's speaker and a living room's TV want their own game volume (GoldenEye VR, 2026-11-24),
 * and the headset's own rows (the gun's fit, the view, the cabinet's place) mean nothing on a screen. */
const std::set<std::string> &sharedOptionKeys()
{
    static const std::set<std::string> keys = {
        "rumble", "pad_trigger_feedback", "music_volume", "effects_volume", "voice_chat_volume", "crash_reports",
    };
    return keys;
}
/* online.cfg: the name and the lobby server follow the player; the LAN address, the mic, the voice mode's device */
const std::set<std::string> &sharedOnlineKeys()
{
    static const std::set<std::string> keys = { "name", "server", "voice_volume" };
    return keys;
}

struct CfgFile {
    std::string name;      // in the files folder
    const char *kvBase;    // the store's name for it
    bool online;           // online.cfg (its own shared keys)
};
/* "1": the store's first names; a change in the rules gets new ones and leaves the old alone (GoldenEye VR's "2") */
CfgFile g_opt = { "options.cfg", "opt1", false };
CfgFile g_onl = { "online.cfg", "onl1", true };
NSString *const kSaveKey = @"save1";

bool isLocalKey(const CfgFile &f, const std::string &k)
{
    return f.online ? !sharedOnlineKeys().count(k) : !sharedOptionKeys().count(k);
}

/* key=value lines (quest_options.c, online.c): the value is what follows the first '=' */
std::map<std::string, std::string> cfgMap(const std::string &text)
{
    std::map<std::string, std::string> m;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0) continue;
        m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

/* the file's text with these keys set: lines changed in place, new keys added at the end */
std::string cfgApply(const std::string &text, const std::map<std::string, std::string> &set)
{
    std::string out;
    std::set<std::string> done;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0 && line[0] != '#') {
            const std::string k = line.substr(0, eq);
            auto it = set.find(k);
            if (it != set.end()) {
                line = k + "=" + it->second;
                done.insert(k);
            }
        }
        out += line + "\n";
    }
    for (const auto &kv : set) {
        if (!done.count(kv.first)) out += kv.first + "=" + kv.second + "\n";
    }
    return out;
}

NSUbiquitousKeyValueStore *kvs() { return [NSUbiquitousKeyValueStore defaultStore]; }

NSString *ns(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

std::map<std::string, std::string> kvDict(NSString *key)
{
    std::map<std::string, std::string> m;
    NSDictionary *d = [kvs() dictionaryForKey:key];
    for (id k in d) {
        id v = d[k];
        if ([k isKindOfClass:[NSString class]] && [v isKindOfClass:[NSString class]]) {
            m[[(NSString *)k UTF8String]] = [(NSString *)v UTF8String];
        }
    }
    return m;
}

void kvSetDict(NSString *key, const std::map<std::string, std::string> &m)
{
    NSMutableDictionary *d = [NSMutableDictionary dictionaryWithCapacity:m.size()];
    for (const auto &kv : m) d[ns(kv.first)] = ns(kv.second);
    [kvs() setDictionary:d forKey:key];
}

NSString *sharedKey(const CfgFile &f) { return [NSString stringWithUTF8String:f.kvBase]; }
NSString *familyKey(const CfgFile &f) { return [NSString stringWithFormat:@"%s@%s", f.kvBase, kFamily]; }

/* Keys changed in `now` against `before` go up (shared or this family's). */
void cfgPush(const CfgFile &f, const std::map<std::string, std::string> &before, const std::map<std::string, std::string> &now)
{
    std::map<std::string, std::string> shared = kvDict(sharedKey(f)), mine = kvDict(familyKey(f));
    bool sharedChanged = false, mineChanged = false;
    for (const auto &kv : now) {
        auto b = before.find(kv.first);
        if (b != before.end() && b->second == kv.second) continue;
        const bool local = isLocalKey(f, kv.first);
        std::map<std::string, std::string> &dst = local ? mine : shared;
        auto cur = dst.find(kv.first);
        if (cur != dst.end() && cur->second == kv.second) continue;
        dst[kv.first] = kv.second;
        (local ? mineChanged : sharedChanged) = true;
    }
    if (sharedChanged) kvSetDict(sharedKey(f), shared);
    if (mineChanged) kvSetDict(familyKey(f), mine);
}

/* What the file gets from the cloud at the start. In step (joined, with the copy last synced): a key the cloud changed
 * and this device didn't. Joining (or no copy last synced: tvOS emptied the caches): every value the cloud has. */
std::map<std::string, std::string> cfgTake(const std::map<std::string, std::string> &local, const std::map<std::string, std::string> &last,
                                           const std::map<std::string, std::string> &remote, bool inStep)
{
    std::map<std::string, std::string> take;
    for (const auto &kv : remote) {
        auto l = local.find(kv.first);
        if (l != local.end() && l->second == kv.second) continue;
        if (inStep && l != local.end()) {
            auto b = last.find(kv.first);
            if (b == last.end() || b->second != l->second) continue;   // changed here since the last sync: it goes up
        }
        take[kv.first] = kv.second;
    }
    return take;
}

void cfgLaunchMerge(const CfgFile &f, bool joined)
{
    const std::string path = g_root + "/" + f.name;
    const std::string lastPath = statePath("last-" + f.name);
    std::string text, lastText;
    const bool haveLocal = readFile(path, &text);
    const bool inStep = joined && readFile(lastPath, &lastText);
    const std::map<std::string, std::string> local = cfgMap(text), last = cfgMap(lastText);
    std::map<std::string, std::string> remote = kvDict(sharedKey(f));
    for (const auto &kv : kvDict(familyKey(f))) remote[kv.first] = kv.second;
    const std::map<std::string, std::string> take = cfgTake(local, last, remote, inStep);
    std::string merged = text;
    if (!take.empty()) {
        merged = cfgApply(text, take);
        if (!writeFileAtomic(path, merged)) {
            cloudLog("could not write %s: %s", f.name.c_str(), strerror(errno));
            return;
        }
        cloudLog("%s: %zu value%s from iCloud", f.name.c_str(), take.size(), take.size() == 1 ? "" : "s");
    }
    // in step: what changed here since the last sync; else what the cloud lacks or holds otherwise
    cfgPush(f, inStep ? last : remote, cfgMap(merged));
    if (haveLocal || !take.empty()) writeFileAtomic(lastPath, merged);
}

/* the watch: the file changed since the copy last synced */
void cfgWatch(const CfgFile &f)
{
    const std::string path = g_root + "/" + f.name;
    const std::string lastPath = statePath("last-" + f.name);
    std::string text, lastText;
    if (!readFile(path, &text)) return;
    readFile(lastPath, &lastText);
    if (text == lastText) return;
    cfgPush(f, cfgMap(lastText), cfgMap(text));
    writeFileAtomic(lastPath, text);
    cloudLog("%s to iCloud", f.name.c_str());
}

/* ------------------------------------------------------------ the saves */

/* save/: the game's settings EEPROM and backup RAM (its high scores), written together (tools/common/nvram.h): one
 * set, never merged (GoldenEye VR's owner: choose the right one, don't combine them). A set changed on one side only
 * since the last sync moves that way; changed on both, the one written last wins, by the files' own time, carried
 * with it and put back on the files when it comes down. A device joining takes the cloud's (a new install's fresh
 * scores never go over a played one's). A set that is replaced is kept in
 * .hotd-icloud/backups/, the last five. online-save/ (online play's fresh saves) never moves. */
struct SaveSet {
    std::map<std::string, std::string> files;
    long long t = 0;
    bool busy = false;   // a file being written (the game writes in place): looked at again later
};

bool saveName(const char *n)
{
    if (n[0] == '.') return false;
    const size_t l = strlen(n);
    return !strstr(n, ".tmp") && !strstr(n, "icloud") && l > 0;
}

SaveSet localSaves(double settle)
{
    SaveSet s;
    const std::string dir = g_root + "/save";
    DIR *d = opendir(dir.c_str());
    if (!d) return s;
    const time_t now = time(nullptr);
    while (struct dirent *e = readdir(d)) {
        if (!saveName(e->d_name)) continue;
        const std::string p = dir + "/" + e->d_name;
        struct stat a, b;
        if (stat(p.c_str(), &a) != 0 || !S_ISREG(a.st_mode) || a.st_size > 512 * 1024) continue;
        if (difftime(now, a.st_mtime) < settle) s.busy = true;
        std::string data;
        if (!readFile(p, &data)) continue;
        if (stat(p.c_str(), &b) != 0 || b.st_mtime != a.st_mtime || b.st_size != a.st_size || (off_t)data.size() != a.st_size) s.busy = true;
        s.files[e->d_name] = data;
        if ((long long)a.st_mtime > s.t) s.t = a.st_mtime;
    }
    closedir(d);
    return s;
}

SaveSet remoteSaves()
{
    SaveSet s;
    NSDictionary *r = [kvs() dictionaryForKey:kSaveKey];
    NSDictionary *f = [r[@"f"] isKindOfClass:[NSDictionary class]] ? r[@"f"] : nil;
    for (id k in f) {
        id v = f[k];
        if (![k isKindOfClass:[NSString class]] || ![v isKindOfClass:[NSData class]]) continue;
        const std::string n = [(NSString *)k UTF8String];
        if (n.find('/') != std::string::npos || !saveName(n.c_str())) continue;
        s.files[n] = std::string((const char *)[(NSData *)v bytes], [(NSData *)v length]);
    }
    s.t = [r[@"t"] respondsToSelector:@selector(longLongValue)] ? [r[@"t"] longLongValue] : 0;
    return s;
}

std::string saveSum(const SaveSet &s)
{
    std::string all;
    for (const auto &kv : s.files) all += kv.first + "\n" + std::to_string(kv.second.size()) + "\n" + kv.second;
    return sha256Text(all);
}

/* the set as last synced, by its sum: in the app's defaults (on Apple TV they outlive tvOS emptying the caches, and
 * tv_store.m puts the saves back with new file times then: their time alone would make them the newest) */
NSString *const kSaveSumDefault = @"hotd.icloud.savesum1";
std::string lastSaveSum()
{
    NSString *s = [[NSUserDefaults standardUserDefaults] stringForKey:kSaveSumDefault];
    return s ? std::string(s.UTF8String) : std::string();
}
void setLastSaveSum(const std::string &sum) { [[NSUserDefaults standardUserDefaults] setObject:ns(sum) forKey:kSaveSumDefault]; }

void savePush(const SaveSet &s)
{
    NSMutableDictionary *f = [NSMutableDictionary dictionary];
    for (const auto &kv : s.files) f[ns(kv.first)] = [NSData dataWithBytes:kv.second.data() length:kv.second.size()];
    [kvs() setDictionary:@{ @"f": f, @"t": @(s.t) } forKey:kSaveKey];
    setLastSaveSum(saveSum(s));
}

void saveBackup(const SaveSet &s)
{
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const std::string dir = statePath("backups"), mine = dir + "/save-" + stamp;
    makeDirs(mine);
    for (const auto &kv : s.files) writeFileAtomic(mine + "/" + kv.first, kv.second);
    std::vector<std::string> old;
    if (DIR *d = opendir(dir.c_str())) {
        while (struct dirent *e = readdir(d)) {
            if (!strncmp(e->d_name, "save-", 5)) old.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + 5 < old.size(); i++) removeTree(dir + "/" + old[i]);
}

enum class SaveMove { None, Up, Down };

/* What the saves do at the start: L, R this device's and the cloud's sets (empty: none); joined: in step before;
 * last: the sum of the set as last synced ("" unknown) */
SaveMove decideSaves(const SaveSet &L, const SaveSet &R, bool joined, const std::string &last)
{
    if (L.files.empty() && R.files.empty()) return SaveMove::None;
    if (R.files.empty()) return SaveMove::Up;
    if (L.files.empty()) return SaveMove::Down;
    if (L.files == R.files) return SaveMove::None;
    if (!joined) return SaveMove::Down;                       // joining: the other devices' set
    if (!last.empty() && saveSum(L) == last) return SaveMove::Down;   // changed in iCloud only
    if (!last.empty() && saveSum(R) == last) return SaveMove::Up;     // changed here only
    return L.t >= R.t ? SaveMove::Up : SaveMove::Down;       // both: the one written last
}

void savesLaunchMerge(bool joined)
{
    const SaveSet L = localSaves(0), R = remoteSaves();
    const SaveMove m = decideSaves(L, R, joined, lastSaveSum());
    if (m == SaveMove::None) {
        if (!L.files.empty()) setLastSaveSum(saveSum(L));
        return;
    }
    if (m == SaveMove::Up) {
        savePush(L);
        cloudLog("saves to iCloud (written %lld, iCloud's %lld)", L.t, R.t);
        return;
    }
    if (!L.files.empty()) saveBackup(L);
    const std::string dir = g_root + "/save";
    makeDirs(dir);
    for (const auto &kv : L.files) {
        if (!R.files.count(kv.first)) remove((dir + "/" + kv.first).c_str());   // (one set: the cloud's)
    }
    for (const auto &kv : R.files) {
        const std::string p = dir + "/" + kv.first;
        if (writeFileAtomic(p, kv.second) && R.t > 0) {
            struct timeval tv[2] = { { (time_t)R.t, 0 }, { (time_t)R.t, 0 } };
            utimes(p.c_str(), tv);
        }
    }
    setLastSaveSum(saveSum(R));
    cloudLog("saves from iCloud (written %lld, this device's %lld%s)", R.t, L.t, L.files.empty() ? "" : ", kept in .hotd-icloud/backups");
}

/* the watch: the game wrote its set (settled for two seconds), and it isn't the one last synced */
void savesWatch()
{
    const SaveSet L = localSaves(2);
    if (L.busy || L.files.empty() || saveSum(L) == lastSaveSum()) return;
    savePush(L);
    cloudLog("saves to iCloud");
}

bool storeHasData()
{
    for (NSString *k in [kvs() dictionaryRepresentation]) {
        if ([k hasPrefix:@"opt1"] || [k hasPrefix:@"onl1"] || [k isEqualToString:kSaveKey]) return true;
    }
    return false;
}

/* ------------------------------------------------------------ the ROM set (CloudKit) */

/* This device's set: arcade/hotd.zip, else arcade/hotd.7z (the game takes hotd.zip first). Its stamp changes when its
 * content may have: the name, the size and the time. */
struct LocalRom {
    std::string name;     // "" none
    std::string stamp;
    long long bytes = 0;
};

LocalRom localRom()
{
    LocalRom r;
    for (const char *n : { "hotd.zip", "hotd.7z" }) {
        struct stat st;
        const std::string p = g_root + "/arcade/" + n;
        if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        r.name = n;
        r.bytes = st.st_size;
        r.stamp = r.name + ":" + std::to_string((long long)st.st_size) + ":" + std::to_string((long long)st.st_mtime);
        break;
    }
    return r;
}

/* its SHA-256, kept by stamp (.hotd-icloud/rom.hash): a set of 60 MB isn't read again each pass */
std::string romHash(const LocalRom &r)
{
    std::string cached;
    if (readFile(statePath("rom.hash"), &cached)) {
        const size_t tab = cached.find('\t');
        if (tab != std::string::npos && cached.substr(0, tab) == r.stamp) return cached.substr(tab + 1);
    }
    const std::string h = sha256File(g_root + "/arcade/" + r.name);
    if (!h.empty()) writeFileAtomic(statePath("rom.hash"), r.stamp + "\t" + h);
    return h;
}

/* A CloudKit call made to wait: the worker thread is ours. Retries what CloudKit says to retry. */
template <typename Start>
NSError *ckWait(Start start)
{
    for (int attempt = 0;; attempt++) {
        __block NSError *err = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        start(^(NSError *e) {
            err = e;
            dispatch_semaphore_signal(done);
        });
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (!err || attempt >= 3) return err;
        const NSInteger c = err.code;
        if (c != CKErrorRequestRateLimited && c != CKErrorZoneBusy && c != CKErrorServiceUnavailable && c != CKErrorNetworkFailure
            && c != CKErrorNetworkUnavailable) {
            return err;
        }
        NSNumber *after = err.userInfo[CKErrorRetryAfterKey];
        const double wait = after ? std::min(after.doubleValue, 60.0) : 3.0 * (attempt + 1);
        [NSThread sleepForTimeInterval:wait];
    }
}

/* cellular: a download (the player waits for it); an upload goes on Wi-Fi only (a set is 30 to 60 MB) */
void configure(CKOperation *op, bool cellular)
{
    op.qualityOfService = NSQualityOfServiceUtility;
    CKOperationConfiguration *c = [[CKOperationConfiguration alloc] init];
    c.allowsCellularAccess = cellular;
    c.timeoutIntervalForResource = 24 * 3600;
    c.qualityOfService = NSQualityOfServiceUtility;
    op.configuration = c;
}

struct Cloud {
    CKContainer *container = nil;
    CKDatabase *db = nil;
    CKRecordZoneID *zone = nil;
};

NSError *ckEnsureZone(const Cloud &c)
{
    return ckWait([&](void (^finish)(NSError *)) {
        CKRecordZone *z = [[CKRecordZone alloc] initWithZoneID:c.zone];
        CKModifyRecordZonesOperation *op = [[CKModifyRecordZonesOperation alloc] initWithRecordZonesToSave:@[ z ] recordZoneIDsToDelete:nil];
        configure(op, true);
        op.modifyRecordZonesCompletionBlock = ^(NSArray *saved, NSArray *deleted, NSError *e) { finish(e); };
        [c.db addOperation:op];
    });
}

NSArray<NSString *> *itemKeys() { return @[ @"name", @"stamp", @"size", @"chunks", @"gen", @"hash" ]; }

struct ChunkRef { std::string name; double age; };   // a chunk's record name and seconds since it was made

/* the item (nil: none in iCloud) and every chunk record */
NSError *ckFetchAll(const Cloud &c, CKRecord **item, std::vector<ChunkRef> *chunks)
{
    *item = nil;
    NSMutableArray<CKRecord *> *got = [NSMutableArray array];
    NSMutableArray<CKRecord *> *parts = [NSMutableArray array];
    NSError *err = ckWait([&](void (^finish)(NSError *)) {
        [got removeAllObjects];
        [parts removeAllObjects];
        CKFetchRecordZoneChangesConfiguration *cfg = [[CKFetchRecordZoneChangesConfiguration alloc] init];
        cfg.desiredKeys = itemKeys();
        CKFetchRecordZoneChangesOperation *op =
            [[CKFetchRecordZoneChangesOperation alloc] initWithRecordZoneIDs:@[ c.zone ] configurationsByRecordZoneID:@{ c.zone: cfg }];
        configure(op, true);
        op.fetchAllChanges = YES;
        op.recordWasChangedBlock = ^(CKRecordID *rid, CKRecord *rec, NSError *e) {
            if (rec && [rec.recordType isEqualToString:kItemType] && [rid.recordName isEqualToString:@"i-rom"]) {
                @synchronized(got) { [got addObject:rec]; }
            } else if (rec && [rec.recordType isEqualToString:kChunkType]) {
                @synchronized(parts) { [parts addObject:rec]; }
            }
        };
        op.fetchRecordZoneChangesCompletionBlock = ^(NSError *e) { finish(e); };
        [c.db addOperation:op];
    });
    if (err) return err;
    *item = got.firstObject;
    for (CKRecord *r in parts) {
        const double age = r.creationDate ? -[r.creationDate timeIntervalSinceNow] : 0;
        chunks->push_back({ r.recordID.recordName.UTF8String, age });
    }
    return nil;
}

/* the item record as it is now: nil when there is none */
NSError *ckFetchItem(const Cloud &c, CKRecord **out)
{
    *out = nil;
    CKRecordID *rid = [[CKRecordID alloc] initWithRecordName:@"i-rom" zoneID:c.zone];
    NSMutableArray<CKRecord *> *box = [NSMutableArray array];
    NSError *err = ckWait([&](void (^finish)(NSError *)) {
        CKFetchRecordsOperation *op = [[CKFetchRecordsOperation alloc] initWithRecordIDs:@[ rid ]];
        configure(op, true);
        op.desiredKeys = itemKeys();
        op.fetchRecordsCompletionBlock = ^(NSDictionary<CKRecordID *, CKRecord *> *recs, NSError *e) {
            if (recs[rid]) [box addObject:recs[rid]];
            NSError *one = e && e.code == CKErrorPartialFailure ? e.userInfo[CKPartialErrorsByItemIDKey][rid] : e;
            finish(one && one.code != CKErrorUnknownItem && one.code != CKErrorZoneNotFound ? one : nil);
        };
        [c.db addOperation:op];
    });
    if (!err && box.count) *out = box.firstObject;
    return err;
}

NSError *ckSave(const Cloud &c, NSArray<CKRecord *> *records, NSArray<CKRecordID *> *deletes, bool cellular)
{
    return ckWait([&](void (^finish)(NSError *)) {
        CKModifyRecordsOperation *op = [[CKModifyRecordsOperation alloc] initWithRecordsToSave:records recordIDsToDelete:deletes];
        configure(op, cellular);
        op.savePolicy = CKRecordSaveAllKeys;
        op.modifyRecordsCompletionBlock = ^(NSArray *saved, NSArray *deleted, NSError *e) { finish(e); };
        [c.db addOperation:op];
    });
}

NSError *ckFetchChunk(const Cloud &c, CKRecordID *rid, std::string *assetPath)
{
    NSMutableArray<NSString *> *box = [NSMutableArray array];
    NSError *err = ckWait([&](void (^finish)(NSError *)) {
        CKFetchRecordsOperation *op = [[CKFetchRecordsOperation alloc] initWithRecordIDs:@[ rid ]];
        configure(op, true);
        op.desiredKeys = @[ @"data" ];
        op.fetchRecordsCompletionBlock = ^(NSDictionary<CKRecordID *, CKRecord *> *recs, NSError *e) {
            CKRecord *r = recs[rid];
            CKAsset *a = r ? r[@"data"] : nil;
            // the asset's file lasts only as long as the operation's callback: moved out
            if ([a isKindOfClass:[CKAsset class]] && a.fileURL) {
                NSString *dst = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"hotd-chunk-%@", [NSUUID UUID].UUIDString]];
                if ([[NSFileManager defaultManager] moveItemAtPath:a.fileURL.path toPath:dst error:nil]
                    || [[NSFileManager defaultManager] copyItemAtPath:a.fileURL.path toPath:dst error:nil]) {
                    [box addObject:dst];
                }
            }
            finish(e ?: (box.count ? nil : [NSError errorWithDomain:CKErrorDomain code:CKErrorUnknownItem userInfo:nil]));
        };
        [c.db addOperation:op];
    });
    if (!err && box.count) *assetPath = box.firstObject.UTF8String;
    return err;
}

/* A batch's error says only "Failed to modify some records (2)": the first record's own error follows it */
std::string errText(NSError *e)
{
    if (!e) return std::string();
    std::string text = std::string(e.localizedDescription.UTF8String ?: "error") + " (" + std::to_string((long)e.code) + ")";
    NSDictionary *each = e.code == CKErrorPartialFailure ? e.userInfo[CKPartialErrorsByItemIDKey] : nil;
    NSError *one = [each isKindOfClass:[NSDictionary class]] ? each.allValues.firstObject : nil;
    if ([one isKindOfClass:[NSError class]]) text += ": " + errText(one);
    return text;
}

NSString *genOf(CKRecord *rec)
{
    NSString *g = rec ? rec[@"gen"] : nil;
    return [g isKindOfClass:[NSString class]] ? g : nil;
}

std::string strOf(CKRecord *rec, NSString *key)
{
    NSString *s = rec ? rec[key] : nil;
    return [s isKindOfClass:[NSString class]] ? std::string(s.UTF8String) : std::string();
}

/* the record holds this content already: its size and SHA-256 */
bool sameContent(CKRecord *rec, const LocalRom &r, const std::string &hash)
{
    return rec && [rec[@"size"] longLongValue] == r.bytes && !hash.empty() && strOf(rec, @"hash") == hash;
}

NSArray<CKRecordID *> *chunkIds(const Cloud &c, const std::string &gen, int n)
{
    NSMutableArray<CKRecordID *> *ids = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        [ids addObject:[[CKRecordID alloc] initWithRecordName:ns("c-rom-" + gen + "-" + std::to_string(i)) zoneID:c.zone]];
    }
    return ids;
}

/* Up: chunks first, the item record last. Another device may upload at the same time: the record is looked at before
 * and after; if it already holds this content, ours is dropped and theirs adopted (*adopted). */
NSError *uploadRom(const Cloud &c, const LocalRom &r, CKRecord *old, CKRecord **adopted)
{
    *adopted = nil;
    const std::string abs = g_root + "/arcade/" + r.name;
    const std::string hash = romHash(r);
    CKRecord *now = nil;
    NSError *err = ckFetchItem(c, &now);
    if (err) return err;
    if (now && ![genOf(now) isEqualToString:genOf(old)]) {
        if (sameContent(now, r, hash)) {
            *adopted = now;
            cloudLog("%s: in iCloud already, from another device", r.name.c_str());
            return nil;
        }
        old = now;
    }
    char gen[16];
    snprintf(gen, sizeof(gen), "%08x", arc4random());
    cloudLog("uploading %s (%lld bytes)", r.name.c_str(), r.bytes);
    FILE *f = fopen(abs.c_str(), "rb");
    if (!f) return [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
    int chunks = 0;
    long long done = 0;
    std::string buf;
    std::vector<char> io(1 << 20);
    for (;;) {
        size_t n = 0;
        buf.clear();
        while (buf.size() < kChunkBytes && (n = fread(io.data(), 1, std::min(io.size(), kChunkBytes - buf.size()), f)) > 0) buf.append(io.data(), n);
        if (buf.empty()) break;
        NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"hotd-up-%s-%d", gen, chunks]];
        if (!writeFileAtomic(tmp.UTF8String, buf)) {
            err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
            break;
        }
        CKRecord *rec = [[CKRecord alloc] initWithRecordType:kChunkType recordID:chunkIds(c, gen, chunks + 1).lastObject];
        rec[@"data"] = [[CKAsset alloc] initWithFileURL:[NSURL fileURLWithPath:tmp]];
        err = ckSave(c, @[ rec ], nil, false);
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
        if (err) break;
        chunks++;
        done += (long long)buf.size();
        const int pct = r.bytes ? (int)(done * 100 / r.bytes) : 0;
        setStatus("iCloud: uploading " + r.name + " " + std::to_string(pct) + "%");
        if (buf.size() < kChunkBytes) break;
    }
    fclose(f);
    if (!err && done != r.bytes) err = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:@{ NSLocalizedDescriptionKey: @"the set changed while it went up" }];
    if (err) {
        if (chunks) ckSave(c, nil, chunkIds(c, gen, chunks), false);   // what did go up
        return err;
    }
    // another device may have finished the same set meanwhile
    err = ckFetchItem(c, &now);
    if (!err && now && ![genOf(now) isEqualToString:genOf(old)]) {
        if (sameContent(now, r, hash)) {
            ckSave(c, nil, chunkIds(c, gen, chunks), false);
            *adopted = now;
            cloudLog("%s: another device finished it first, its copy kept", r.name.c_str());
            return nil;
        }
        old = now;
    }
    CKRecordID *rid = [[CKRecordID alloc] initWithRecordName:@"i-rom" zoneID:c.zone];
    CKRecord *rec = [[CKRecord alloc] initWithRecordType:kItemType recordID:rid];
    rec[@"name"] = ns(r.name);
    rec[@"stamp"] = ns(r.stamp);
    rec[@"size"] = @(r.bytes);
    rec[@"chunks"] = @(chunks);
    rec[@"gen"] = ns(gen);
    rec[@"hash"] = ns(hash);
    err = ckSave(c, @[ rec ], nil, false);
    if (!err && genOf(old)) ckSave(c, nil, chunkIds(c, genOf(old).UTF8String, [old[@"chunks"] intValue]), false);   // (best effort)
    if (!err) cloudLog("%s: in iCloud (%d chunk%s)", r.name.c_str(), chunks, chunks == 1 ? "" : "s");
    return err;
}

/* The set put in place as arcade/<name>, the other kind removed (one set: the game takes hotd.zip first) */
bool installRom(const std::string &file, const std::string &name)
{
    const std::string dir = g_root + "/arcade";
    makeDirs(dir);
    if (rename(file.c_str(), (dir + "/" + name).c_str()) != 0) return false;
    remove((dir + "/" + (name == "hotd.zip" ? "hotd.7z" : "hotd.zip")).c_str());
    return true;
}

/* .hotd-icloud/ready/: a set that came down while one was here, and "rom.txt" (its name), put in place at the start */
bool installReady()
{
    const std::string ready = statePath("ready");
    std::string name;
    if (!readFile(ready + "/rom.txt", &name)) {
        removeTree(ready);
        return false;
    }
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
    const bool ok = (name == "hotd.zip" || name == "hotd.7z") && installRom(ready + "/rom", name);
    cloudLog(ok ? "installed %s from iCloud" : "could not install %s from iCloud", name.c_str());
    removeTree(ready);
    return ok;
}

NSError *downloadRom(const Cloud &c, CKRecord *rec, bool *deferred)
{
    *deferred = false;
    NSString *gen = genOf(rec);
    const int chunks = [rec[@"chunks"] intValue];
    const long long size = [rec[@"size"] longLongValue];
    const std::string name = strOf(rec, @"name"), hash = strOf(rec, @"hash");
    if (!gen || chunks <= 0 || (name != "hotd.zip" && name != "hotd.7z") || hash.empty()) {
        return [NSError errorWithDomain:CKErrorDomain code:CKErrorUnknownItem userInfo:@{ NSLocalizedDescriptionKey: @"iCloud's set is incomplete" }];
    }
    const std::string part = statePath("dl-rom.part");
    remove(part.c_str());
    FILE *out = fopen(part.c_str(), "wb");
    if (!out) return [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
    cloudLog("downloading %s (%d chunk%s)", name.c_str(), chunks, chunks == 1 ? "" : "s");
    CC_SHA256_CTX sha;
    CC_SHA256_Init(&sha);
    long long got = 0;
    NSError *err = nil;
    for (int i = 0; i < chunks && !err; i++) {
        std::string asset;
        err = ckFetchChunk(c, chunkIds(c, gen.UTF8String, chunks)[i], &asset);
        if (err) break;
        FILE *f = fopen(asset.c_str(), "rb");
        std::vector<char> buf(1 << 20);
        size_t n;
        while (f && (n = fread(buf.data(), 1, buf.size(), f)) > 0) {
            if (fwrite(buf.data(), 1, n, out) != n) {
                err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
                break;
            }
            CC_SHA256_Update(&sha, buf.data(), (CC_LONG)n);
            got += (long long)n;
            const float share = size ? (float)std::min<long long>(got, size) / (float)size : 0;
            setStatus("iCloud: downloading " + name + " " + std::to_string((int)(share * 100)) + "%");
            {
                std::lock_guard<std::mutex> hold(g_lock);
                if (g_rom == HOTD_CLOUD_ROM_LOOKING || g_rom == HOTD_CLOUD_ROM_DOWNLOADING) {
                    g_rom = HOTD_CLOUD_ROM_DOWNLOADING;
                    g_romDone = share;
                }
            }
        }
        if (f) fclose(f);
        remove(asset.c_str());
    }
    if (fclose(out) != 0 && !err) err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &sha);
    if (!err && (got != size || hexOf(md, sizeof(md)) != hash)) {
        err = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:@{ NSLocalizedDescriptionKey: @"the download didn't match iCloud's set" }];
    }
    if (err) {
        remove(part.c_str());
        return err;
    }
    if (!localRom().name.empty()) {
        // a set is here, and the game may be reading it: at the next start
        const std::string ready = statePath("ready");
        removeTree(ready);
        makeDirs(ready);
        rename(part.c_str(), (ready + "/rom").c_str());
        writeFileAtomic(ready + "/rom.txt", name + "\n");
        *deferred = true;
        return nil;
    }
    if (!installRom(part, name)) {
        const int e = errno;
        remove(part.c_str());
        return [NSError errorWithDomain:NSPOSIXErrorDomain code:e userInfo:nil];
    }
    writeFileAtomic(statePath("rom.hash"), localRom().stamp + "\t" + hash);
    return nil;
}

/* files.txt: "#cloud <id>", then the item \t this device's stamp \t iCloud's, as last synced with that database.
 * Another database's stamps, or none, count as never synced. */
struct Synced { std::string local, remote; };

std::map<std::string, Synced> readSynced(const std::string &cloud)
{
    std::map<std::string, Synced> m;
    std::string text;
    if (!readFile(statePath("files.txt"), &text)) return m;
    const std::string head = "#cloud " + cloud + "\n";
    if (cloud.empty() || text.compare(0, head.size(), head) != 0) return m;
    size_t pos = head.size();
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        m[line.substr(0, a)] = { line.substr(a + 1, b - a - 1), line.substr(b + 1) };
    }
    return m;
}

void writeSynced(const std::map<std::string, Synced> &m, const std::string &cloud)
{
    std::string text = "#cloud " + cloud + "\n";
    for (const auto &kv : m) text += kv.first + "\t" + kv.second.local + "\t" + kv.second.remote + "\n";
    writeFileAtomic(statePath("files.txt"), text);
}

/*
 * This CloudKit database's own id: a HotdJoined record "files-db" holding a random id, made by the first device to
 * look. Development builds and TestFlight's are two databases: GoldenEye VR's TestFlight, installed over test builds
 * (2026-11-23), took every record for changed and downloaded packs the devices had, because its last-synced stamps
 * were the test builds'. "" when it cannot be read or made.
 */
std::string cloudId(const Cloud &c)
{
    static std::string known;   // one database for the process's life
    if (!known.empty()) return known;
    CKRecordID *rid = [[CKRecordID alloc] initWithRecordName:@"files-db" zoneID:c.zone];
    for (int attempt = 0; attempt < 2; attempt++) {
        NSMutableArray<NSString *> *box = [NSMutableArray array];
        NSError *err = ckWait([&](void (^finish)(NSError *)) {
            CKFetchRecordsOperation *op = [[CKFetchRecordsOperation alloc] initWithRecordIDs:@[ rid ]];
            configure(op, true);
            op.desiredKeys = @[ @"by" ];
            op.fetchRecordsCompletionBlock = ^(NSDictionary<CKRecordID *, CKRecord *> *recs, NSError *e) {
                NSString *by = recs[rid] ? recs[rid][@"by"] : nil;
                if ([by isKindOfClass:[NSString class]] && by.length) [box addObject:by];
                NSError *one = e && e.code == CKErrorPartialFailure ? e.userInfo[CKPartialErrorsByItemIDKey][rid] : e;
                finish(one && one.code != CKErrorUnknownItem && one.code != CKErrorZoneNotFound ? one : nil);
            };
            [c.db addOperation:op];
        });
        if (err) return std::string();
        if (box.count) return known = box.firstObject.UTF8String;
        // none yet: ours, unless another device makes it first (then read theirs)
        CKRecord *rec = [[CKRecord alloc] initWithRecordType:kJoinedType recordID:rid];
        rec[@"by"] = [NSUUID UUID].UUIDString;
        err = ckWait([&](void (^finish)(NSError *)) {
            CKModifyRecordsOperation *op = [[CKModifyRecordsOperation alloc] initWithRecordsToSave:@[ rec ] recordIDsToDelete:nil];
            configure(op, true);
            op.savePolicy = CKRecordSaveIfServerRecordUnchanged;
            op.modifyRecordsCompletionBlock = ^(NSArray *saved, NSArray *deleted, NSError *e) { finish(e); };
            [c.db addOperation:op];
        });
        if (!err) return known = ((NSString *)rec[@"by"]).UTF8String;
        NSError *one = err.code == CKErrorPartialFailure ? err.userInfo[CKPartialErrorsByItemIDKey][rid] : err;
        if (one.code == CKErrorServerRecordChanged) continue;   // another device made it meanwhile: the next look reads theirs
        cloudLog("files-db: %s", errText(err).c_str());
    }
    return std::string();
}

/* What the set does in a pass. L, R: its stamp here and in iCloud ("" for none); s: as last synced with this
 * database, or null; same(): whether the two hold the same content, asked only when it decides. */
enum class Move { None, Adopt, Up, Down };

template <typename Same>
Move decideMove(const std::string &L, const std::string &R, const Synced *s, Same same)
{
    if (L.empty() && R.empty()) return Move::None;
    if (R.empty()) return Move::Up;
    if (L.empty()) return Move::Down;
    if (s && s->local == L && s->remote == R) return Move::None;
    if (s && s->local == L) return same() ? Move::Adopt : Move::Down;   // changed in iCloud only
    if (s && s->remote == R) return Move::Up;                           // changed here only
    if (s) return same() ? Move::Adopt : Move::Down;                    // both: iCloud's, what the other devices have
    // never synced with this database (a new device, another CloudKit environment's build, tvOS emptied the caches):
    // the set here stays as it is, whatever iCloud holds, and from now on its changes move either way
    return Move::Adopt;
}

/* the first look while this device has no set is over: it came, or it isn't coming this time */
void romLookDone()
{
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_rom == HOTD_CLOUD_ROM_LOOKING || g_rom == HOTD_CLOUD_ROM_DOWNLOADING) g_rom = HOTD_CLOUD_ROM_NONE;
}

void syncFiles(const Cloud &c)
{
    setStatus("iCloud: checking");
    NSError *err = ckEnsureZone(c);
    CKRecord *item = nil;
    std::vector<ChunkRef> chunkRefs;
    if (!err) err = ckFetchAll(c, &item, &chunkRefs);
    if (err) {
        setStatus("iCloud: " + errText(err));
        cloudLog("files: %s", errText(err).c_str());
        return;
    }
    {   // chunks no item names, a day old (an upload cut off): removed
        std::set<std::string> named;
        NSString *g = genOf(item);
        for (int i = 0; g && i < [item[@"chunks"] intValue]; i++) named.insert("c-rom-" + std::string(g.UTF8String) + "-" + std::to_string(i));
        NSMutableArray<CKRecordID *> *orphans = [NSMutableArray array];
        for (const ChunkRef &ch : chunkRefs) {
            if (!named.count(ch.name) && ch.age > 24 * 3600) [orphans addObject:[[CKRecordID alloc] initWithRecordName:ns(ch.name) zoneID:c.zone]];
        }
        if (orphans.count && !ckSave(c, nil, orphans, false)) cloudLog("removed %lu unused chunk(s)", (unsigned long)orphans.count);
    }
    const std::string cloud = cloudId(c);
    if (cloud.empty()) {
        setStatus("iCloud: unavailable");
        cloudLog("files: this database's id could not be read; nothing moves this pass");
        return;
    }
    std::map<std::string, Synced> synced = readSynced(cloud);
    const LocalRom l = localRom();
    const std::string R = strOf(item, @"stamp");
    auto s = synced.find(kRomItem.UTF8String);
    if (exists(statePath("ready/rom.txt"))) {   // a set waits for the next start already: nothing moves until then
        setStatus("iCloud: a new ROM set is used from the next start");
        return;
    }
    const Move move = decideMove(l.stamp, R, s != synced.end() ? &s->second : nullptr, [&]() { return sameContent(item, l, romHash(l)); });
    switch (move) {
    case Move::None:
        setStatus("iCloud: up to date");
        return;
    case Move::Adopt:
        if (s == synced.end()) cloudLog("%s: kept as it is here (first sync with this iCloud)", l.name.c_str());
        synced[kRomItem.UTF8String] = { l.stamp, R };
        writeSynced(synced, cloud);
        setStatus("iCloud: up to date");
        return;
    case Move::Up: {
        CKRecord *adopted = nil;
        err = uploadRom(c, l, item, &adopted);
        if (!err) {
            synced[kRomItem.UTF8String] = { l.stamp, adopted ? strOf(adopted, @"stamp") : l.stamp };
            writeSynced(synced, cloud);
            setStatus("iCloud: up to date");
        }
        break;
    }
    case Move::Down: {
        bool deferred = false;
        err = downloadRom(c, item, &deferred);
        if (!err) {
            // a deferred one is stamped at the next pass, once it is in place
            if (!deferred) {
                synced[kRomItem.UTF8String] = { localRom().stamp, R };
                writeSynced(synced, cloud);
                setRom(HOTD_CLOUD_ROM_ARRIVED, 1);
                setStatus("iCloud: your ROM set came from iCloud");
            } else {
                setStatus("iCloud: a new ROM set is used from the next start");
            }
            cloudLog("%s from iCloud%s", strOf(item, @"name").c_str(), deferred ? " (in place at the next start)" : "");
        }
        break;
    }
    }
    if (err) {
        setStatus("iCloud: " + errText(err));
        cloudLog("%s: %s", move == Move::Up ? "upload" : "download", errText(err).c_str());
    }
}

bool makeCloud(Cloud *c)
{
    NSString *containerId = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"HOTDICloudContainer"];
    if (![containerId isKindOfClass:[NSString class]] || !containerId.length) containerId = @"iCloud.com.mrsco.hotdvr";
    @try {
        c->container = [CKContainer containerWithIdentifier:containerId];
        c->db = c->container.privateCloudDatabase;
        c->zone = [[CKRecordZoneID alloc] initWithZoneName:kZoneName ownerName:CKCurrentUserDefaultName];
    } @catch (NSException *e) {
        cloudLog("CloudKit unavailable: %s", e.reason.UTF8String ?: "?");
        return false;
    }
    return true;
}

/* The record a device leaves when it first puts settings in the store: a new device that finds the store empty but
 * this record there knows the other devices' settings are still on their way, and pushes nothing (GoldenEye VR's
 * first device test, 2026-11-22: a new iPad pushed its own settings over the iPhone's). 1: there, 0: not there, -1:
 * could not tell (no account, no network). */
int joinedMarker(bool create)
{
    Cloud c;
    if (!makeCloud(&c)) return -1;
    CKRecordID *rid = [[CKRecordID alloc] initWithRecordName:@"joined" zoneID:c.zone];
    NSMutableArray<NSNumber *> *box = [NSMutableArray array];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    CKOperationConfiguration *cfg = [[CKOperationConfiguration alloc] init];
    cfg.timeoutIntervalForRequest = 8;
    cfg.timeoutIntervalForResource = 10;
    cfg.qualityOfService = NSQualityOfServiceUserInitiated;
    if (create) {
        if (ckEnsureZone(c)) return -1;
        CKRecord *rec = [[CKRecord alloc] initWithRecordType:kJoinedType recordID:rid];
        rec[@"by"] = ns(kFamily);
        CKModifyRecordsOperation *op = [[CKModifyRecordsOperation alloc] initWithRecordsToSave:@[ rec ] recordIDsToDelete:nil];
        op.configuration = cfg;
        op.savePolicy = CKRecordSaveAllKeys;
        op.modifyRecordsCompletionBlock = ^(NSArray *saved, NSArray *deleted, NSError *e) {
            [box addObject:@(e ? -1 : 1)];
            dispatch_semaphore_signal(done);
        };
        [c.db addOperation:op];
    } else {
        CKFetchRecordsOperation *op = [[CKFetchRecordsOperation alloc] initWithRecordIDs:@[ rid ]];
        op.configuration = cfg;
        op.desiredKeys = @[];
        op.fetchRecordsCompletionBlock = ^(NSDictionary<CKRecordID *, CKRecord *> *recs, NSError *e) {
            int v = recs[rid] ? 1 : -1;
            if (!recs[rid] && e) {
                NSError *one = e.code == CKErrorPartialFailure ? e.userInfo[CKPartialErrorsByItemIDKey][rid] : e;
                if (one.code == CKErrorUnknownItem || one.code == CKErrorZoneNotFound) v = 0;
            }
            [box addObject:@(v)];
            dispatch_semaphore_signal(done);
        };
        [c.db addOperation:op];
    }
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC)) != 0) return -1;
    return box.firstObject.intValue;
}

void fileWorker()
{
    Cloud c;
    if (!makeCloud(&c)) {
        setStatus("");
        romLookDone();
        return;
    }
    for (;;) {
        bool sync;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            sync = g_sync;
        }
        if (sync) {
            @autoreleasepool {
                CKAccountStatus status = CKAccountStatusCouldNotDetermine;
                CKAccountStatus *statusOut = &status;
                ckWait([&](void (^finish)(NSError *)) {
                    [c.container accountStatusWithCompletionHandler:^(CKAccountStatus s, NSError *e) {
                        *statusOut = s;
                        finish(nil);
                    }];
                });
                if (status == CKAccountStatusAvailable) syncFiles(c);
                else setStatus(status == CKAccountStatusNoAccount ? "iCloud: not signed in" : "iCloud: unavailable");
            }
        } else {
            setStatus("");
        }
        romLookDone();
        std::unique_lock<std::mutex> hold(g_workLock);
        g_workWake.wait_for(hold, std::chrono::minutes(15), [] { return g_workKick; });
        g_workKick = false;
    }
}

void startWorker()
{
    if (g_workerStarted) return;
    g_workerStarted = true;
    std::thread(fileWorker).detach();
}

void kick()
{
    std::lock_guard<std::mutex> hold(g_workLock);
    g_workKick = true;
    g_workWake.notify_all();
}

/* every 20 s and on going to the background: settings and saves changed here go up */
void watch()
{
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_sync || !g_joined) return;
    }
    @autoreleasepool {
        std::lock_guard<std::mutex> hold(g_kvLock);
        cfgWatch(g_opt);
        cfgWatch(g_onl);
        savesWatch();
    }
}

void startWatch()
{
    if (g_watchTimer) return;
    g_watchQueue = dispatch_queue_create("hotd.icloud.watch", DISPATCH_QUEUE_SERIAL);
    g_watchTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_watchQueue);
    dispatch_source_set_timer(g_watchTimer, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), 20 * NSEC_PER_SEC, 5 * NSEC_PER_SEC);
    dispatch_source_set_event_handler(g_watchTimer, ^{ watch(); });
    dispatch_resume(g_watchTimer);
    [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil
                                                  usingBlock:^(NSNotification *n) {
                                                      dispatch_async(g_watchQueue, ^{
                                                          watch();
                                                          [kvs() synchronize];
                                                      });
                                                  }];
}

/* the menu's row, before the settings are read: from the options file's text (absent: on) */
void readOptions()
{
    std::string text;
    if (!readFile(g_root + "/" + g_opt.name, &text)) return;
    const std::map<std::string, std::string> m = cfgMap(text);
    auto s = m.find("icloud_sync");
    if (s != m.end()) g_sync = atoi(s->second.c_str()) != 0;
}

}   // namespace

extern "C" int hotd_cloud_available(void)
{
    static int avail = -1;
    if (avail < 0) {
        id v = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"HOTDICloud"];
        avail = [v respondsToSelector:@selector(boolValue)] && [v boolValue] ? 1 : 0;
    }
    return avail;
}

extern "C" void hotd_cloud_launch(const char *files, const char *options)
{
    if (g_launched || !hotd_cloud_available() || !files) return;
    g_launched = true;
    g_root = files;
    while (g_root.size() > 1 && g_root.back() == '/') g_root.pop_back();
    if (options && *options) g_opt.name = options;
    makeDirs(statePath(""));
    readOptions();
    if (installReady()) setStatus("iCloud: your new ROM set is in");
    [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:nil
                                                  usingBlock:^(NSNotification *n) { kick(); }];
    if (!g_sync) {
        cloudLog("off (icloud_sync=0)");
        return;
    }
    if (localRom().name.empty()) setRom(HOTD_CLOUD_ROM_LOOKING, 0);

    @autoreleasepool {
        // Joining: a device takes part in the settings sync once the store has the other devices' settings (they
        // win), or once CloudKit says no device has joined yet (it is the first, and its settings go up). Until then
        // it pushes nothing: a new device whose store has not come down yet would put its own over everyone's.
        dispatch_semaphore_t arrived = dispatch_semaphore_create(0);
        [[NSNotificationCenter defaultCenter] addObserverForName:NSUbiquitousKeyValueStoreDidChangeExternallyNotification
                                                          object:kvs()
                                                           queue:[[NSOperationQueue alloc] init]
                                                      usingBlock:^(NSNotification *n) {
                                                          NSNumber *reason = n.userInfo[NSUbiquitousKeyValueStoreChangeReasonKey];
                                                          if (reason.integerValue == NSUbiquitousKeyValueStoreQuotaViolationChange)
                                                              cloudLog("key-value store over its quota");
                                                          if (g_launchDone && !g_joined && storeHasData())
                                                              cloudLog("the other devices' settings arrived: used from the next start");
                                                          dispatch_semaphore_signal(arrived);
                                                      }];
        [kvs() synchronize];
        const bool joined = [[NSUserDefaults standardUserDefaults] boolForKey:kJoinedDefault];
        bool join = joined;
        if (!join) {
            if (!storeHasData()) {
                cloudLog("first sync: waiting for the store");
                dispatch_semaphore_wait(arrived, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
            }
            if (storeHasData()) {
                join = true;
                cloudLog("joining: the other devices' settings and saves win");
            } else {
                const int there = joinedMarker(false);
                if (there == 0) {
                    join = true;
                    cloudLog("joining as the first device: this device's settings and saves go up");
                } else {
                    cloudLog(there > 0 ? "the other devices' settings have not arrived yet: nothing goes up until they do"
                                       : "could not reach iCloud: settings stay on this device for now");
                }
            }
        }
        if (join) {
            {
                std::lock_guard<std::mutex> hold(g_kvLock);
                cfgLaunchMerge(g_opt, joined);
                cfgLaunchMerge(g_onl, joined);
                savesLaunchMerge(joined);
                g_joined = true;
            }
            if (!joined) {
                [[NSUserDefaults standardUserDefaults] setBool:YES forKey:kJoinedDefault];
                std::thread([] { joinedMarker(true); }).detach();
            }
            [kvs() synchronize];
        }
    }
    g_launchDone = true;
    startWorker();
    startWatch();
    cloudLog("on (%s, %s, files %s)", kFamily, g_joined ? "joined" : "not joined yet", g_root.c_str());
}

extern "C" void hotd_cloud_kick(void)
{
    if (g_launched) kick();
}

extern "C" void hotd_cloud_set_sync(int on)
{
    if (!g_launched) return;   // a build without iCloud
    bool changed;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        changed = g_sync != (on != 0);
        g_sync = on != 0;
    }
    if (!changed) return;
    cloudLog("sync %s", on ? "on" : "off");
    // turned on in a run that began with it off: the ROM set now, the settings and saves from the next start (the
    // game has read its files)
    if (on) startWorker();
    kick();
}

extern "C" const char *hotd_cloud_line(void)
{
    if (!hotd_cloud_available()) return NULL;
    static char line[2][160];
    static int which;
    std::lock_guard<std::mutex> hold(g_lock);
    which ^= 1;
    snprintf(line[which], sizeof line[which], "%s", g_sync ? g_status.c_str() : "");
    return line[which];
}

extern "C" int hotd_cloud_rom(float *done)
{
    std::lock_guard<std::mutex> hold(g_lock);
    if (done) *done = g_romDone;
    return g_launched ? g_rom : HOTD_CLOUD_ROM_NONE;
}
