// Host test of src/quest/quest_report.c's sending (send.sh): the same JNI calls into java.net.HttpURLConnection as on
// the headset, made in a desktop JVM, against a local server (send_server.py) that accepts the first report and
// refuses the second (4xx); a third waits because nothing answers at the second address.
#include "quest_report.h"
#include "quest_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

AAsset *AAssetManager_open(AAssetManager *m, const char *name, int mode) { (void)m; (void)name; (void)mode; return NULL; }
int AAsset_read(AAsset *a, void *buf, size_t n) { (void)a; (void)buf; (void)n; return 0; }
void AAsset_close(AAsset *a) { (void)a; }
int __system_property_get(const char *name, char *value) { (void)name; value[0] = 0; return 0; }

static const char *g_dir = ".";
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    char p[700]; snprintf(p, sizeof p, "%s/crash-reports/last-result.txt", g_dir);
    FILE *f = fopen(p, "r"); int c;
    if (f) { while ((c = fgetc(f)) != EOF) fputc(c, stderr); fclose(f); }
    exit(1);
}
static int exists(const char *dir, const char *name) {
    char p[700]; struct stat st; snprintf(p, sizeof p, "%s/crash-reports/%s", dir, name); return stat(p, &st) == 0;
}
static void pending(const char *dir, const char *name, const char *body) {
    char p[700]; snprintf(p, sizeof p, "%s/crash-reports", dir); mkdir(p, 0775);
    snprintf(p, sizeof p, "%s/crash-reports/%s", dir, name);
    FILE *f = fopen(p, "w"); fputs(body, f); fclose(f);
}
static void wait_for(const char *dir, const char *name) {
    for (int i = 0; i < 200 && !exists(dir, name); i++) usleep(50000);
}
int main(int argc, char **argv) {
    if (argc < 4) fail("usage: send <files dir> <url> <dead url>");
    const char *dir = argv[1];
    g_dir = dir;
    JavaVM *vm; JNIEnv *env;
    JavaVMInitArgs va = { .version = JNI_VERSION_1_8 };
    if (JNI_CreateJavaVM(&vm, (void **)&env, &va) != JNI_OK) fail("no JVM");
    int opt[QOPT_COUNT];
    qoptions_defaults(opt);
    ANativeActivity act = { .vm = vm };

    pending(dir, "20260101-000001.json", "{\"schema\":1,\"accept\":true}\n");
    pending(dir, "20260101-000002.json", "{\"schema\":1,\"accept\":false}\n");
    setenv("QUEST_CRASH_URL", argv[2], 1);
    setenv("QUEST_CRASH_KEY", "test-key", 1);
    qreport_start(&act, dir, opt);
    wait_for(dir, "20260101-000002.rejected.json");
    if (!exists(dir, "20260101-000001.sent.json")) fail("accepted report not marked sent");
    if (!exists(dir, "20260101-000002.rejected.json")) fail("refused report not marked rejected");

    pending(dir, "20260101-000003.json", "{\"schema\":1}\n");
    setenv("QUEST_CRASH_URL", argv[3], 1);
    qreport_start(&act, dir, opt);
    wait_for(dir, "last-result.txt.done");   // (never made: just the time the try takes)
    if (!exists(dir, "20260101-000003.json")) fail("report not kept when nothing answers");
    if (exists(dir, "20260101-000003.sent.json") || exists(dir, "20260101-000003.rejected.json")) fail("unanswered report marked");

    char p[700]; snprintf(p, sizeof p, "%s/crash-reports/last-result.txt", dir);
    FILE *f = fopen(p, "r"); char r[600] = ""; if (f) { size_t n = fread(r, 1, sizeof r - 1, f); r[n] = 0; fclose(f); }
    printf("last-result.txt: %s", r);
    if (!strstr(r, "20260101-000003.json: -1")) fail("last-result.txt");
    printf("ok\n");
    return 0;
}
