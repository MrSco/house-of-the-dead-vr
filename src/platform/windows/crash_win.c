// Crash reports on Windows (the SYSTEM tab's Crash reports): as the Quest app's (src/quest/quest_report.c), the same
// JSON to the same relay (tools/vr/crash-relay), so one relay address serves both.
//  - The crash: an unhandled exception's handler appends to crash.txt in the files folder: "crash: " and the
//    exception, the faulting address as module+offset, the build id, and the stack as module+offset frames (dbghelp's
//    StackWalk64 on the exception's context). Nothing is sent from a crashed process.
//  - The next start: with reports on and a relay address, the last crash in crash.txt, the end of the crashed run's
//    port.log, the build, the settings and the Windows version become crash-reports/<time>.json (crash.txt emptied),
//    and every report not sent yet is POSTed (WinHTTP) on a thread of its own: accepted, <time>.sent.json; refused
//    for good (4xx), <time>.rejected.json; otherwise tried again next time. crash-reports/last-result.txt says how.
// The build id is the PDB's (its GUID and age, as a symbol server names it): build_windows.py keeps HOTDVR.pdb under
// build_windows/symbols/<build id>/. Nothing personal: paths in the files folder are written as <files>.
//
// The same exe runs flat and in a headset (PC VR): the report says which ("platform": windows or pcvr).
//
// env.txt: QUEST_CRASH_URL=https://... (the relay; overrides REPORT_URL), QUEST_CRASH_KEY=... (sent as X-Report-Key),
// FLAT_CRASH_TEST=seconds (crash on purpose that long after the start).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

static char files_dir[600];
static const char *platform_name = "windows";
static char build_id[64];
static wchar_t crash_path[MAX_PATH];

// ---------------------------------------------------------------- the build id: the PDB's GUID and age
const char *crash_win_build_id(void) {
    if (build_id[0]) return build_id;
    snprintf(build_id, sizeof build_id, "none");
    const BYTE *base = (const BYTE *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const IMAGE_DEBUG_DIRECTORY *d = (const IMAGE_DEBUG_DIRECTORY *)(base + dd->VirtualAddress);
    for (DWORD i = 0; dd->VirtualAddress && i < dd->Size / sizeof *d; i++) {
        if (d[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        const BYTE *cv = base + d[i].AddressOfRawData;
        if (memcmp(cv, "RSDS", 4)) continue;
        const GUID *g = (const GUID *)(cv + 4);
        const DWORD age = *(const DWORD *)(cv + 20);
        snprintf(build_id, sizeof build_id, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX", g->Data1, g->Data2, g->Data3,
                 g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7], age);
    }
    return build_id;
}

// ---------------------------------------------------------------- the crash
static void where(HANDLE f, DWORD64 pc) {
    HMODULE m = NULL;
    char line[400], name[MAX_PATH] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)pc, &m)) {
        char full[MAX_PATH];
        GetModuleFileNameA(m, full, sizeof full);
        const char *slash = strrchr(full, '\\');
        snprintf(name, sizeof name, "%s", slash ? slash + 1 : full);
    }
    snprintf(line, sizeof line, "%s+0x%llx", name, (unsigned long long)(pc - (DWORD64)(uintptr_t)m));
    DWORD w;
    WriteFile(f, line, (DWORD)strlen(line), &w, NULL);
}
static void put(HANDLE f, const char *s) { DWORD w; WriteFile(f, s, (DWORD)strlen(s), &w, NULL); }

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE f = CreateFileW(crash_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char line[300];
    const DWORD code = er->ExceptionCode;
    const char *what = code == EXCEPTION_ACCESS_VIOLATION ? "access violation" : code == EXCEPTION_STACK_OVERFLOW ? "stack overflow"
                       : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction" : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "divide by zero"
                       : code == 0xC0000409 ? "fast fail (stack buffer)" : "exception";
    snprintf(line, sizeof line, "crash: %s (0x%08lX)", what, code);
    put(f, line);
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        snprintf(line, sizeof line, ", %s 0x%llx", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "running" : "reading",
                 (unsigned long long)er->ExceptionInformation[1]);
        put(f, line);
    }
    put(f, " at ");
    where(f, (DWORD64)(uintptr_t)er->ExceptionAddress);
    snprintf(line, sizeof line, ", thread %lu\r\nbuild-id %s\r\nbacktrace:\r\n", GetCurrentThreadId(), crash_win_build_id());
    put(f, line);
    // the stack, from the exception's context
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof sf);
    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
    SymInitialize(proc, NULL, TRUE);
    for (int i = 0; i < 48; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &sf, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        if (!sf.AddrPC.Offset) break;
        snprintf(line, sizeof line, "  #%02d pc ", i);
        put(f, line);
        where(f, sf.AddrPC.Offset);
        put(f, "\r\n");
    }
    CloseHandle(f);
    plat_log(PLAT_LOG_ERROR, "crash: written to crash.txt");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;   // (Windows ends the process as it would)
}

