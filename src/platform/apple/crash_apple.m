// Crash reports on Apple's systems (the SYSTEM tab's Crash reports): iPhone and iPad, Mac, Apple TV (src/flat/*:
// flat_os_crash_reports) and Vision Pro (src/visionos/xr_vos.m: xr_os_crash_reports). As the Quest app's
// (src/quest/quest_report.c) and Windows' (src/platform/windows/crash_win.c): the same JSON to the same relay
// (tools/vr/crash-relay), so one address serves every platform.
//  - The crash: a signal handler (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP: Swift's traps) appends to
//    crash.txt in the files folder: "crash: signal ..", the build id (the app executable's LC_UUID, as its dSYM
//    names it) and the frames (pc, lr, then the frame pointers: Apple's ABI keeps them), each as image+offset from
//    the image's Mach-O header. The shipped app is stripped, so no names: the triage symbolizes the frames with the
//    dSYM of that UUID. Then the default action runs, and the system's own crash log is made as before.
//  - The next start: with reports on and a relay address, the last crash in crash.txt, the end of the crashed run's
//    port.log (the front ends open a new port.log first thing: crash_apple_keep_log keeps the last as port.prev.log), the build, the settings and the device's model and system become crash-reports/<time>.json (crash.txt
//    emptied), and every report not sent yet is POSTed (NSURLSession) in the background: accepted, <time>.sent.json;
//    refused for good (4xx), <time>.rejected.json; otherwise tried again next time. crash-reports/last-result.txt
//    says how. Nothing personal: the files folder's path is written <files>, the home folder's ~.
//
// env.txt: QUEST_CRASH_URL=https://... (the relay; overrides REPORT_URL), QUEST_CRASH_KEY=... (sent as X-Report-Key),
// FLAT_CRASH_TEST=seconds (QUEST_CRASH_TEST too: crash on purpose that long after the start).
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include "../platform.h"
#include "quest_options.h"

#define REPORT_URL "https://hotdvr.roccojuliano.com/v1/reports"   // as quest_report.c's: the relay
#define REPORT_LOG_LINES 200
#define REPORT_CRASH_MAX (16 * 1024)
#define REPORT_LOG_MAX (48 * 1024)
#define REPORT_KEEP 10
#define REPORT_SEND_MAX 5
// the version and commit (CMake: scripts/build_info.py gives them)
#ifndef HOTD_BUILD_VERSION
#define HOTD_BUILD_VERSION ""
#endif
#ifndef HOTD_BUILD_COMMIT
#define HOTD_BUILD_COMMIT ""
#endif

// which app this is (the relay's PLATFORMS)
#if TARGET_OS_VISION
#define CRASH_PLATFORM "visionos"
#elif TARGET_OS_TV
#define CRASH_PLATFORM "tvos"
#elif TARGET_OS_IOS
#define CRASH_PLATFORM "ios"
#else
#define CRASH_PLATFORM "mac"
#endif

void crash_apple_start(const char *files, const int *opt);
void crash_apple_keep_log(const char *files);

static char files_dir[1024];
static char build_id[33];
static const struct mach_header *app_header;   // the app's executable (the game is linked into it)
static char app_name[128];                     // its file name, spaces as _ (the relay's APP_BINARIES)

// ---------------------------------------------------------------- the build id: the executable's LC_UUID
static void find_app(void) {
    if (app_header) return;
    app_header = _dyld_get_image_header(0);   // (image 0: the main executable)
    const char *path = _dyld_get_image_name(0);
    const char *slash = path ? strrchr(path, '/') : NULL;
    snprintf(app_name, sizeof app_name, "%s", slash ? slash + 1 : path ? path : "app");
    for (char *p = app_name; *p; p++) if (*p == ' ') *p = '_';
    snprintf(build_id, sizeof build_id, "none");
    if (!app_header || app_header->magic != MH_MAGIC_64) return;
    const uint8_t *lc = (const uint8_t *)app_header + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < app_header->ncmds; i++) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd == LC_UUID) {
            const uint8_t *u = ((const struct uuid_command *)c)->uuid;
            for (int k = 0; k < 16; k++) snprintf(build_id + k * 2, 3, "%02x", u[k]);
            return;
        }
        lc += c->cmdsize;
    }
}

