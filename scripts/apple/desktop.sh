#!/bin/bash
# desktop.sh [--timeout SECONDS] [--keep] COMMAND [ARGS...]
#
# Runs a command in the Mac's desktop session (a Terminal window), from ssh, and waits for it: what needs the logged-in
# user's keychain (codesign with the development identity, Xcode's automatic signing: "No Accounts" over ssh) runs
# there. Writes a temporary .command (the current folder, the HOTD_* / ANGLE_DIR / DEVELOPMENT_TEAM variables, the
# command), opens it, shows its output as it comes, and exits with the command's exit code. The Mac's user must be
# logged in at the desktop. --timeout gives up after that many seconds (the command keeps running); --keep leaves the
# temporary folder (the .command and its log) and prints where.
#
#   ssh mac 'cd ~/src/house-of-the-dead-vr && bash scripts/apple/desktop.sh bash scripts/apple/testflight.sh tvos 12'
. "$(dirname "$0")/lib.sh"

timeout=0 keep=
while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) timeout="$2"; shift 2 ;;
        --keep) keep=1; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done
[ $# -gt 0 ] || die "usage: desktop.sh [--timeout SECONDS] [--keep] COMMAND [ARGS...]"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/hotd-desktop.XXXXXX")" || die "no temporary folder"
cmdf="$tmp/run.command" log="$tmp/log" st="$tmp/status"
{
    echo '#!/bin/bash'
    printf 'cd %q || exit 1\n' "$PWD"
    # (the desktop session's environment is its own: pass this one's settings along)
    for v in $(compgen -e | grep -E '^(HOTD_|ANGLE_DIR$|DEVELOPMENT_TEAM$)'); do printf 'export %s=%q\n' "$v" "${!v}"; done
    printf '{ '; printf '%q ' "$@"; printf '; } > %q 2>&1 < /dev/null\n' "$log"
    printf 'echo $? > %q.tmp && mv %q.tmp %q\n' "$st" "$st" "$st"
} > "$cmdf"
chmod +x "$cmdf"
: > "$log"
open "$cmdf" || die "open failed (is the Mac's user logged in at the desktop?)"

off=0 waited=0
flush() {
    local sz; sz=$(wc -c < "$log" | tr -d ' ')
    if [ "$sz" -gt "$off" ]; then tail -c +$((off + 1)) "$log" | head -c $((sz - off)); off=$sz; fi
}
while [ ! -f "$st" ]; do
    sleep 2; waited=$((waited + 2)); flush
    if [ "$timeout" -gt 0 ] && [ "$waited" -ge "$timeout" ]; then
        echo "desktop.sh: gave up after ${timeout}s; still running, log: $log" >&2; exit 124
    fi
done
flush
code="$(cat "$st")"
if [ -n "$keep" ]; then echo "desktop.sh: kept $tmp" >&2; else rm -rf "$tmp"; fi
exit "${code:-1}"
