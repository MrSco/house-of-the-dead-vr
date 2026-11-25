// iCloud sync's local rules (src/platform/apple/icloud_apple.mm): the settings files' merge, which keys stay on one kind
// of device, the saves' choice, the ROM set's moves and files (files.txt per CloudKit database, a set put in place, a
// download that waits for the next start). No iCloud account needed: built for the iOS simulator and run there
// (tests/apple/test_icloud.sh, on the Mac).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "platform/platform.h"
}
extern "C" void plat_log(int level, const char *fmt, ...)
{
    (void)level;
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
}

#include "../../src/platform/apple/icloud_apple.mm"

static int s_fail = 0, s_checks = 0;
static void check(bool ok, const char *what)
{
    s_checks++;
    if (!ok) {
        printf("FAIL: %s\n", what);
        s_fail++;
    }
}

static void put(const std::string &path, const std::string &data)
{
    makeDirs(path.substr(0, path.rfind('/')));
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static std::string get(const std::string &path)
{
    std::string s;
    readFile(path, &s);
    return s;
}

static void setTime(const std::string &path, long long t)
{
    struct timeval tv[2] = { { (time_t)t, 0 }, { (time_t)t, 0 } };
    utimes(path.c_str(), tv);
}

static SaveSet saves(std::map<std::string, std::string> files, long long t)
{
    SaveSet s;
    s.files = files;
    s.t = t;
    return s;
}

int main()
{
    // key=value files (quest_options.c, online.c): lines changed in place, new keys at the end, no header
    {
        const std::string text = "rumble=1\r\nname=A=B\nmusic_volume=80\n";
        std::map<std::string, std::string> m = cfgMap(text);
        check(m["name"] == "A=B" && m["rumble"] == "1" && m.size() == 3, "a value keeps its own '=', CRLF lines read");
        check(cfgApply(text, { { "rumble", "0" }, { "icloud_sync", "1" } }) == "rumble=0\nname=A=B\nmusic_volume=80\nicloud_sync=1\n",
              "changed in place, a new key at the end");
        check(cfgApply("", { { "rumble", "0" } }) == "rumble=0\n", "a file from nothing has only the key");
    }
    // which keys every device shares
    {
        check(!isLocalKey(g_opt, "rumble") && !isLocalKey(g_opt, "pad_trigger_feedback") && !isLocalKey(g_opt, "music_volume")
                  && !isLocalKey(g_opt, "effects_volume") && !isLocalKey(g_opt, "voice_chat_volume"),
              "rumble, trigger feedback and the mix are shared");
        check(isLocalKey(g_opt, "game_volume") && isLocalKey(g_opt, "flat_picture") && isLocalKey(g_opt, "flat_gyro")
                  && isLocalKey(g_opt, "tv_remote_speed_pct") && isLocalKey(g_opt, "gun_pitch") && isLocalKey(g_opt, "world_scale")
                  && isLocalKey(g_opt, "cabinet_crosshair") && isLocalKey(g_opt, "brightness_level_pct") && isLocalKey(g_opt, "icloud_sync"),
              "the game's volume, the picture, the gyro, the remote, the headset's rows, the crosshair and iCloud are the device's");
        check(isLocalKey(g_opt, "some_new_key"), "a key nobody named stays on its device");
        check(!isLocalKey(g_onl, "name") && !isLocalKey(g_onl, "server") && isLocalKey(g_onl, "join") && isLocalKey(g_onl, "mic_muted"),
              "online.cfg: the name and server shared, the LAN address and the mic the device's");
        check(strcmp(sharedKey(g_opt).UTF8String, "opt1") == 0 && [familyKey(g_opt) hasPrefix:@"opt1@"], "the store's names");
    }
    // what a file takes from the cloud at the start
    {
        const std::map<std::string, std::string> local = { { "a", "1" }, { "b", "2" }, { "c", "3" } };
        const std::map<std::string, std::string> last = { { "a", "1" }, { "b", "1" }, { "c", "3" } };
        const std::map<std::string, std::string> remote = { { "a", "9" }, { "b", "9" }, { "c", "3" }, { "d", "4" } };
        std::map<std::string, std::string> t = cfgTake(local, last, remote, true);
        check(t.size() == 2 && t["a"] == "9" && t["d"] == "4" && !t.count("b"),
              "in step: the cloud's change comes down, this device's own change stays (it goes up)");
        t = cfgTake(local, last, remote, false);
        check(t.size() == 3 && t["a"] == "9" && t["b"] == "9" && t["d"] == "4", "joining: every value the cloud has wins");
    }
    // the saves: one set, never merged
    {
        const SaveSet none, a = saves({ { "backup_ram.bin", "A" }, { "ioboard_eeprom.bin", "E" } }, 100),
                            b = saves({ { "backup_ram.bin", "B" }, { "ioboard_eeprom.bin", "E" } }, 200);
        check(decideSaves(none, none, true, "") == SaveMove::None, "no saves anywhere: nothing");
        check(decideSaves(a, none, true, "") == SaveMove::Up && decideSaves(none, a, true, "") == SaveMove::Down,
              "only one side: it moves");
        check(decideSaves(a, saves(a.files, 300), true, "") == SaveMove::None, "the same set: nothing, whatever its time");
        check(decideSaves(a, b, false, "") == SaveMove::Down && decideSaves(b, a, false, "") == SaveMove::Down,
              "joining: the cloud's set, even an older one (a new install's fresh scores never go up over it)");
        check(decideSaves(b, a, true, "") == SaveMove::Up && decideSaves(a, b, true, "") == SaveMove::Down,
              "both changed: the one written last");
        const SaveSet restored = saves(a.files, 900);   // tvOS emptied the caches: tv_store.m put the set back, new times
        check(decideSaves(restored, b, true, saveSum(a)) == SaveMove::Down,
              "unchanged here since the last sync (only its time is new): the cloud's comes down");
        check(decideSaves(saves(b.files, 50), a, true, saveSum(a)) == SaveMove::Up, "changed here only, even written earlier: it goes up");
        check(saveSum(a) != saveSum(b) && saveSum(a) == saveSum(saves(a.files, 5)), "the sum is the content's, not the time's");
    }

    char tmpl[] = "/tmp/hotd-icloud-test-XXXXXX";
    const char *made = mkdtemp(tmpl);
    if (!made) {
        printf("FAIL: no temp folder\n");
        return 1;
    }
    g_root = made;
    makeDirs(statePath(""));

    // the save folder: the game's files, never ours; one still being written is looked at later
    {
        put(g_root + "/save/ioboard_eeprom.bin", "EEPROM");
        put(g_root + "/save/backup_ram.bin", "SCORES");
        put(g_root + "/save/backup_ram.bin.icloud-tmp", "x");
        put(g_root + "/save/.hidden", "x");
        setTime(g_root + "/save/ioboard_eeprom.bin", 1000);
        setTime(g_root + "/save/backup_ram.bin", 2000);
        SaveSet s = localSaves(2);
        check(s.files.size() == 2 && s.files["backup_ram.bin"] == "SCORES" && s.t == 2000 && !s.busy, "the two save files, the newest time");
        put(g_root + "/save/backup_ram.bin", "SCORES2");
        check(localSaves(2).busy && !localSaves(0).busy, "a file written just now waits");
        for (int i = 0; i < 7; i++) makeDirs(statePath("backups/save-2026010" + std::to_string(i) + "-000000"));
        saveBackup(s);
        int n = 0;
        if (DIR *d = opendir(statePath("backups").c_str())) {
            while (struct dirent *e = readdir(d)) n += strncmp(e->d_name, "save-", 5) == 0;
            closedir(d);
        }
        check(n == 5 && !exists(statePath("backups/save-20260100-000000")), "backups: five kept, the oldest gone");
    }
    // the ROM set: hotd.zip first, else hotd.7z
    {
        check(localRom().name.empty(), "no set: none");
        put(g_root + "/arcade/hotd.7z", "SEVENZIP");
        LocalRom r = localRom();
        check(r.name == "hotd.7z" && r.bytes == 8 && r.stamp.compare(0, 10, "hotd.7z:8:") == 0, "a 7z set and its stamp");
        put(g_root + "/arcade/hotd.zip", "ZIPDATA");
        check(localRom().name == "hotd.zip", "hotd.zip before hotd.7z (as the game takes them)");
        const std::string h = romHash(localRom());
        check(h == sha256Text("ZIPDATA") && get(statePath("rom.hash")).find(h) != std::string::npos, "its hash, kept by stamp");
        put(statePath("rom.hash"), localRom().stamp + "\tcached");
        check(romHash(localRom()) == "cached", "the kept hash is used while the stamp holds");
    }
    // a set put in place: the other kind goes (one set)
    {
        put(statePath("dl-rom.part"), "NEW7Z");
        check(installRom(statePath("dl-rom.part"), "hotd.7z"), "installed");
        check(get(g_root + "/arcade/hotd.7z") == "NEW7Z" && !exists(g_root + "/arcade/hotd.zip"), "hotd.7z in, hotd.zip gone");
        // a download while a set is here waits for the next start
        put(statePath("ready/rom"), "READYZIP");
        put(statePath("ready/rom.txt"), "hotd.zip\n");
        check(installReady() && get(g_root + "/arcade/hotd.zip") == "READYZIP" && !exists(g_root + "/arcade/hotd.7z")
                  && !exists(statePath("ready")),
              "the waiting set put in place at the start");
        put(statePath("ready/rom"), "BAD");
        put(statePath("ready/rom.txt"), "../escape\n");
        check(!installReady() && !exists(g_root + "/escape") && !exists(statePath("ready")), "a bad name in ready/ is dropped");
        check(!installReady(), "nothing waiting: nothing");
    }
    // the set's moves in a pass
    {
        int asked = 0;
        auto same = [&]() { asked++; return true; };
        auto differs = [&]() { asked++; return false; };
        const Synced was{ "L1", "R1" };
        check(decideMove("", "", nullptr, differs) == Move::None, "no set anywhere: nothing");
        check(decideMove("L1", "", nullptr, differs) == Move::Up, "only here: up");
        check(decideMove("", "R1", nullptr, differs) == Move::Down, "only in iCloud: down");
        check(decideMove("", "R1", &was, differs) == Move::Down, "gone here (tvOS emptied the caches): down again");
        check(decideMove("L1", "R1", &was, differs) == Move::None, "in step: nothing");
        check(decideMove("L1", "R2", &was, differs) == Move::Down, "changed in iCloud only: down");
        check(decideMove("L1", "R2", &was, same) == Move::Adopt, "another device's stamp, the same content: synced");
        check(decideMove("L2", "R1", &was, differs) == Move::Up, "changed here only: up");
        check(decideMove("L2", "R2", &was, differs) == Move::Down, "both changed: iCloud's wins");
        asked = 0;
        check(decideMove("L9", "R9", nullptr, differs) == Move::Adopt && asked == 0,
              "never synced with this iCloud: the set here stays, unread (GoldenEye VR's TestFlight over test builds)");
    }
    // files.txt: one CloudKit database's stamps
    {
        writeSynced({ { "rom", { "L1", "R1" } } }, "dev-db");
        check(readSynced("dev-db").count("rom") == 1 && readSynced("dev-db")["rom"].remote == "R1", "read back by its own database");
        check(readSynced("prod-db").empty(), "another database's stamps count as never synced");
        check(readSynced("").empty(), "an unknown database: never synced");
        put(statePath("files.txt"), "rom\tL1\tR1\n");
        check(readSynced("dev-db").empty(), "a files.txt without its database line: never synced");
    }
    // the ROM state the front ends wait on, before a launch
    {
        float done = -1;
        check(hotd_cloud_rom(&done) == HOTD_CLOUD_ROM_NONE, "not launched: none (the picker or the address page, as before)");
        check(hotd_cloud_available() == 0 && hotd_cloud_line() == NULL, "a binary not signed for iCloud: no row, no line");
    }
    removeTree(g_root);
    if (s_fail) {
        printf("%d of %d checks failed\n", s_fail, s_checks);
        return 1;
    }
    printf("PASS: icloud: %d checks (settings merge, device keys, saves, ROM set files, moves, files.txt per database)\n", s_checks);
    return 0;
}