// ---------------------------------------------------------------- the crash -> crash.txt
static int crash_fd = -1;
static char crash_head[96];   // "  build-id <uuid>\n"
static void crash_put(const char *s) { if (crash_fd >= 0 && write(crash_fd, s, strlen(s)) < 0) {} }
// a return address without its pointer authentication code (the system's arm64e frames sign theirs): XPACLRI, a
// no-op on hardware without it
static uintptr_t strip_pac(uintptr_t a) {
#if defined(__arm64__)
    register uintptr_t x30 __asm__("x30") = a;
    __asm__ volatile("hint #7" : "+r"(x30));
    return x30;
#else
    return a;
#endif
}
static void crash_addr(const char *what, uintptr_t a) {
    char b[400];
    Dl_info di;
    a = strip_pac(a);
    if (a && dladdr((void *)a, &di) && di.dli_fbase) {
        char name[128];
        if (di.dli_fbase == (void *)app_header) snprintf(name, sizeof name, "%s", app_name);
        else {
            const char *f = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
            snprintf(name, sizeof name, "%s", f ? f + 1 : di.dli_fname ? di.dli_fname : "?");
            for (char *p = name; *p; p++) if (*p == ' ') *p = '_';
        }
        // (the system's names are real; the stripped app's nearest export would mislead: none for it)
        const char *sym = di.dli_fbase != (void *)app_header && di.dli_sname ? di.dli_sname : "?";
        snprintf(b, sizeof b, "  %s %s+0x%lx (%s)\n", what, name, (unsigned long)(a - (uintptr_t)di.dli_fbase), sym);
    } else snprintf(b, sizeof b, "  %s %p\n", what, (void *)a);
    crash_put(b);
}
static void on_crash(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    char b[200];
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    snprintf(b, sizeof b, "crash: signal %d code %d addr %p thread %llu\n", sig, si ? si->si_code : 0, si ? si->si_addr : NULL,
             (unsigned long long)tid);
    crash_put(b);
    crash_put(crash_head);
    uintptr_t pc = 0, lr = 0, fpv = 0;
#if defined(__arm64__)
    if (uc && uc->uc_mcontext) {
        pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
        lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
        fpv = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
    }
#elif defined(__x86_64__)
    if (uc && uc->uc_mcontext) { pc = uc->uc_mcontext->__ss.__rip; fpv = uc->uc_mcontext->__ss.__rbp; }
#endif
    crash_addr("pc", pc);
    if (lr) crash_addr("lr", lr);
    uintptr_t *fp = (uintptr_t *)fpv;
    for (int i = 0; i < 48 && fp && ((uintptr_t)fp & 7) == 0; i++) {
        uintptr_t next = fp[0], ret = fp[1];
        if (!ret) break;
        crash_addr("at", ret);
        if (next <= (uintptr_t)fp) break;
        fp = (uintptr_t *)next;
    }
    if (crash_fd >= 0) fsync(crash_fd);
    signal(sig, SIG_DFL);   // (the system's crash log, as without this handler)
    raise(sig);
}
static void crash_handler(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/crash.txt", files_dir);
    crash_fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0660);
    snprintf(crash_head, sizeof crash_head, "  build-id %s\n", build_id);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
#if !TARGET_OS_TV
    // (its own stack, for this thread: a stack overflow here still gets written; tvOS has no sigaltstack)
    static char alt[64 * 1024];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0 };
    if (sigaltstack(&ss, NULL) == 0) sa.sa_flags |= SA_ONSTACK;
#endif
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP };
    for (int i = 0; i < 6; i++) sigaction(sigs[i], &sa, NULL);
}

