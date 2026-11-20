// Crash reports (quest_report.h), for every Android build: the Quest and the Steam Frame (src/quest/quest_main.c), and
// the phones and tablets (src/flat/android/main_android.c); the package tells them apart ("platform" in the report).
//
// A crash can't send anything itself (the handler below only appends to crash.txt), so the next start does it:
//  1. collect (at start, before the game thread opens a new port.log): the last crash in crash.txt, the last
//     REPORT_LOG_LINES lines of the crashed run's port.log, the build, the settings and the headset's model and OS
//     version go into crash-reports/<time>.json; crash.txt is emptied (the report keeps its text).
//  2. send (a background thread): each pending report is POSTed as JSON to the crash relay (tools/vr/crash-relay),
//     through the framework's java.net.HttpURLConnection over JNI (the app has no Java code of its own and no native
//     HTTP / TLS). Accepted: renamed <time>.sent.json; refused for good (4xx): <time>.rejected.json; no network or a
//     server error: tried again at the next start. crash-reports/last-result.txt says what happened.
// Nothing personal is in a report: no account, no file paths outside the app's own folder (written as <files>), no
// serial numbers. The relay holds the GitHub token; the app only knows the relay's address.
//
// env.txt: QUEST_CRASH_URL=https://... (the relay; overrides REPORT_URL below), QUEST_CRASH_KEY=... (sent as
// X-Report-Key when the relay asks for one), QUEST_CRASH_TEST=seconds (FLAT_CRASH_TEST on the phone: crash on purpose
// that long after the start, to try the whole path: take it out again afterwards).
#include "quest_report.h"
#include "quest_options.h"
#include <android/asset_manager.h>
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define TAG "hotdvr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// The crash relay's address (tools/vr/crash-relay: the project's Worker). Empty (and no QUEST_CRASH_URL in env.txt):
// no reports at all, crash.txt is left as it is.
#define REPORT_URL "https://hotdvr.roccojuliano.com/v1/reports"
#define REPORT_LOG_LINES 200
#define REPORT_CRASH_MAX (16 * 1024)       // the last crash in crash.txt, at most
#define REPORT_LOG_MAX (48 * 1024)         // port.log's tail, at most
#define REPORT_KEEP 10                     // pending reports kept (the oldest beyond are dropped)
#define REPORT_SEND_MAX 5                  // reports sent per start

// ---------------------------------------------------------------- the build id (the GNU build-id note)
static char build_id[41];
static uintptr_t self_base;
static int note_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (info->dlpi_addr != self_base) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_NOTE) continue;
        const uint8_t *p = (const uint8_t *)(info->dlpi_addr + ph->p_vaddr), *end = p + ph->p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) *n = (const ElfW(Nhdr) *)p;
            const uint8_t *name = p + sizeof *n, *desc = name + ((n->n_namesz + 3) & ~3u);
            if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 && !memcmp(name, "GNU", 4) && n->n_descsz <= 20) {
                for (unsigned k = 0; k < n->n_descsz; k++) snprintf(build_id + k * 2, 3, "%02x", desc[k]);
                return 1;
            }
            p = desc + ((n->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}
const char *qreport_build_id(void) {
    if (!build_id[0] && !self_base) {
        Dl_info di;
        if (dladdr((void *)qreport_build_id, &di) && di.dli_fbase) {
            self_base = (uintptr_t)di.dli_fbase;
            dl_iterate_phdr(note_cb, NULL);
        }
    }
    return build_id;
}

// ---------------------------------------------------------------- crashes -> crash.txt
// A crash (a bad pointer, an abort) appends to crash.txt in the files folder: the signal, the faulting address and
// the call chain (pc, lr, then the frame pointers), each as library+offset (and the nearest dynamic symbol), which
// llvm-symbolizer turns into source lines with the unstripped libhotdvr.so the build-id line names. Then the default
// action runs (the system's own report). The next start sends it (qreport_start) when Crash reports is on.
static int crash_fd = -1;
static char crash_build[64];
static void crash_put(const char *s) { if (crash_fd >= 0 && write(crash_fd, s, strlen(s)) < 0) {} }
static void crash_addr(const char *what, uintptr_t a) {
    char b[300];
    Dl_info di;
    if (a && dladdr((void *)a, &di) && di.dli_fname) {
        const char *f = strrchr(di.dli_fname, '/');
        snprintf(b, sizeof b, "  %s %s+0x%lx (%s)\n", what, f ? f + 1 : di.dli_fname, (unsigned long)(a - (uintptr_t)di.dli_fbase),
                 di.dli_sname ? di.dli_sname : "?");
    } else snprintf(b, sizeof b, "  %s %p\n", what, (void *)a);
    crash_put(b);
}
static void on_crash(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    char b[200];
    snprintf(b, sizeof b, "crash: signal %d code %d addr %p thread %d\n", sig, si->si_code, si->si_addr, gettid());
    crash_put(b);
    crash_put(crash_build);
#if defined(__aarch64__)
    crash_addr("pc", (uintptr_t)uc->uc_mcontext.pc);
    crash_addr("lr", (uintptr_t)uc->uc_mcontext.regs[30]);
    uintptr_t *fp = (uintptr_t *)uc->uc_mcontext.regs[29];
    for (int i = 0; i < 48 && fp && ((uintptr_t)fp & 7) == 0; i++) {
        uintptr_t next = fp[0], ret = fp[1];
        crash_addr("at", ret);
        if (next <= (uintptr_t)fp) break;
        fp = (uintptr_t *)next;
    }
#else
    (void)uc; (void)crash_addr;
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}
void qreport_handler(const char *dir) {
    char p[600];
    snprintf(p, sizeof p, "%s/crash.txt", dir);
    crash_fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0660);
    snprintf(crash_build, sizeof crash_build, "  build-id %s\n", qreport_build_id());
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (int i = 0; i < 5; i++) sigaction(sigs[i], &sa, NULL);
}

