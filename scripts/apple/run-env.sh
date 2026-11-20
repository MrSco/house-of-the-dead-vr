#!/bin/bash
# run-env.sh [--force] DEVICE ENVFILE TAG SECONDS [SHOT_AT...]
#
# A test run of the app already installed on a paired Apple TV: ENVFILE becomes its env.txt (the test switches,
# docs/platforms.md "Apple TV"), the app starts afresh, a screenshot at each SHOT_AT second, and after SECONDS its
# port.log comes back. Into HOTD_TEST_OUT (the settings' folder by default): shots/TAG_<second>.png, pulled/TAG.log.
# An empty ENVFILE clears the switches (devicectl can't delete the file). DEVICE as dev-install.sh's.
. "$(dirname "$0")/lib.sh"
hotd_env

[ "$1" = --force ] && { export HOTD_FORCE=1; shift; }
[ $# -ge 4 ] || die "usage: run-env.sh [--force] DEVICE ENVFILE TAG SECONDS [SHOT_AT...]"
udid="$(hotd_device "$1")" envf="$2" tag="$3" secs="$4"; shift 4
[ -f "$envf" ] || die "no file $envf"
base="${HOTD_TEST_OUT:-$HOTD_TOOLS_DIR}"
mkdir -p "$base/pulled" "$base/shots"
hotd_device_free "$udid"

for p in $(xcrun devicectl device info processes --device "$udid" 2>/dev/null | grep HouseOfTheDead | awk '{print $1}'); do
    xcrun devicectl device process terminate --device "$udid" --pid "$p" >/dev/null 2>&1
done
hotd_copy to "$udid" "$envf" "$HOTD_FILES/env.txt" || echo "run-env: env.txt copy failed" >&2
xcrun devicectl device process launch --device "$udid" --terminate-existing "$HOTD_APP_ID" 2>&1 | tail -1
t=0
for at in "$@"; do
    sleep $((at - t)); t=$at
    xcrun devicectl device capture screenshot --device "$udid" --destination "$base/shots/${tag}_$at.png" >/dev/null 2>&1 \
        || echo "run-env: no screenshot at ${at}s (asleep?)" >&2
done
[ "$secs" -gt "$t" ] && sleep $((secs - t))
hotd_copy from "$udid" "$HOTD_FILES/port.log" "$base/pulled/$tag.log" || die "couldn't pull port.log"
echo "== $base/pulled/$tag.log"