// ---------------------------------------------------------------- the report (as quest_report.c's)
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) return;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
// a JSON string; bytes outside printable ASCII (but tabs and newlines) become '?', so the report is plain ASCII
static void jstr(Buf *b, const char *s, size_t n) {
    bputs(b, "\"");
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        char e[4];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; bput(b, e, 2); }
        else if (c == '\n') bputs(b, "\\n");
        else if (c == '\r') continue;
        else if (c == '\t') bputs(b, "\\t");
        else if (c < 0x20 || c >= 0x7f) bputs(b, "?");
        else bput(b, (const char *)&s[i], 1);
    }
    bputs(b, "\"");
}
static void jkey(Buf *b, const char *k, const char *v) { jstr(b, k, strlen(k)); bputs(b, ":"); jstr(b, v, strlen(v)); }
static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)(n > 0 ? n : 0) + 1);
    *len = s ? fread(s, 1, (size_t)(n > 0 ? n : 0), f) : 0;
    if (s) s[*len] = 0;
    fclose(f);
    return s;
}
static char *read_tail(const char *path, int lines, size_t max) {
    size_t n;
    char *s = read_all(path, &n);
    if (!s) return NULL;
    size_t start = n > max ? n - max : 0;
    int seen = 0;
    for (size_t i = n; i > start; i--) if (s[i - 1] == '\n' && ++seen > lines) { start = i; break; }
    memmove(s, s + start, n - start + 1);
    return s;
}
static void replace_all(char *s, const char *from, const char *to) {   // (to no longer than from)
    const size_t fl = strlen(from), tl = strlen(to);
    if (!fl || tl > fl) return;
    for (char *p = strstr(s, from); p; p = strstr(p + tl, from)) {
        memcpy(p, to, tl);
        memmove(p + tl, p + fl, strlen(p + fl) + 1);
    }
}
static void scrub(char *s) {   // the files folder -> <files>, then the home folder (the user's name on a Mac) -> ~
    replace_all(s, files_dir, "<files>");
    const char *home = NSHomeDirectory().UTF8String;
    if (home && strlen(home) > 1) replace_all(s, home, "~");
}
static void report_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/crash-reports/%s", files_dir, name); }
static void sysctl_str(const char *name, char *out, size_t n) {
    out[0] = 0;
    size_t len = n;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0) out[0] = 0;
    out[n - 1] = 0;
}