// ---------------------------------------------------------------- a growing buffer, JSON
typedef struct { char *p; size_t n, cap; } Buf;
static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) return;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }
// a JSON string; bytes outside printable ASCII (but tabs and newlines) become '?', so the report is plain ASCII
static void jstr(Buf *b, const char *s, size_t n) {
    put(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; put(b, e, 2); }
        else if (c == '\n') put(b, "\\n", 2);
        else if (c == '\t') put(b, "\\t", 2);
        else if (c == '\r') continue;
        else if (c < 0x20 || c >= 0x7f) put(b, "?", 1);
        else put(b, (const char *)&c, 1);
    }
    put(b, "\"", 1);
}
static void jkey(Buf *b, const char *k, const char *v) { jstr(b, k, strlen(k)); put(b, ":", 1); jstr(b, v, strlen(v)); }

// ---------------------------------------------------------------- reading
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (p) { n = (long)fread(p, 1, (size_t)n, f); p[n] = 0; if (len) *len = (size_t)n; }
    fclose(f);
    return p;
}
// the last `lines` lines of a file, at most `max` bytes
static char *read_tail(const char *path, int lines, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), from = size > (long)max ? size - (long)max : 0;
    fseek(f, from, SEEK_SET);
    char *p = malloc(max + 1);
    size_t n = p ? fread(p, 1, max, f) : 0;
    fclose(f);
    if (!p) return NULL;
    p[n] = 0;
    char *s = p;
    if (from > 0) { char *nl = strchr(s, '\n'); s = nl ? nl + 1 : s; }   // a whole first line
    int count = 0;
    for (char *e = p + n; e > s; e--) if (e[-1] == '\n' && e != p + n && ++count >= lines) { s = e; break; }
    memmove(p, s, strlen(s) + 1);
    return p;
}
// every `from` in s replaced by `to` (in place, `to` no longer than `from`)
static void replace_all(char *s, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to);
    if (!fl || tl > fl) return;
    for (char *p = strstr(s, from); p; p = strstr(p + tl, from)) {
        memcpy(p, to, tl);
        memmove(p + tl, p + fl, strlen(p + fl) + 1);
    }
}
static void scrub(char *s, const char *files) {
    replace_all(s, files, "<files>");
    // the same folder under its other name
    const char *a = "/storage/emulated/0/", *b = "/sdcard/";
    char alt[600];
    if (!strncmp(files, a, strlen(a))) snprintf(alt, sizeof alt, "%s%s", b, files + strlen(a));
    else if (!strncmp(files, b, strlen(b))) snprintf(alt, sizeof alt, "%s%s", a, files + strlen(b));
    else return;
    replace_all(s, alt, "<files>");
}
static void prop(const char *name, char *out) {
    out[0] = 0;
    __system_property_get(name, out);
}

// ---------------------------------------------------------------- collect
static char files_dir[512];
// which app this is: the package's files folder (Android/data/<package>/files) tells (the relay's PLATFORMS)
static const char *platform(void) {
    return strstr(files_dir, ".frame/") ? "frame" : strstr(files_dir, ".android/") ? "android" : "quest";
}
static void path_of(char *out, size_t n, const char *name) { snprintf(out, n, "%s/crash-reports/%s", files_dir, name); }

