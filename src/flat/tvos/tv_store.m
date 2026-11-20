// tv.h's store: what has to outlive tvOS emptying the app's caches. A tvOS app's files all live in Library/Caches
// (Documents can't be written on the device), and the system empties caches when it runs short of space; the only
// local storage it keeps is the app's defaults (NSUserDefaults, about 500 KB). The ROM set (tens of MB) can't go
// there and is sent again when it is gone (main_tvos.m asks); the settings (options.cfg, online.cfg) and the game's
// saves (save/: its settings and high scores, a few KB) are copied into the defaults and put back.
#import <Foundation/Foundation.h>
#include "tv.h"
#include "platform/platform.h"

static NSString *const KEY_FILES = @"hotd.files";     // relative path -> contents
static NSString *const KEY_HAD_ROM = @"hotd.had_rom";
enum { LIMIT = 400 * 1024 };                           // (under tvOS's 500 KB for the defaults)

// the files kept: options.cfg, online.cfg, everything in save/
static NSArray<NSString *> *kept(NSString *root) {
    NSMutableArray *out = [NSMutableArray arrayWithObjects:@"options.cfg", @"online.cfg", nil];
    NSDirectoryEnumerator *e = [NSFileManager.defaultManager enumeratorAtPath:[root stringByAppendingPathComponent:@"save"]];
    for (NSString *f in e) {
        BOOL dir = NO;
        NSString *rel = [@"save" stringByAppendingPathComponent:f];
        if ([NSFileManager.defaultManager fileExistsAtPath:[root stringByAppendingPathComponent:rel] isDirectory:&dir] && !dir)
            [out addObject:rel];
    }
    return out;
}

void tv_store_restore(const char *files) {
    NSString *root = [NSString stringWithUTF8String:files];
    NSDictionary *d = [NSUserDefaults.standardUserDefaults dictionaryForKey:KEY_FILES];
    int n = 0;
    for (NSString *rel in d) {
        NSData *data = d[rel];
        if (![data isKindOfClass:NSData.class] || [rel containsString:@".."]) continue;
        NSString *p = [root stringByAppendingPathComponent:rel];
        if ([NSFileManager.defaultManager fileExistsAtPath:p]) continue;
        [NSFileManager.defaultManager createDirectoryAtPath:p.stringByDeletingLastPathComponent withIntermediateDirectories:YES
                                                attributes:nil error:nil];
        if ([data writeToFile:p atomically:YES]) n++;
    }
    if (n) plat_log(PLAT_LOG_INFO, "tv: %d file(s) put back from the app's defaults (tvOS had emptied the caches)", n);
}

void tv_store_save(const char *files) {
    NSString *root = [NSString stringWithUTF8String:files];
    NSDictionary *old = [NSUserDefaults.standardUserDefaults dictionaryForKey:KEY_FILES] ?: @{};
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    NSUInteger size = 0;
    for (NSString *rel in kept(root)) {
        NSData *data = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:rel]];
        if (!data) continue;
        if (size + data.length > LIMIT) { plat_log(PLAT_LOG_ERROR, "tv: %s not kept (the defaults are full)", rel.UTF8String); continue; }
        size += data.length;
        d[rel] = data;
    }
    if ([d isEqualToDictionary:old]) return;
    [NSUserDefaults.standardUserDefaults setObject:d forKey:KEY_FILES];
    plat_log(PLAT_LOG_INFO, "tv: %d file(s), %u bytes kept in the app's defaults", (int)d.count, (unsigned)size);
}

int tv_store_had_rom(void) { return [NSUserDefaults.standardUserDefaults boolForKey:KEY_HAD_ROM]; }
void tv_store_set_had_rom(int had) {
    if (tv_store_had_rom() != !!had) [NSUserDefaults.standardUserDefaults setBool:!!had forKey:KEY_HAD_ROM];
}