static void collect(const int *opt) {
    char p[1200];
    snprintf(p, sizeof p, "%s/crash.txt", files_dir);
    size_t len = 0;
    char *crash = read_all(p, &len);
    if (!crash || !len) { free(crash); return; }
    struct stat st;
    time_t when = stat(p, &st) == 0 ? st.st_mtime : time(NULL);
    char *c = crash, *last = NULL;
    for (char *q = strstr(c, "crash: "); q; q = strstr(q + 1, "crash: ")) if (q == crash || q[-1] == '\n') last = q;
    if (last) c = last;
    if (strlen(c) > REPORT_CRASH_MAX) c[REPORT_CRASH_MAX] = 0;
    char crashed_id[48] = "";
    const char *bl = strstr(c, "build-id ");
    if (bl) sscanf(bl + 9, "%40[0-9a-f]", crashed_id);
    snprintf(p, sizeof p, "%s/port.prev.log", files_dir);   // (the crashed run's: crash_apple_keep_log)
    char *log = read_tail(p, REPORT_LOG_LINES, REPORT_LOG_MAX);
    scrub(c);
    if (log) scrub(log);
    // the device: its model (hw.machine on iPhone, iPad, Apple TV and Vision Pro; hw.model on a Mac), its system
    char model[96], os[160], cpus[16];
#if TARGET_OS_OSX
    sysctl_str("hw.model", model, sizeof model);
#else
    sysctl_str("hw.machine", model, sizeof model);
    const char *sim = getenv("SIMULATOR_MODEL_IDENTIFIER");   // (the simulator's hw.machine is the Mac's CPU)
    if (sim && *sim) snprintf(model, sizeof model, "%s (simulator)", sim);
#endif
    snprintf(os, sizeof os, "%s", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String ?: "");
    snprintf(cpus, sizeof cpus, "%lu", (unsigned long)NSProcessInfo.processInfo.activeProcessorCount);
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    NSString *ver = info[@"CFBundleShortVersionString"], *bno = info[@"CFBundleVersion"];
    char t[64];
    struct tm tm;
    gmtime_r(&when, &tm);
    strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%SZ", &tm);
    Buf b = { 0 };
    bputs(&b, "{\"schema\":1,");
    jkey(&b, "platform", CRASH_PLATFORM); bputs(&b, ",");
    jkey(&b, "build_id", crashed_id[0] ? crashed_id : build_id); bputs(&b, ",");
    jkey(&b, "running_build_id", build_id); bputs(&b, ",");
    jkey(&b, "crash_time", t); bputs(&b, ",\"build_info\":{");
    jkey(&b, "app", app_name); bputs(&b, ",");
    jkey(&b, "version", HOTD_BUILD_VERSION[0] ? HOTD_BUILD_VERSION : ver ? ver.UTF8String : ""); bputs(&b, ",");
    jkey(&b, "build", bno ? bno.UTF8String : ""); bputs(&b, ",");
    jkey(&b, "commit", HOTD_BUILD_COMMIT); bputs(&b, "},\"device\":{");
    jkey(&b, "model", model); bputs(&b, ",");
    jkey(&b, "os", os); bputs(&b, ",");
    jkey(&b, "cpus", cpus); bputs(&b, "},\"settings\":{");
    for (int i = 0; i < QOPT_COUNT; i++) {
        char kv[96];
        snprintf(kv, sizeof kv, "%s\"%s\":%d", i ? "," : "", qopt_defs[i].key, opt[i]);
        bputs(&b, kv);
    }
    bputs(&b, "},\"crash\":");
    jstr(&b, c, strlen(c));
    bputs(&b, ",\"log\":");
    jstr(&b, log ? log : "", log ? strlen(log) : 0);
    bputs(&b, "}\n");
    free(crash);
    free(log);
    char dir[1100], name[64];
    snprintf(dir, sizeof dir, "%s/crash-reports", files_dir);
    mkdir(dir, 0775);
    strftime(name, sizeof name, "%Y%m%d-%H%M%S.json", &tm);
    report_path(p, sizeof p, name);
    FILE *f = b.p ? fopen(p, "wb") : NULL;
    int ok = f && fwrite(b.p, 1, b.n, f) == b.n;
    if (f && fclose(f)) ok = 0;
    free(b.p);
    if (!ok) { plat_log(PLAT_LOG_ERROR, "crash report: cannot write %s", name); return; }
    plat_log(PLAT_LOG_INFO, "crash report: %s", name);
    // emptied, not removed: the handler keeps it open for appending
    snprintf(p, sizeof p, "%s/crash.txt", files_dir);
    int fd = open(p, O_WRONLY | O_TRUNC);
    if (fd >= 0) close(fd);
}

