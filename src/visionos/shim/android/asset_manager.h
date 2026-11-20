// The Quest's asset calls over the app bundle's files, so the model loaders (src/quest/quest_gun.c, quest_hand.c,
// quest_cabinet.c) and the reload lines build unchanged for Vision Pro. An AAssetManager here is the folder the
// assets are in (vos_assets(): the bundle's resources).
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct AAssetManager AAssetManager;
typedef struct AAsset { FILE *f; unsigned char *buf; long len; } AAsset;
enum { AASSET_MODE_UNKNOWN, AASSET_MODE_RANDOM, AASSET_MODE_STREAMING, AASSET_MODE_BUFFER };

static inline AAsset *AAssetManager_open(AAssetManager *am, const char *name, int mode) {
    (void)mode;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", (const char *)am, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    AAsset *a = (AAsset *)calloc(1, sizeof *a);
    if (!a) { fclose(f); return NULL; }
    a->f = f;
    fseek(f, 0, SEEK_END);
    a->len = ftell(f);
    fseek(f, 0, SEEK_SET);
    return a;
}
static inline int AAsset_read(AAsset *a, void *buf, size_t n) { return (int)fread(buf, 1, n, a->f); }
static inline off_t AAsset_getLength(AAsset *a) { return (off_t)a->len; }
static inline const void *AAsset_getBuffer(AAsset *a) {
    if (!a->buf && a->len > 0 && (a->buf = (unsigned char *)malloc((size_t)a->len))) {
        fseek(a->f, 0, SEEK_SET);
        if (fread(a->buf, 1, (size_t)a->len, a->f) != (size_t)a->len) { free(a->buf); a->buf = NULL; }
    }
    return a->buf;
}
static inline void AAsset_close(AAsset *a) { if (!a) return; fclose(a->f); free(a->buf); free(a); }
