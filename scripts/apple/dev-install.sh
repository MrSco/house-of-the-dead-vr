#!/bin/bash
# dev-install.sh [-C CHECKOUT] [--build-only] [--launch] [--force] DEVICE
#
# A development build of the Apple TV app for one paired Apple TV, installed on it: `scripts/build_tvos.py device
# --app --dest UDID` (automatic signing through the account's App Store Connect key: HOTD_TEAM, HOTD_ASC_KEY_DIR,
# HOTD_ASC_KEY_ID, HOTD_ASC_ISSUER from the settings; the Apple TV is registered with the team if it isn't yet), then
# `devicectl device install app`. DEVICE is a UDID or a name from the settings (HOTD_DEVICE_<NAME>: "4k" for
# HOTD_DEVICE_4K). The build needs the desktop session's keychain: over ssh it runs there (desktop.sh).
#
# --build-only: build, don't install. --launch: start the app after installing. A device running something that
# matches HOTD_DEVICE_BUSY (someone else's session) is refused unless --force.
. "$(dirname "$0")/lib.sh"
hotd_env

dir= only= launch= dev= here=
while [ $# -gt 0 ]; do
    case "$1" in
        -C) dir="$2"; shift 2 ;;
        --build-only) only=1; shift ;;
        --launch) launch=1; shift ;;
        --force) export HOTD_FORCE=1; shift ;;
        --build-here) here=1; shift ;;   # (desktop.sh's own call: the build only, here)
        -*) die "unknown option $1" ;;
        *) dev="$1"; shift ;;
    esac
done
[ -n "$dev" ] || die "usage: dev-install.sh [-C CHECKOUT] [--build-only] [--launch] [--force] DEVICE"
udid="$(hotd_device "$dev")"
hotd_checkout "$dir"
cd "$HOTD_CHECKOUT" || exit 1
for v in HOTD_TEAM HOTD_ASC_KEY_DIR HOTD_ASC_KEY_ID HOTD_ASC_ISSUER; do [ -n "${!v}" ] || die "$v isn't set (the settings)"; done

[ -n "$only$here" ] || hotd_device_free "$udid"   # (before a long build, and again before installing)

if [ -n "$here" ] || ! hotd_over_ssh; then
    echo "[dev-install] building for $udid from $HOTD_CHECKOUT"
    DEVELOPMENT_TEAM="$HOTD_TEAM" python3 scripts/build_tvos.py device --app --dest "$udid" || exit $?
    [ -n "$here" ] && exit 0
else
    bash "$HOTD_APPLE_DIR/desktop.sh" bash "$HOTD_APPLE_DIR/dev-install.sh" --build-here "$dev" || exit $?
fi
app="$HOTD_CHECKOUT/build_tvos/dd/Build/Products/Release-appletvos/HouseOfTheDeadVR.app"
[ -d "$app" ] || die "no app at $app"
echo "[dev-install] app: $app"
[ -n "$only" ] && exit 0
hotd_device_free "$udid"
xcrun devicectl device install app --device "$udid" "$app" >/dev/null || die "install failed (asleep? devicectl device reboot wakes it)"
echo "[dev-install] installed on $udid"
if [ -n "$launch" ]; then
    xcrun devicectl device process launch --device "$udid" --terminate-existing "$HOTD_APP_ID" 2>&1 | tail -1
fi