static void collect(ANativeActivity *act, const int *opt) {
    char p[700];
    snprintf(p, sizeof p, "%s/crash.txt", files_dir);
    size_t len = 0;
    char *crash = read_file(p, &len);
    if (!crash || !len) { free(crash); return; }
    struct stat st;
    time_t when = stat(p, &st) == 0 ? st.st_mtime : time(NULL);
    // the last crash in it (one run's; the lines before belong to earlier ones)
    char *c = crash, *last = NULL;
    for (char *q = strstr(c, "crash: "); q; q = strstr(q + 1, "crash: ")) if (q == crash || q[-1] == '\n') last = q;
    if (last) c = last;
    if (strlen(c) > REPORT_CRASH_MAX) c[REPORT_CRASH_MAX] = 0;
    // the build that crashed: the handler writes its build id (it may differ from this one after an update)
    char crashed_id[41] = "";
    const char *bl = strstr(c, "build-id ");
    if (bl) sscanf(bl + 9, "%40[0-9a-f]", crashed_id);
    char *log = NULL;
    snprintf(p, sizeof p, "%s/port.log", files_dir);
    log = read_tail(p, REPORT_LOG_LINES, REPORT_LOG_MAX);
    scrub(c, files_dir);
    if (log) scrub(log, files_dir);
    // the APK's build-info.txt (scripts/quest_tools.py): commit, version, build time
    char info[512] = "";
    AAsset *as = act && act->assetManager ? AAssetManager_open(act->assetManager, "build-info.txt", AASSET_MODE_BUFFER) : NULL;
    if (as) { int n = AAsset_read(as, info, sizeof info - 1); info[n > 0 ? n : 0] = 0; AAsset_close(as); }

    Buf b = { 0 };
    char t[64];
    struct tm tm;
    gmtime_r(&when, &tm);
    strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%SZ", &tm);
    puts_(&b, "{\"schema\":1,");
    jkey(&b, "platform", platform()); puts_(&b, ",");
    jkey(&b, "build_id", crashed_id[0] ? crashed_id : qreport_build_id()); puts_(&b, ",");
    jkey(&b, "running_build_id", qreport_build_id()); puts_(&b, ",");
    jkey(&b, "crash_time", t); puts_(&b, ",");
    puts_(&b, "\"build_info\":{");
    int first = 1;
    char *save = NULL;
    for (char *line = strtok_r(info, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!first) puts_(&b, ",");
        jkey(&b, line, eq + 1); first = 0;
    }
    puts_(&b, "},\"device\":{");
    static const char *const props[][2] = {
        { "manufacturer", "ro.product.manufacturer" }, { "model", "ro.product.model" }, { "device", "ro.product.device" },
        { "android", "ro.build.version.release" }, { "sdk", "ro.build.version.sdk" }, { "os_build", "ro.build.display.id" },
    };
    for (int i = 0; i < (int)(sizeof props / sizeof *props); i++) {
        char v[PROP_VALUE_MAX];
        prop(props[i][1], v);
        if (i) puts_(&b, ",");
        jkey(&b, props[i][0], v);
    }
    puts_(&b, "},\"settings\":{");
    for (int i = 0; i < QOPT_COUNT; i++) {
        char v[80];
        snprintf(v, sizeof v, "%s\"%s\":%d", i ? "," : "", qopt_defs[i].key, opt[i]);
        puts_(&b, v);
    }
    puts_(&b, "},\"crash\":");
    jstr(&b, c, strlen(c));
    puts_(&b, ",\"log\":");
    jstr(&b, log ? log : "", log ? strlen(log) : 0);
    puts_(&b, "}\n");
    free(crash); free(log);

    char dir[600], name[64];
    snprintf(dir, sizeof dir, "%s/crash-reports", files_dir);
    mkdir(dir, 0775);
    strftime(name, sizeof name, "%Y%m%d-%H%M%S.json", &tm);
    path_of(p, sizeof p, name);
    FILE *f = b.p ? fopen(p, "wb") : NULL;
    int ok = f && fwrite(b.p, 1, b.n, f) == b.n;
    if (f && fclose(f)) ok = 0;
    free(b.p);
    if (!ok) { LOGI("crash report: cannot write %s", p); return; }
    LOGI("crash report: %s", p);
    // emptied, not removed: quest_xr.c's handler keeps it open for appending
    snprintf(p, sizeof p, "%s/crash.txt", files_dir);
    int fd = open(p, O_WRONLY | O_TRUNC);
    if (fd >= 0) close(fd);
}

