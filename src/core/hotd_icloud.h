// iCloud sync across the Apple apps (docs/icloud-sync.md): iPhone and iPad, Apple TV and Vision Pro are one App Store
// Connect app (com.mrsco.hotdvr), so one iCloud store reaches all three. The Mac app (a GitHub zip) has none.
//  - Settings: options.cfg (the flat apps) and vr-options.cfg (Vision Pro), online.cfg: the iCloud key-value store,
//    merged key by key; keys that belong to one kind of device (the picture, the gyro, the remote, the headset's
//    view, the game's volume...) are kept per kind of device.
//  - The game's saves (save/: its settings and high scores): the key-value store too, whole, never merged; the one
//    written last wins.
//  - The ROM set (arcade/hotd.zip or hotd.7z): CloudKit, the player's private database.
// The code: src/platform/apple/icloud_apple.mm, built into the iPhone / iPad, Apple TV and Vision Pro apps with
// HOTD_ICLOUD defined. Everywhere else these calls are empty, and on an Apple build signed without the iCloud
// entitlements (the simulator, HOTD_ICLOUD=1 unset at the build) hotd_cloud_available() is 0 and nothing happens.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// What hotd_cloud_rom says about the ROM set while this device has none
enum {
    HOTD_CLOUD_ROM_NONE,          // iCloud has none for it (or sync is off, no account, no network, an error)
    HOTD_CLOUD_ROM_LOOKING,       // the first look in iCloud isn't over
    HOTD_CLOUD_ROM_DOWNLOADING,   // coming down (*done: its share, 0..1)
    HOTD_CLOUD_ROM_ARRIVED,       // it came down and is in arcade/
};

#if defined(HOTD_ICLOUD)

// At start, before the settings are read (flat_app_start, xr_setup), off the main thread: it may wait a few seconds
// on a device's first sync. files: the files folder; options: "options.cfg" or "vr-options.cfg". Merges the cloud's
// settings and saves into the files, puts in place a ROM set that came down during the last run, and starts the file
// sync (the ROM set) and the watch on the settings and saves (each change goes up).
void hotd_cloud_launch(const char *files, const char *options);
void hotd_cloud_kick(void);               // a ROM set was added or replaced: the file sync looks again
void hotd_cloud_set_sync(int on);         // the menu's iCloud sync row (QOPT_ICLOUD)
int hotd_cloud_available(void);           // a build signed for iCloud (the menu row shows)
// The menu's line beside CHANGE ROM SET ("iCloud: downloading hotd.zip 40%", "iCloud: up to date"), "" for none;
// NULL in a build without iCloud (the menu then has no iCloud row)
const char *hotd_cloud_line(void);
// The ROM set while this device has none (HOTD_CLOUD_ROM_*), *done its share while it comes down
int hotd_cloud_rom(float *done);

#else

static inline void hotd_cloud_launch(const char *files, const char *options) { (void)files; (void)options; }
static inline void hotd_cloud_kick(void) {}
static inline void hotd_cloud_set_sync(int on) { (void)on; }
static inline int hotd_cloud_available(void) { return 0; }
static inline const char *hotd_cloud_line(void) { return 0; }
static inline int hotd_cloud_rom(float *done) { (void)done; return HOTD_CLOUD_ROM_NONE; }

#endif

#ifdef __cplusplus
}
#endif
