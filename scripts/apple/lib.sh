# Shared by scripts/apple/*.sh (sourced, not run): the account's settings, the checkout, the device.
#
# The account (team, keychain, App Store Connect key, profiles, device names) lives outside this repository, in the
# file HOTD_TOOLS_ENV names (default ~/src/hotd-tools/env.sh). scripts/apple/README.md lists what it sets.

HOTD_APPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

die() { echo "$(basename "$0"): $*" >&2; exit 1; }

# The account's settings (HOTD_TEAM, HOTD_ASC_*, HOTD_PROFILE_*, HOTD_DEVICE_*, HOTD_ARCHIVES, ...).
hotd_env() {
    export HOTD_TOOLS_ENV="${HOTD_TOOLS_ENV:-$HOME/src/hotd-tools/env.sh}"
    [ -f "$HOTD_TOOLS_ENV" ] || die "no account settings: $HOTD_TOOLS_ENV (set HOTD_TOOLS_ENV; see scripts/apple/README.md)"
    # shellcheck disable=SC1090
    . "$HOTD_TOOLS_ENV"
    export HOTD_TOOLS_DIR="${HOTD_TOOLS_DIR:-$(cd "$(dirname "$HOTD_TOOLS_ENV")" && pwd)}"
}

# The checkout to build from: -C DIR (hotd_checkout "$dir"), else the current folder when it is one, else the
# checkout these scripts are in. Sets HOTD_CHECKOUT.
hotd_checkout() {
    local d="$1"
    if [ -z "$d" ]; then
        if [ -f "$PWD/scripts/build_tvos.py" ]; then d="$PWD"; else d="$HOTD_APPLE_DIR/../.."; fi
    fi
    [ -f "$d/scripts/build_tvos.py" ] || die "not a House of the Dead VR checkout: $d"
    HOTD_CHECKOUT="$(cd "$d" && pwd)"
    export HOTD_CHECKOUT
}

# A device by UDID, or by a name the settings give (HOTD_DEVICE_<NAME>, e.g. "4k" -> HOTD_DEVICE_4K).
hotd_device() {
    case "$1" in ''|*[!A-Za-z0-9_-]*) echo "$1"; return ;; esac
    local v="HOTD_DEVICE_$(echo "$1" | tr 'a-z-' 'A-Z_')"
    if [ -n "${!v}" ]; then echo "${!v}"; else echo "$1"; fi
}

# The app's files folder on the device (devicectl copy): the app data container.
HOTD_APP_ID="${HOTD_APP_ID:-com.mrsco.hotdvr}"
HOTD_FILES="Library/Caches/HOTDVR"
hotd_copy() {   # hotd_copy to|from DEVICE SOURCE DESTINATION
    xcrun devicectl device copy "$1" --device "$2" --domain-type appDataContainer \
        --domain-identifier "$HOTD_APP_ID" --source "$3" --destination "$4" >/dev/null 2>&1
}

# Refuses a device someone else is using: a process matching HOTD_DEVICE_BUSY (a regex, from the settings) running
# on it. --force (HOTD_FORCE=1) skips the check.
hotd_device_free() {
    [ -n "$HOTD_DEVICE_BUSY" ] && [ -z "$HOTD_FORCE" ] || return 0
    local busy
    busy="$(xcrun devicectl device info processes --device "$1" 2>/dev/null | grep -E "$HOTD_DEVICE_BUSY" | head -3)"
    [ -z "$busy" ] || die "the device is in use (HOTD_DEVICE_BUSY matched; --force to go ahead):
$busy"
}

# In the Mac's desktop session (Terminal), or over ssh?
hotd_over_ssh() { [ -n "$SSH_CONNECTION" ] || [ -n "$SSH_TTY" ]; }
