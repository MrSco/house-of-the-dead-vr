#!/bin/sh
# Host test of the Quest client's report collection (src/quest/quest_report.c) on Linux (or WSL), with stubbed Android
# headers; writes test/fixtures/client-report.json, which the relay tests (npm test) then post to a mocked GitHub.
#   sh tools/vr/crash-relay/test/client/run.sh [path to the NDK's jni.h]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
jni=${1:-$(ls "${ANDROID_NDK_HOME:-/nonexistent}"/toolchains/llvm/prebuilt/*/sysroot/usr/include/jni.h 2>/dev/null | head -1)}
[ -f "$jni" ] || jni=$(ls /usr/lib/jvm/*/include/jni.h 2>/dev/null | head -1)
[ -f "$jni" ] || { echo "jni.h not found: pass the NDK's sysroot/usr/include/jni.h"; exit 1; }
mkdir -p "$tmp/inc" "$tmp/files"
cp "$jni" "$tmp/inc/jni.h"
# (Linux's jni.h needs jni_md.h next to it)
[ -f "$(dirname "$jni")/linux/jni_md.h" ] && cp "$(dirname "$jni")/linux/jni_md.h" "$tmp/inc/"
gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I"$here/stub" -I"$tmp/inc" -I"$root/src/quest" -I"$root/src/core" \
    "$here/main.c" "$root/src/quest/quest_report.c" "$root/src/core/quest_options.c" -o "$tmp/t" -lpthread -Wl,--build-id=sha1
"$tmp/t" "$tmp/files" "$here/../fixtures/crash.txt" "$here/../fixtures/client-report.json"
