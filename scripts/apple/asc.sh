#!/bin/bash
# asc.sh METHOD PATH [JSON BODY]: the App Store Connect API with the account's key (HOTD_ASC_KEY_DIR, HOTD_ASC_KEY_ID,
# HOTD_ASC_ISSUER from the settings), through the helper HOTD_ASC_SWIFT names (default: hotd-asc.swift beside the
# settings file). Prints the response's JSON; never the key or the token.
#
#   bash scripts/apple/asc.sh GET '/v1/apps/<app id>/builds?limit=5'
. "$(dirname "$0")/lib.sh"
hotd_env
[ $# -ge 2 ] || die "usage: asc.sh METHOD PATH [JSON BODY]"
swift_helper="${HOTD_ASC_SWIFT:-$HOTD_TOOLS_DIR/hotd-asc.swift}"
[ -f "$swift_helper" ] || die "no App Store Connect helper: $swift_helper (HOTD_ASC_SWIFT)"
exec swift "$swift_helper" "$@"