// ---------------------------------------------------------------- the report (as quest_report.c's)
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void jstr(Buf *b, const char *s, size_t n) {
    bputs(b, "\"");
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; bput(b, e, 2); }
        else if (c == '\n') bputs(b, "\\n");
        else if (c == '\r') continue;
        else if (c == '\t') bputs(b, "\\t");
        else if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); bputs(b, e); }
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
static void scrub(char *s) {   // the files folder's path (either slash) -> <files>
    char a[600], b[600];
    snprintf(a, sizeof a, "%s", files_dir);
    snprintf(b, sizeof b, "%s", files_dir);
    for (char *p = b; *p; p++) if (*p == '\\') *p = '/';
    for (const char *from = a;; from = b) {
        const size_t fl = strlen(from);
        for (char *q = fl ? strstr(s, from) : NULL; q; q = strstr(q, from)) {
            memcpy(q, "<files>", 7);
            memmove(q + 7, q + fl, strlen(q + fl) + 1);
        }
        if (from == b) break;
    }
}
static void report_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/crash-reports/%s", files_dir, name); }

static void collect(const int *opt) {
    char p[800];
    snprintf(p, sizeof p, "%s/crash.txt", files_dir);
    size_t len = 0;
    char *crash = read_all(p, &len);
    if (!crash || !len) { free(crash); return; }
    char *c = crash, *last = NULL;
    for (char *q = strstr(c, "crash: "); q; q = strstr(q + 1, "crash: ")) if (q == crash || q[-1] == '\n') last = q;
    if (last) c = last;
    if (strlen(c) > REPORT_CRASH_MAX) c[REPORT_CRASH_MAX] = 0;
    char crashed_id[64] = "";
    const char *bl = strstr(c, "build-id ");
    if (bl) sscanf(bl + 9, "%63[0-9A-Fa-f]", crashed_id);
    snprintf(p, sizeof p, "%s/port.log", files_dir);
    char *log = read_tail(p, REPORT_LOG_LINES, REPORT_LOG_MAX);
    scrub(c);
    if (log) scrub(log);
    // the Windows version (RtlGetVersion: GetVersionEx is shimmed) and the machine
    OSVERSIONINFOW v = { sizeof v };
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOW *);
    PFN_RtlGetVersion rgv = (PFN_RtlGetVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv) rgv(&v);
    char os[64], cpus[16];
    snprintf(os, sizeof os, "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    snprintf(cpus, sizeof cpus, "%lu", si.dwNumberOfProcessors);
    time_t when = time(NULL);
    struct tm tm;
    gmtime_s(&tm, &when);
    char t[64];
    strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%SZ", &tm);
    Buf b = { 0 };
    bputs(&b, "{\"schema\":1,");
    jkey(&b, "platform", platform_name); bputs(&b, ",");
    jkey(&b, "build_id", crashed_id[0] ? crashed_id : crash_win_build_id()); bputs(&b, ",");
    jkey(&b, "running_build_id", crash_win_build_id()); bputs(&b, ",");
    jkey(&b, "crash_time", t); bputs(&b, ",\"build_info\":{");
    jkey(&b, "app", "HOTDVR.exe"); bputs(&b, ",");
    jkey(&b, "version", HOTD_BUILD_VERSION); bputs(&b, ",");
    jkey(&b, "commit", HOTD_BUILD_COMMIT); bputs(&b, "},\"device\":{");
    jkey(&b, "os", "windows"); bputs(&b, ",");
    jkey(&b, "windows", os); bputs(&b, ",");
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
    char dir[700], name[64];
    snprintf(dir, sizeof dir, "%s/crash-reports", files_dir);
    plat_mkdir(dir);
    strftime(name, sizeof name, "%Y%m%d-%H%M%S.json", &tm);
    report_path(p, sizeof p, name);
    FILE *f = fopen(p, "wb");
    const int ok = f && fwrite(b.p, 1, b.n, f) == b.n;
    if (f) fclose(f);
    free(b.p);
    if (!ok) { plat_log(PLAT_LOG_ERROR, "crash report: cannot write %s", p); return; }
    plat_log(PLAT_LOG_INFO, "crash report: %s", name);
    snprintf(p, sizeof p, "%s/crash.txt", files_dir);
    FILE *e = fopen(p, "wb");   // (emptied)
    if (e) fclose(e);
}

