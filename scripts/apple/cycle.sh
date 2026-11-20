#!/bin/bash
# cycle.sh [-C CHECKOUT] [--force] DEVICE TAG [SECONDS]
#
# One test round on a paired Apple TV: dev-install.sh (build for it, install, launch), wait SECONDS (60), then pull
# the app's port.log to HOTD_TEST_OUT/pulled/TAG.log (HOTD_TEST_OUT: the settings' folder by default).
. "$(dirname "$0")/lib.sh"
hotd_env

opts=() args=()
while [ $# -gt 0 ]; do
    case "$1" in
        -C) opts+=(-C "$2"); shift 2 ;;
        --force) opts+=(--force); shift ;;
        *) args+=("$1"); shift ;;
    esac
done
[ ${#args[@]} -ge 2 ] || die "usage: cycle.sh [-C CHECKOUT] [--force] DEVICE TAG [SECONDS]"
dev="${args[0]}" tag="${args[1]}" secs="${args[2]:-60}"
udid="$(hotd_device "$dev")"
out="${HOTD_TEST_OUT:-$HOTD_TOOLS_DIR}/pulled"
mkdir -p "$out"

bash "$HOTD_APPLE_DIR/dev-install.sh" "${opts[@]}" --launch "$dev" || exit $?
sleep "$secs"
hotd_copy from "$udid" "$HOTD_FILES/port.log" "$out/$tag.log" || die "couldn't pull port.log"
echo "== $out/$tag.log"
