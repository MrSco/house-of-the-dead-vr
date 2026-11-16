// Just enough of the Android NDK for quest_report.c to run on a Linux host (test/client/run.sh).
#pragma once
#include <stdio.h>
#include <string.h>
#include <jni.h>
typedef struct AAssetManager AAssetManager;
typedef struct AAsset { const char *data; int pos; } AAsset;
enum { AASSET_MODE_BUFFER = 3 };
AAsset *AAssetManager_open(AAssetManager *m, const char *name, int mode);
int AAsset_read(AAsset *a, void *buf, size_t n);
void AAsset_close(AAsset *a);
typedef struct ANativeActivity { JavaVM *vm; AAssetManager *assetManager; } ANativeActivity;
enum { ANDROID_LOG_INFO = 4 };
#define __android_log_print(prio, tag, ...) (fprintf(stderr, "[%s] ", tag), fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n"))
#define PROP_VALUE_MAX 92
int __system_property_get(const char *name, char *value);
