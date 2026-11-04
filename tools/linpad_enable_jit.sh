#!/bin/sh
# Turn on LinPad fast mode (JIT) over USB without typing LLDB commands. Drives
# the procedure in release/USB-JIT.md: attach to the running LinPad, then
# `linpad-jit --retry`, which triggers the app's own Retry, prepares the RX
# region and detaches when LinPad asks. Needed after every LinPad launch,
# including the first one after re-signing.
#
#   tools/linpad_enable_jit.sh
#
# On any failure LLDB is left attached and stopped, as USB-JIT.md requires:
# detaching on an unserviced JIT trap would crash LinPad. The script prints
# how to continue that session.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
attach_limit=${LINPAD_JIT_ATTACH_SECONDS:-600}
protocol_limit=${LINPAD_JIT_PROTOCOL_SECONDS:-330}

fail() { printf '%s\n' "linpad-jit: $*" >&2; exit 1; }

udid=${LINPAD_DEVICE_UDID:-}
if [ -z "$udid" ] && [ -f "$HOME/.config/linpad/ssh-device-udid" ]; then
    IFS= read -r udid < "$HOME/.config/linpad/ssh-device-udid" || :
fi
[ -n "$udid" ] || fail 'set LINPAD_DEVICE_UDID or ~/.config/linpad/ssh-device-udid'

# Match the executable, not the bundle folder: iloader keeps "iSH ARM64.app",
# SideStore installs it as "App.app".
pid=$(xcrun devicectl device info processes --device "$udid" 2>/dev/null |
    awk '/\.app\/iSH ARM64( |$)/ { print $1; exit }')
[ -n "$pid" ] || fail 'LinPad is not running; open it on the iPad first'
printf 'LinPad PID %s; attaching (LinPad freezes until this finishes).\n' "$pid"

work=$(mktemp -d "${TMPDIR:-/tmp}/linpad-jit.XXXXXX")
fifo=$work/in log=$work/lldb.log
mkfifo "$fifo"
# Hold the FIFO open so LLDB keeps reading between commands and survives
# this script if it has to be left attached.
sleep 86400 > "$fifo" &
holder=$!
xcrun lldb --no-lldbinit < "$fifo" > "$log" 2>&1 &
lldb_pid=$!

send() { printf '%s\n' "$@" > "$fifo"; }
# LLDB reports these without an "error:" prefix.
lldb_errors='^error:|Traceback|needs to be unlocked|no device selected|unable to attach|attach failed'
# wait_for SECONDS SUCCESS_REGEX: 0 on success, 1 on an LLDB error, 2 on timeout.
wait_for() {
    waited=0
    while [ "$waited" -lt "$1" ]; do
        if grep -qE "$2" "$log"; then return 0; fi
        if grep -qiE "$lldb_errors" "$log"; then return 1; fi
        kill -0 "$lldb_pid" 2>/dev/null || return 1
        sleep 2
        waited=$((waited + 2))
    done
    return 2
}
# Nothing is attached yet, so LLDB can simply be closed.
end_session() {
    send quit 2>/dev/null || :
    sleep 2
    kill "$lldb_pid" "$holder" 2>/dev/null || :
    wait "$lldb_pid" "$holder" 2>/dev/null || :
    rm -rf "$work"
}
not_attached() {
    grep -iE "$lldb_errors" "$log" | sed 's/^/  /' >&2 || :
    locked=0
    grep -qi 'needs to be unlocked' "$log" && locked=1
    # Keep the LLDB transcript for diagnosis; only the FIFO and holder go.
    mkdir -p "$HOME/Library/Logs"
    kept=$HOME/Library/Logs/linpad-jit-$(date +%Y%m%d-%H%M%S).log
    cp "$log" "$kept" 2>/dev/null && printf '%s\n' "LLDB log kept at $kept" >&2
    tail -n 8 "$log" | sed 's/^/  | /' >&2
    end_session
    [ "$locked" = 0 ] || fail 'unlock the iPad, keep LinPad in front and run this again (nothing was attached)'
    fail "$1 (nothing was attached)"
}
keep_session() {
    tail -n 20 "$log" >&2
    printf '%s\n' "linpad-jit: $1" \
        "LLDB is still attached (PID $lldb_pid). Inspect with: tail -f '$log'" \
        "Send commands with: printf '%s\\n' 'thread backtrace all' > '$fifo'" \
        "When safe, end it with: printf '%s\\n' quit > '$fifo'; kill $holder" >&2
    exit 1
}

send 'script lldb.debugger.SetAsync(False)' "device select $udid" "device process attach -p $pid"
if ! wait_for "$attach_limit" "Process $pid stopped"; then
    # Only a stopped, attached target must be preserved for inspection.
    grep -q "Process $pid" "$log" && keep_session 'attach did not reach a stopped target'
    not_attached 'attach failed'
fi

send "command script import $here/enable_device_jit.py" 'linpad-jit --retry --timeout 300'
if ! wait_for "$protocol_limit" 'LinPad protocol completed and LLDB detached|already On; LLDB detached'; then
    keep_session 'linpad-jit did not complete'
fi
grep -E 'Prepared 16384|already On|protocol completed' "$log" | sed 's/^/  /'

send quit
waited=0
while kill -0 "$lldb_pid" 2>/dev/null && [ "$waited" -lt 30 ]; do sleep 1; waited=$((waited + 1)); done
kill "$holder" 2>/dev/null || :
wait "$holder" 2>/dev/null || :
rm -rf "$work"
printf '%s\n' 'Fast mode is on for this LinPad process. Linux programs started from now on use the JIT.'
