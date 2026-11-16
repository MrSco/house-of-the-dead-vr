// Host test of src/quest/quest_report.c's collect step (run.sh): a files folder with a crash.txt holding two crashes
// and a long port.log that names the folder; the report must hold only the last crash, the last 200 lines of the log
// with the folder written as <files>, the settings, the stub device, and crash.txt must be left empty.
#include "quest_report.h"
#include "quest_options.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---- stubs (stub/android/stubs.h)
static const char *build_info = "version=0.1\ncommit=8349ba5e61ee-dirty\nbuilt=2026-11-12T09:00:00Z\n";
AAsset *AAssetManager_open(AAssetManager *m, const char *name, int mode) {
    (void)m; (void)mode;
    if (strcmp(name, "build-info.txt")) return NULL;
    AAsset *a = calloc(1, sizeof *a); a->data = build_info; return a;
}
int AAsset_read(AAsset *a, void *buf, size_t n) {
    size_t left = strlen(a->data) - a->pos; if (n > left) n = left;
    memcpy(buf, a->data + a->pos, n); a->pos += (int)n; return (int)n;
}
void AAsset_close(AAsset *a) { free(a); }
int __system_property_get(const char *name, char *value) {
    static const char *const p[][2] = { { "ro.product.manufacturer", "Oculus" }, { "ro.product.model", "Quest 3" },
        { "ro.product.device", "eureka" }, { "ro.build.version.release", "14" }, { "ro.build.version.sdk", "34" },
        { "ro.build.display.id", "UP1A.231005.007.A1" } };
    for (unsigned i = 0; i < sizeof p / sizeof *p; i++) if (!strcmp(name, p[i][0])) { strcpy(value, p[i][1]); return (int)strlen(value); }
    value[0] = 0; return 0;
}

static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }
int main(int argc, char **argv) {
    if (argc < 4) fail("usage: test <files dir> <crash fixture> <out json>");
    const char *dir = argv[1];
    char p[600], fx[8192];
    FILE *f = fopen(argv[2], "r"); if (!f) fail("fixture");
    size_t n = fread(fx, 1, sizeof fx - 1, f); fx[n] = 0; fclose(f);
    snprintf(p, sizeof p, "%s/crash.txt", dir);
    f = fopen(p, "w");
    fprintf(f, "crash: signal 7 code 1 addr 0x1 thread 1\n  pc libhotdvr.so+0x10 (old_crash)\n%s", fx);
    fclose(f);
    snprintf(p, sizeof p, "%s/port.log", dir);
    f = fopen(p, "w");
    for (int i = 1; i <= 300; i++) fprintf(f, "line %d: open %s/game/ART/X%d.BMP \"quoted\" tab\there \x01 \xc3\xa9\n", i, dir, i);
    fclose(f);
    int opt[QOPT_COUNT];
    qoptions_defaults(opt);
    ANativeActivity act = { .vm = NULL, .assetManager = (AAssetManager *)&act };   // (any non-null: the stub ignores it; no vm: nothing is sent)
    struct stat st;
    snprintf(p, sizeof p, "%s/crash.txt", dir);
    qreport_start(&act, dir, opt);                                // no relay address: nothing happens
    if (stat(p, &st) || st.st_size == 0) fail("crash.txt touched without a relay");
    opt[QOPT_CRASH_REPORTS] = 0;
    setenv("QUEST_CRASH_URL", "http://127.0.0.1:9/report", 1);   // (no vm: nothing is sent)
    qreport_start(&act, dir, opt);                                // reports off: nothing happens
    if (stat(p, &st) || st.st_size == 0) fail("crash.txt touched with reports off");
    opt[QOPT_CRASH_REPORTS] = 1;
    qreport_start(&act, dir, opt);

    if (strlen(qreport_build_id()) != 40) fail("no build id");
    if (stat(p, &st) || st.st_size != 0) fail("crash.txt not emptied");
    snprintf(p, sizeof p, "%s/crash-reports", dir);
    DIR *d = opendir(p); if (!d) fail("no crash-reports");
    char name[300] = "";
    for (struct dirent *de; (de = readdir(d));) if (strstr(de->d_name, ".json")) snprintf(name, sizeof name, "%s", de->d_name);
    closedir(d);
    if (!name[0]) fail("no report");
    snprintf(p, sizeof p, "%s/crash-reports/%s", dir, name);
    f = fopen(p, "r"); static char rep[200000]; n = fread(rep, 1, sizeof rep - 1, f); rep[n] = 0; fclose(f);
    f = fopen(argv[3], "w"); fputs(rep, f); fclose(f);   // (looked at when a check fails)
    if (strstr(rep, "old_crash")) fail("older crash included");
    if (!strstr(rep, "qmenu_update")) fail("crash missing");
    if (strstr(rep, dir)) fail("files path not scrubbed");
    if (!strstr(rep, "line 300:") || !strstr(rep, "line 101:") || strstr(rep, "line 100:")) fail("log tail not the last 200 lines");
    if (!strstr(rep, "\"crash_reports\":1") || !strstr(rep, "\"model\":\"Quest 3\"") || !strstr(rep, "\"commit\":\"8349ba5e61ee-dirty\""))
        fail("fields");
    for (const unsigned char *c = (const unsigned char *)rep; *c; c++) if (*c >= 0x80) fail("non-ASCII byte");
    printf("ok: %s (%zu bytes)\n", name, n);
    return 0;
}