// ---------------------------------------------------------------- send (WinHTTP)
static const char *relay_url(void) { const char *u = getenv("QUEST_CRASH_URL"); return u && *u ? u : REPORT_URL; }
// the status (-1: no answer), the body's start in resp
static int http_post(const char *url, const char *key, const char *body, size_t n, char *resp, size_t rn) {
    resp[0] = 0;
    wchar_t wurl[1024], host[256], path[768];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 768;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return -1;
    int code = -1;
    HINTERNET s = WinHttpOpen(L"hotdvr-crash/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (r) {
        WinHttpSetTimeouts(r, 10000, 10000, 15000, 15000);
        wchar_t hdr[400];
        swprintf(hdr, 400, L"Content-Type: application/json\r\n%s%S%s", key && *key ? L"X-Report-Key: " : L"", key && *key ? key : "",
                 key && *key ? L"\r\n" : L"");
        if (WinHttpSendRequest(r, hdr, (DWORD)-1L, (LPVOID)body, (DWORD)n, (DWORD)n, 0) && WinHttpReceiveResponse(r, NULL)) {
            DWORD st = 0, sz = sizeof st;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX))
                code = (int)st;
            DWORD got = 0;
            if (WinHttpReadData(r, resp, (DWORD)rn - 1, &got)) resp[got] = 0;
            for (char *q = resp; *q; q++) if (*q == '\r' || *q == '\n') *q = ' ';
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return code;
}
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int ends_with(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !strcmp(s + a - b, t); }
static int send_thread(void *arg) {
    (void)arg;
    char pat[800], p[800], q[800], resp[1024], line[1400];
    snprintf(pat, sizeof pat, "%s/crash-reports/*.json", files_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    char *names[256];
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!ends_with(fd.cFileName, ".sent.json") && !ends_with(fd.cFileName, ".rejected.json") && n < 256) names[n++] = _strdup(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(names, (size_t)n, sizeof *names, by_name);
    const char *key = getenv("QUEST_CRASH_KEY");
    Buf result = { 0 };
    const int keep_from = n > REPORT_KEEP ? n - REPORT_KEEP : 0;
    for (int i = 0; i < n; i++) {
        report_path(p, sizeof p, names[i]);
        if (i < keep_from) { remove(p); continue; }
        if (i >= keep_from + REPORT_SEND_MAX) continue;
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
    return 0;
}
static int crash_test(void *arg) {
    plat_sleep_until_ns(plat_now_ns() + (uint64_t)(uintptr_t)arg * 1000000000ull);
    plat_log(PLAT_LOG_INFO, "FLAT_CRASH_TEST: crashing on purpose");
    *(volatile int *)(uintptr_t)8 = 1;
    return 0;
}

// At start (platform: "windows" flat, "pcvr" in a headset; the files folder made, env.txt and the settings read, before the game thread starts a new port.log):
// the handler in place, then a crash of the last run collected and the reports sent (reports on, a relay set).
void crash_win_start(const char *files, const int *opt, const char *platform) {
    snprintf(files_dir, sizeof files_dir, "%s", files);
    platform_name = platform;
    char p[700];
    snprintf(p, sizeof p, "%s/crash.txt", files);
    MultiByteToWideChar(CP_UTF8, 0, p, -1, crash_path, MAX_PATH);
    crash_win_build_id();
    SetUnhandledExceptionFilter(on_crash);
    plat_log(PLAT_LOG_INFO, "build %s", build_id);
    const char *ct = getenv("FLAT_CRASH_TEST");
    if (!ct || !*ct) ct = getenv("QUEST_CRASH_TEST");
    if (ct && atoi(ct) > 0) plat_thread_start(crash_test, (void *)(uintptr_t)atoi(ct));
    if (!opt[QOPT_CRASH_REPORTS] || !*relay_url()) return;   // off, or nowhere to send: crash.txt stays as it is
    collect(opt);
    plat_thread_start(send_thread, NULL);
}