// ---------------------------------------------------------------- send (JNI: java.net.HttpURLConnection)
// The response's status (-1: no answer, the network or the address), its body in resp.
static int http_post(JavaVM *vm, const char *url, const char *key, const char *body, size_t n, char *resp, size_t rn) {
    JNIEnv *e;
    resp[0] = 0;
    if ((*vm)->AttachCurrentThread(vm, (void *)&e, NULL) != JNI_OK) return -1;
    int code = -1;
    jobject conn = NULL;
    jmethodID m_disc = NULL;
    (*e)->PushLocalFrame(e, 64);
#define J(x) x; if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); goto done; }
    J(jclass cUrl = (*e)->FindClass(e, "java/net/URL"))
    J(jclass cHttp = (*e)->FindClass(e, "java/net/HttpURLConnection"))
    J(jclass cOut = (*e)->FindClass(e, "java/io/OutputStream"))
    J(jclass cIn = (*e)->FindClass(e, "java/io/InputStream"))
    J(jmethodID m_new = (*e)->GetMethodID(e, cUrl, "<init>", "(Ljava/lang/String;)V"))
    J(jmethodID m_open = (*e)->GetMethodID(e, cUrl, "openConnection", "()Ljava/net/URLConnection;"))
    J(jmethodID m_method = (*e)->GetMethodID(e, cHttp, "setRequestMethod", "(Ljava/lang/String;)V"))
    J(jmethodID m_prop = (*e)->GetMethodID(e, cHttp, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"))
    J(jmethodID m_doout = (*e)->GetMethodID(e, cHttp, "setDoOutput", "(Z)V"))
    J(jmethodID m_ctime = (*e)->GetMethodID(e, cHttp, "setConnectTimeout", "(I)V"))
    J(jmethodID m_rtime = (*e)->GetMethodID(e, cHttp, "setReadTimeout", "(I)V"))
    J(jmethodID m_fixed = (*e)->GetMethodID(e, cHttp, "setFixedLengthStreamingMode", "(I)V"))
    J(jmethodID m_getout = (*e)->GetMethodID(e, cHttp, "getOutputStream", "()Ljava/io/OutputStream;"))
    J(jmethodID m_code = (*e)->GetMethodID(e, cHttp, "getResponseCode", "()I"))
    J(jmethodID m_getin = (*e)->GetMethodID(e, cHttp, "getInputStream", "()Ljava/io/InputStream;"))
    J(jmethodID m_geterr = (*e)->GetMethodID(e, cHttp, "getErrorStream", "()Ljava/io/InputStream;"))
    J(m_disc = (*e)->GetMethodID(e, cHttp, "disconnect", "()V"))
    J(jmethodID m_write = (*e)->GetMethodID(e, cOut, "write", "([B)V"))
    J(jmethodID m_oclose = (*e)->GetMethodID(e, cOut, "close", "()V"))
    J(jmethodID m_read = (*e)->GetMethodID(e, cIn, "read", "([B)I"))
    J(jmethodID m_iclose = (*e)->GetMethodID(e, cIn, "close", "()V"))
    J(jobject u = (*e)->NewObject(e, cUrl, m_new, (*e)->NewStringUTF(e, url)))
    J(conn = (*e)->CallObjectMethod(e, u, m_open))
    if (!(*e)->IsInstanceOf(e, conn, cHttp)) goto done;
    J((*e)->CallVoidMethod(e, conn, m_method, (*e)->NewStringUTF(e, "POST")))
    J((*e)->CallVoidMethod(e, conn, m_prop, (*e)->NewStringUTF(e, "Content-Type"), (*e)->NewStringUTF(e, "application/json")))
    J((*e)->CallVoidMethod(e, conn, m_prop, (*e)->NewStringUTF(e, "User-Agent"), (*e)->NewStringUTF(e, "hotdvr-crash/1")))
    if (key && *key) { J((*e)->CallVoidMethod(e, conn, m_prop, (*e)->NewStringUTF(e, "X-Report-Key"), (*e)->NewStringUTF(e, key))) }
    J((*e)->CallVoidMethod(e, conn, m_doout, JNI_TRUE))
    J((*e)->CallVoidMethod(e, conn, m_ctime, 15000))
    J((*e)->CallVoidMethod(e, conn, m_rtime, 30000))
    J((*e)->CallVoidMethod(e, conn, m_fixed, (jint)n))
    J(jbyteArray arr = (*e)->NewByteArray(e, (jsize)n))
    J((*e)->SetByteArrayRegion(e, arr, 0, (jsize)n, (const jbyte *)body))
    J(jobject os = (*e)->CallObjectMethod(e, conn, m_getout))
    J((*e)->CallVoidMethod(e, os, m_write, arr))
    J((*e)->CallVoidMethod(e, os, m_oclose))
    J(code = (*e)->CallIntMethod(e, conn, m_code))
    {
        jobject is = (*e)->CallObjectMethod(e, conn, code < 400 ? m_getin : m_geterr);
        if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); is = NULL; }
        if (is) {
            jbyteArray rb = (*e)->NewByteArray(e, 1024);
            size_t got = 0;
            while (rb && got + 1 < rn) {
                jint k = (*e)->CallIntMethod(e, is, m_read, rb);
                if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); break; }
                if (k <= 0) break;
                if ((size_t)k > rn - 1 - got) k = (jint)(rn - 1 - got);
                (*e)->GetByteArrayRegion(e, rb, 0, k, (jbyte *)resp + got);
                got += (size_t)k;
            }
            resp[got] = 0;
            (*e)->CallVoidMethod(e, is, m_iclose);
            if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
        }
    }