// ---------------------------------------------------------------- send (NSURLSession)
static const char *relay_url(void) { const char *u = getenv("QUEST_CRASH_URL"); return u && *u ? u : REPORT_URL; }
// the status (-1: no answer), the body's start in resp
static int http_post(const char *url, const char *key, const char *body, size_t n, char *resp, size_t rn) {
    resp[0] = 0;
    NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    if (!u) return -1;
    NSMutableURLRequest *rq = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                  timeoutInterval:30];
    rq.HTTPMethod = @"POST";
    [rq setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [rq setValue:@"hotdvr-crash/1" forHTTPHeaderField:@"User-Agent"];
    if (key && *key) [rq setValue:[NSString stringWithUTF8String:key] forHTTPHeaderField:@"X-Report-Key"];
    rq.HTTPBody = [NSData dataWithBytes:body length:n];
    __block int code = -1;
    __block NSData *got = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionConfiguration *cfg = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    cfg.timeoutIntervalForRequest = 30;
    NSURLSession *s = [NSURLSession sessionWithConfiguration:cfg];
    [[s dataTaskWithRequest:rq completionHandler:^(NSData *data, NSURLResponse *r, NSError *err) {
        if (!err && [r isKindOfClass:NSHTTPURLResponse.class]) { code = (int)((NSHTTPURLResponse *)r).statusCode; got = data; }
        dispatch_semaphore_signal(done);
    }] resume];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));
    [s finishTasksAndInvalidate];
    if (got.length) {
        const size_t k = got.length < rn - 1 ? got.length : rn - 1;
        memcpy(resp, got.bytes, k);
        resp[k] = 0;
        for (char *q = resp; *q; q++) if (*q == '\r' || *q == '\n') *q = ' ';
    }
    return code;
}
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int ends_with(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !strcmp(s + a - b, t); }
static void send_all(void) {
    char dir[1100], p[1200], q[1200], resp[1024], line[1400];
    snprintf(dir, sizeof dir, "%s/crash-reports", files_dir);
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[256];
    int n = 0;
    for (struct dirent *de; (de = readdir(d)) && n < 256;)
        if (ends_with(de->d_name, ".json") && !ends_with(de->d_name, ".sent.json") && !ends_with(de->d_name, ".rejected.json"))
            names[n++] = strdup(de->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, by_name);
    const char *key = getenv("QUEST_CRASH_KEY");
    Buf result = { 0 };
    const int keep_from = n > REPORT_KEEP ? n - REPORT_KEEP : 0;   // too many waiting: the oldest go
    for (int i = 0; i < n; i++) {
        report_path(p, sizeof p, names[i]);
        if (i < keep_from) { remove(p); continue; }
        if (i >= keep_from + REPORT_SEND_MAX) continue;               // the oldest first, a few per start
        size_t len = 0;
        char *body = read_all(p, &len);
        if (!body) continue;
        const int code = http_post(relay_url(), key, body, len, resp, sizeof resp);
        free(body);
        const char *to = code >= 200 && code < 300 ? ".sent.json" : code >= 400 && code < 500 && code != 408 && code != 429 ? ".rejected.json" : NULL;
        if (to) {
            snprintf(q, sizeof q, "%s", p);
            q[strlen(q) - 5] = 0;
            strncat(q, to, sizeof q - strlen(q) - 1);
            rename(p, q);
        }
        snprintf(line, sizeof line, "%s: %d %s\n", names[i], code, resp);
        plat_log(PLAT_LOG_INFO, "crash report %s", line);
        bputs(&result, line);
    }
    if (result.p) {
        report_path(p, sizeof p, "last-result.txt");
        FILE *f = fopen(p, "w");
        if (f) { fputs(result.p, f); fclose(f); }
    }
    free(result.p);
    for (int i = 0; i < n; i++) free(names[i]);
}

// Before the front end opens this run's port.log: the last run's becomes port.prev.log (what a crash report sends).
void crash_apple_keep_log(const char *files) {
    char a[1100], b[1100];
    snprintf(a, sizeof a, "%s/port.log", files);
    snprintf(b, sizeof b, "%s/port.prev.log", files);
    rename(a, b);
}

// At start (the files folder made, env.txt and the settings read, before the game thread starts a new port.log):
// the handler in place, then a crash of the last run collected and the reports sent (reports on, a relay set).
void crash_apple_start(const char *files, const int *opt) {
    snprintf(files_dir, sizeof files_dir, "%s", files);
    find_app();
    crash_handler();
    plat_log(PLAT_LOG_INFO, "build %s (%s)", build_id, app_name);
    const char *ct = getenv("FLAT_CRASH_TEST");
    if (!ct || !*ct) ct = getenv("QUEST_CRASH_TEST");
    if (ct && atoi(ct) > 0) {
        const int s = atoi(ct);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)s * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            plat_log(PLAT_LOG_INFO, "crash test: crashing on purpose");
            *(volatile int *)(uintptr_t)8 = 1;
        });
    }
    if (!opt[QOPT_CRASH_REPORTS] || !*relay_url()) return;   // off, or nowhere to send: crash.txt stays as it is
    collect(opt);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ @autoreleasepool { send_all(); } });
}
