// Apple TV's front end (main_tvos.m) and its two helpers: the ROM set sent from a phone or computer over the LAN
// (tv_upload.m: a TV has no document picker and no Files app) and the small files kept where tvOS keeps them
// (tv_store.m: an app's local files are caches the system may empty).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ---- tv_upload.m: a small web page on the LAN that sends hotd.zip / hotd.7z here
// Starts serving (port 8007, or any free one); arcade_dir is where the set lands (hotd.zip or hotd.7z, by its first
// bytes). done runs on the main thread when a set arrived whole. 0 when no socket could be opened.
typedef void (*tv_upload_done)(const char *path);
int tv_upload_start(const char *arcade_dir, tv_upload_done done);
void tv_upload_stop(void);
// http://<this TV's LAN address>:<port>/ into url (empty when there is no network); 1 when it has an address
int tv_upload_url(char *url, int n);
// a set on its way: 1 and the bytes so far / in all; and the last refusal's text (a file that isn't a ZIP or 7z)
int tv_upload_progress(long long *got, long long *total);
const char *tv_upload_error(void);

// ---- tv_store.m: options.cfg, online.cfg and save/ also in the app's defaults (NSUserDefaults, which tvOS keeps;
// up to 500 KB an app), put back when tvOS emptied the caches
void tv_store_restore(const char *files);   // at start, before the app reads them: the missing ones put back
void tv_store_save(const char *files);      // those that changed, into the defaults
int tv_store_had_rom(void);                 // a ROM set was here before (missing now: tvOS emptied the caches)
void tv_store_set_had_rom(int had);

#ifdef __cplusplus
}
#endif
