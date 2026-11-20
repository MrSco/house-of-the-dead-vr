#!/bin/bash
# testflight.sh [-C CHECKOUT] [--here] ios|tvos|visionos BUILD [--upload]
# testflight.sh ios|tvos|visionos next
#
# The App Store build of one platform with that build number: scripts/build_apple.py (iPhone and iPad),
# build_tvos.py or build_visionos.py `device --archive BUILD [--upload]`, signed with the account in the settings
# (HOTD_TOOLS_ENV, see README.md here): HOTD_TEAM, HOTD_KEYCHAIN, HOTD_KEYCHAIN_PASS, HOTD_ASC_KEY_DIR, HOTD_ASC_KEY_ID,
# HOTD_ASC_ISSUER, and the platform's profile, HOTD_PROFILE_IOS, HOTD_PROFILE_TVOS or HOTD_PROFILE_VISIONOS.
#
# The checkout is -C, else the current folder (any copy apple_sync.py made: --dest src/<anything>). Build numbers rise
# per platform: `next` prints the one after the highest App Store Connect has for that platform (HOTD_ASC_APP_ID).
# --upload sends it to TestFlight; then the archive (its dSYMs) moves to HOTD_ARCHIVES/<platform>/<version>-<build>.
# Without --upload it stays in the checkout's build_<platform>/testflight-<version>-<build>.
#
# Over ssh, Apple TV and Vision Pro builds run in the Mac's desktop session (desktop.sh) unless --here.
. "$(dirname "$0")/lib.sh"
hotd_env

dir= here= upload= plat= build=
while [ $# -gt 0 ]; do
    case "$1" in
        -C) dir="$2"; shift 2 ;;
        --here) here=1; shift ;;
        --upload) upload=1; shift ;;
        -*) die "unknown option $1" ;;
        *) if [ -z "$plat" ]; then plat="$1"; elif [ -z "$build" ]; then build="$1"; else die "too many arguments"; fi; shift ;;
    esac
done
case "$plat" in
    ios) script=build_apple.py libs=build_ios asc_platform=IOS profile="$HOTD_PROFILE_IOS" ;;
    tvos) script=build_tvos.py libs=build_tvos asc_platform=TV_OS profile="$HOTD_PROFILE_TVOS" ;;
    visionos) script=build_visionos.py libs=build_visionos asc_platform=VISION_OS profile="$HOTD_PROFILE_VISIONOS" ;;
    *) die "usage: testflight.sh [-C CHECKOUT] [--here] ios|tvos|visionos BUILD|next [--upload]" ;;
esac
[ -n "$build" ] || die "which build number? (testflight.sh $plat next prints the next one)"

if [ "$build" = next ]; then
    [ -n "$HOTD_ASC_APP_ID" ] || die "HOTD_ASC_APP_ID isn't set (the app's App Store Connect id)"
    q="/v1/builds?filter%5Bapp%5D=$HOTD_ASC_APP_ID&filter%5BpreReleaseVersion.platform%5D=$asc_platform&fields%5Bbuilds%5D=version&limit=200"
    bash "$HOTD_APPLE_DIR/asc.sh" GET "$q" | python3 -c '
import json, sys
d = json.load(sys.stdin)
if "data" not in d: sys.exit("testflight.sh: App Store Connect said: %s" % json.dumps(d.get("errors", d))[:400])
print(max([int(b["attributes"]["version"]) for b in d["data"] if b["attributes"]["version"].isdigit()] or [0]) + 1)'
    exit
fi
case "$build" in *[!0-9]*) die "the build number must be a number: $build" ;; esac
[ -n "$profile" ] || die "no profile for $plat in the settings (HOTD_PROFILE_$(echo "$plat" | tr a-z A-Z))"
hotd_checkout "$dir"

if [ "$plat" != ios ] && [ -z "$here" ] && hotd_over_ssh; then
    cd "$HOTD_CHECKOUT" || exit 1
    exec bash "$HOTD_APPLE_DIR/desktop.sh" bash "$HOTD_APPLE_DIR/testflight.sh" --here "$plat" "$build" ${upload:+--upload}
fi

cd "$HOTD_CHECKOUT" || exit 1
echo "[testflight] $plat build $build from $HOTD_CHECKOUT${upload:+, to TestFlight}"
HOTD_PROFILE="$profile" python3 "scripts/$script" device --archive "$build" ${upload:+--upload}
rc=$?
out="$(ls -td "$libs"/testflight-*-"$build" 2>/dev/null | head -1)"
if [ $rc -eq 0 ] && [ -n "$upload" ] && [ -n "$out" ] && [ -n "$HOTD_ARCHIVES" ]; then
    dest="$HOTD_ARCHIVES/$plat/${out##*/testflight-}"
    [ -e "$dest" ] && dest="$dest.$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$HOTD_ARCHIVES/$plat" && mv "$out" "$dest" && echo "[testflight] the archive (dSYMs): $dest"
elif [ -n "$out" ]; then
    echo "[testflight] the archive: $HOTD_CHECKOUT/$out"
fi
exit $rc