#undef J
done:
    if (conn && m_disc) { (*e)->CallVoidMethod(e, conn, m_disc); if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e); }
    (*e)->PopLocalFrame(e, NULL);
    (*vm)->DetachCurrentThread(vm);
    return code;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int ends_with(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !strcmp(s + a - b, t); }

static const char *relay_url(void) {
    const char *u = getenv("QUEST_CRASH_URL");
    return u && *u ? u : REPORT_URL;
}
static JavaVM *jvm;
static void *send_thread(void *arg) {
    (void)arg;
    char dir[600];
    snprintf(dir, sizeof dir, "%s/crash-reports", files_dir);
    DIR *d = opendir(dir);
    if (!d) return NULL;
    char *names[256]; int n = 0;
    for (struct dirent *de; (de = readdir(d)) && n < 256;)
        if (ends_with(de->d_name, ".json") && !ends_with(de->d_name, ".sent.json") && !ends_with(de->d_name, ".rejected.json"))
            names[n++] = strdup(de->d_name);
    closedir(d);
    qsort(names, n, sizeof *names, by_name);
    const char *url = relay_url();
    const char *key = getenv("QUEST_CRASH_KEY");
    char p[700], q[700], resp[1024], line[1400];
    Buf result = { 0 };
    const int keep_from = n > REPORT_KEEP ? n - REPORT_KEEP : 0;   // too many waiting: the oldest go
    for (int i = 0; i < n; i++) {
        path_of(p, sizeof p, names[i]);
        if (i < keep_from) { remove(p); continue; }
        if (i >= keep_from + REPORT_SEND_MAX) continue;  // the oldest first, a few per start
        size_t len = 0;
        char *body = read_file(p, &len);
        if (!body) continue;
        int code = http_post(jvm, url, key, body, len, resp, sizeof resp);
        free(body);
        const char *to = code >= 200 && code < 300 ? ".sent.json" : code >= 400 && code < 500 && code != 408 && code != 429 ? ".rejected.json" : NULL;
        if (to) {
            snprintf(q, sizeof q, "%s", p);
            q[strlen(q) - 5] = 0;
            strncat(q, to, sizeof q - strlen(q) - 1);
            rename(p, q);
        }
        snprintf(line, sizeof line, "%s: %d %s\n", names[i], code, resp);
        LOGI("crash report %s", line);
        puts_(&result, line);
    }
    if (result.p) {
        path_of(p, sizeof p, "last-result.txt");
        FILE *f = fopen(p, "w");
        if (f) { fputs(result.p, f); fclose(f); }
    }
    free(result.p);
    for (int i = 0; i < n; i++) free(names[i]);
    return NULL;
}

static void *crash_test(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    LOGI("crash test: crashing on purpose");
    *(volatile int *)(uintptr_t)8 = 1;
    return NULL;
}

void qreport_start(ANativeActivity *act, const char *files, const int *opt) {
    snprintf(files_dir, sizeof files_dir, "%s", files);
    qreport_build_id();
    // off, or no relay to send to: crash.txt stays as it is (as it was before reports), nothing is sent
    if (!opt[QOPT_CRASH_REPORTS] || !*relay_url()) return;
    collect(act, opt);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (act && act->vm) { jvm = act->vm; pthread_create(&t, &at, send_thread, NULL); }
    const char *ct = getenv("QUEST_CRASH_TEST");
    if (!ct || !*ct) ct = getenv("FLAT_CRASH_TEST");
    if (ct && atoi(ct) > 0) pthread_create(&t, &at, crash_test, (void *)(uintptr_t)atoi(ct));
    pthread_attr_destroy(&at);
}
