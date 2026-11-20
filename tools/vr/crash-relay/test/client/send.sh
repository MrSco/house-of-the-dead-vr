#!/bin/sh
# Host test of the Quest client's sending (src/quest/quest_report.c's JNI HttpURLConnection calls) in a desktop JVM on
# Linux (or WSL), against send_server.py.   sh tools/vr/crash-relay/test/client/send.sh /path/to/jdk
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../../.." && pwd)
jdk=${1:-$JAVA_HOME}
[ -f "$jdk/include/jni.h" ] || { echo "pass a JDK (its include/jni.h and lib/server/libjvm.so)"; exit 1; }
tmp=$(mktemp -d)
port=18765
python3 "$here/send_server.py" $port &
srv=$!
trap 'kill $srv; rm -rf "$tmp"' EXIT
mkdir -p "$tmp/files"
gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I"$here/stub" -I"$jdk/include" -I"$jdk/include/linux" \
    -I"$root/src/quest" -I"$root/src/core" "$here/send.c" "$root/src/quest/quest_report.c" "$root/src/core/quest_options.c" -o "$tmp/s" \
    -L"$jdk/lib/server" -ljvm -Wl,-rpath,"$jdk/lib/server" -lpthread
sleep 1
"$tmp/s" "$tmp/files" "http://127.0.0.1:$port/report" "http://127.0.0.1:$((port + 1))/report"
