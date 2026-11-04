#!/bin/sh
# Configure key-only SSH inside the LinPad Linux guest. Read one public key on stdin.
set -eu
umask 077

fail() {
    printf '%s\n' "linpad-ssh: $*" >&2
    exit 1
}

# BusyBox init (guest PID 1) reads /etc/inittab at every LinPad launch, with or
# without the desktop. It runs a "once" entry without waiting; the command has
# shell redirections, so init passes it to /bin/sh. The helper itself checks
# /etc/linpad-ssh/autostart and bounds its own runtime.
boot_entry='::once:/usr/local/bin/linpad-ssh-start --boot </dev/null >/dev/null 2>&1'
boot_entry_comment='# LinPad SSH: start at every launch (tools/setup_guest_ssh.sh)'

boot_entry_present() {
    [ -f "$1" ] && grep -Fqx "$boot_entry" "$1"
}

# Append the entry once, preserving every existing line, mode and ownership.
install_boot_entry() {
    if [ ! -f "$1" ] || [ -L "$1" ] || [ "$(stat -c %u "$1")" != 0 ]; then
        printf '%s\n' "linpad-ssh: $1 is not a root-owned regular file; SSH will not start at launch" >&2
        return 0
    fi
    boot_entry_present "$1" && return 0
    inittab_mode=$(stat -c %a "$1")
    inittab_tmp=$(mktemp "$1.linpad-ssh.XXXXXX")
    cat "$1" > "$inittab_tmp"
    # Keep a final line without a newline separate from the new entry.
    [ ! -s "$inittab_tmp" ] || [ -z "$(tail -c 1 "$inittab_tmp")" ] || printf '\n' >> "$inittab_tmp"
    printf '%s\n%s\n' "$boot_entry_comment" "$boot_entry" >> "$inittab_tmp"
    chmod "$inittab_mode" "$inittab_tmp"
    mv "$inittab_tmp" "$1"
    inittab_tmp=''
}

main() {
    [ "$(id -u)" = 0 ] || fail 'run this script as guest root'
    public_key=''
    IFS= read -r public_key || [ -n "$public_key" ] || fail 'missing public key on stdin'
    printf '%s\n' "$public_key" | awk '
        NF >= 2 && $1 == "ssh-ed25519" && $2 ~ /^[A-Za-z0-9+\/=]+$/ { valid = 1 }
        END { exit !valid }
    ' || fail 'expected one ssh-ed25519 public key, never a private key'

    helper=/usr/local/bin/linpad-ssh-start
    if [ -e "$helper" ] || [ -L "$helper" ]; then
        [ -f "$helper" ] && [ ! -L "$helper" ] &&
            [ "$(sed -n '1p' "$helper")" = '#!/bin/sh' ] &&
            [ "$(sed -n '2p' "$helper")" = '# LinPad SSH helper' ] ||
            fail "refusing to overwrite an unrecognized $helper"
    fi
    switch=/usr/local/bin/linpad-ssh-autostart
    if [ -e "$switch" ] || [ -L "$switch" ]; then
        [ -f "$switch" ] && [ ! -L "$switch" ] &&
            [ "$(sed -n '1p' "$switch")" = '#!/bin/sh' ] &&
            [ "$(sed -n '2p' "$switch")" = '# LinPad SSH autostart switch' ] ||
            fail "refusing to overwrite an unrecognized $switch"
    fi

    resume_dir=/etc/linpad/lifecycle.d
    resume_hook=$resume_dir/50-linpad-ssh
    check_resume_destination() {
        # Check before changing authentication and again immediately before
        # installation. Preserve all other existing lifecycle hooks.
        for resume_parent in /etc /etc/linpad "$resume_dir"; do
            [ ! -L "$resume_parent" ] || fail "refusing symlinked $resume_parent"
            if [ -e "$resume_parent" ]; then
                [ -d "$resume_parent" ] && [ "$(stat -c %u "$resume_parent")" = 0 ] ||
                    fail "resume directory must be root-owned: $resume_parent"
            fi
        done
        if [ -e "$resume_hook" ] || [ -L "$resume_hook" ]; then
            [ -f "$resume_hook" ] && [ ! -L "$resume_hook" ] &&
                [ "$(stat -c %u "$resume_hook")" = 0 ] &&
                [ "$(sed -n '1p' "$resume_hook")" = '#!/bin/sh' ] &&
                [ "$(sed -n '2p' "$resume_hook")" = '# LinPad SSH resume hook' ] ||
                fail "refusing to overwrite an unrecognized $resume_hook"
        fi
    }
    check_resume_destination

    # Earlier installs started SSH from the desktop session only. BusyBox init
    # now starts it at every LinPad launch; remove only that recognized hook.
    legacy_session_hook=/etc/ishwl/session.d/60-linpad-ssh.sh
    if [ -e "$legacy_session_hook" ] || [ -L "$legacy_session_hook" ]; then
        [ -f "$legacy_session_hook" ] && [ ! -L "$legacy_session_hook" ] &&
            [ "$(stat -c %u "$legacy_session_hook")" = 0 ] &&
            [ "$(sed -n '2p' "$legacy_session_hook")" = '# LinPad SSH boot hook' ] ||
            fail "refusing to remove an unrecognized $legacy_session_hook"
    fi
    inittab=/etc/inittab
    autostart_flag=/etc/linpad-ssh/autostart
    # First installation enables autostart. Reruns keep the user's choice.
    enable_autostart=0
    if [ ! -e "$legacy_session_hook" ] && ! boot_entry_present "$inittab"; then
        enable_autostart=1
    fi

    if ! command -v dropbear >/dev/null 2>&1 || ! command -v dropbearkey >/dev/null 2>&1; then
        apk add --no-cache dropbear
    fi

    [ ! -L /root/.ssh ] && [ ! -L /etc/linpad-ssh ] ||
        fail 'refusing to configure SSH through a symlinked directory'
    mkdir -p /root/.ssh /etc/linpad-ssh /usr/local/bin
    chmod 700 /root/.ssh /etc/linpad-ssh
    authorized_keys=/root/.ssh/authorized_keys
    [ ! -L "$authorized_keys" ] || fail 'authorized_keys must not be a symlink'
    [ -e "$authorized_keys" ] || : > "$authorized_keys"
    [ -f "$authorized_keys" ] || fail 'authorized_keys is not a regular file'
    chmod 600 "$authorized_keys"
    if ! grep -Fqx "$public_key" "$authorized_keys"; then
        # A leading newline also preserves a pre-existing final line without a newline.
        printf '\n%s\n' "$public_key" >> "$authorized_keys"
    fi

    host_key=/etc/linpad-ssh/dropbear_ed25519_host_key
    [ ! -L "$host_key" ] || fail 'host key must not be a symlink'
    if [ ! -e "$host_key" ]; then
        dropbearkey -t ed25519 -f "$host_key" >/dev/null
    fi
    [ -f "$host_key" ] || fail 'host key is not a regular file'
    chmod 600 "$host_key"

    helper_tmp=$(mktemp /usr/local/bin/.linpad-ssh-start.XXXXXX)
    public_tmp=''
    resume_tmp=''
    inittab_tmp=''
    switch_tmp=''
    trap 'rm -f "$helper_tmp"; [ -z "$public_tmp" ] || rm -f "$public_tmp"; [ -z "$resume_tmp" ] || rm -f "$resume_tmp"; [ -z "$inittab_tmp" ] || rm -f "$inittab_tmp"; [ -z "$switch_tmp" ] || rm -f "$switch_tmp"' 0
    cat > "$helper_tmp" <<'HELPER'
#!/bin/sh
# LinPad SSH helper
# Key-only IPv4 SSH for USB forwarding and Wi-Fi. No shell/profile autostart;
# BusyBox init may run --boot once per LinPad launch (/etc/inittab).
set -eu
set -f
umask 077
state_dir=/run/linpad-ssh
pid_file=$state_dir/server.pid
log_file=$state_dir/server.log
host_key=/etc/linpad-ssh/dropbear_ed25519_host_key
proc_root=/proc
timeout=/usr/bin/timeout
nc=/usr/bin/nc

fail() { printf '%s\n' "linpad-ssh: $*" >&2; exit 1; }
# Bound the complete invocation as well as individual network/daemon calls.
# --bounded-run is an internal re-exec marker, not a service or retry loop.
[ -x "$timeout" ] && [ -x "$nc" ] || fail 'BusyBox timeout/nc required'
if [ "${1-}" != --bounded-run ]; then
    exec "$timeout" 35 /bin/sh "$0" --bounded-run "$@"
fi
shift
recover_only=0
boot=0
case "$#:${1-}" in
    0:) ;;
    1:--recover-existing) recover_only=1 ;;
    1:--boot) boot=1 ;;
    *) fail 'usage: linpad-ssh-start [--recover-existing | --boot]' ;;
esac
[ "$(id -u)" = 0 ] || fail 'run as guest root'
# --boot follows LinPad's preference (iPad Settings > LinPad > Start SSH at
# Launch), which Linux sees as /proc/ish/defaults/ssh_autostart. Apps without
# that preference fall back to this root-owned marker.
autostart_flag=/etc/linpad-ssh/autostart
autostart_pref=$proc_root/ish/defaults/ssh_autostart
if [ "$boot" = 1 ]; then
    autostart=''
    if [ -r "$autostart_pref" ]; then
        value=$(tr -d ' \t\r\n' < "$autostart_pref" 2>/dev/null) || value=''
        case "$value" in true|1) autostart=on ;; false|0) autostart=off ;; esac
    fi
    if [ -z "$autostart" ]; then
        if [ -f "$autostart_flag" ]; then autostart=on; else autostart=off; fi
    fi
    if [ "$autostart" = off ]; then
        printf '%s\n' 'LinPad SSH autostart is off; left unchanged'
        exit 0
    fi
fi

valid_pid() {
    case "$1" in ''|*[!0-9]*|0[0-9]*) return 1 ;; esac
    [ "${#1}" -le 10 ] && [ "$1" -gt 1 ] && [ "$1" -le 2147483647 ]
}
valid_tick() {
    # <=15 decimal digits keeps +/-1 subtraction within signed 64-bit ash.
    # Reject leading zeroes so arithmetic cannot interpret them as octal.
    case "$1" in ''|*[!0-9]*|0[0-9]*) return 1 ;; esac
    [ "${#1}" -le 15 ]
}
read_start() {
    proc_stat=''
    IFS= read -r proc_stat < "$proc_root/$1/stat" || return 1
    proc_tail=${proc_stat##*) }
    [ "$proc_tail" != "$proc_stat" ] || return 1
    set -- $proc_tail
    [ "$#" -ge 20 ] || return 1
    shift 19
    valid_tick "$1" || return 1
    proc_start=$1
}
start_matches() {
    valid_tick "$1" && valid_tick "$2" || return 1
    # LinPad computes boot ticks using separate realtime/uptime samples:
    # the same process may fluctuate by one 100-Hz tick (10 milliseconds).
    # Larger changes fail closed. Other identity markers are also required.
    tick_delta=$(($1 - $2))
    [ "$tick_delta" -ge -1 ] && [ "$tick_delta" -le 1 ]
}
read_pid() {
    [ -f "$pid_file" ] && [ ! -L "$pid_file" ] || return 1
    pid=''
    IFS= read -r pid < "$pid_file" || :
    valid_pid "$pid"
}
pid_matches() {
    read_pid && [ "$pid" = "$expected_pid" ]
}
master_matches() {
    pid_matches || return 1
    kill -0 "$expected_pid" 2>/dev/null || return 1
    [ "$("$timeout" 2 readlink "$proc_root/$expected_pid/exe" 2>/dev/null)" = /usr/sbin/dropbear ] || return 1
    read_start "$expected_pid" && start_matches "$proc_start" "$expected_start" || return 1
    seen_name=0 seen_pid=0 seen_tgid=0 seen_ppid=0 seen_uid=0
    while IFS=' 	' read -r key a b c d _extra; do
        case "$key" in
            Name:) [ "$a" = dropbear ] || return 1; seen_name=1 ;;
            Pid:) [ "$a" = "$expected_pid" ] || return 1; seen_pid=1 ;;
            Tgid:) [ "$a" = "$expected_pid" ] || return 1; seen_tgid=1 ;;
            PPid:) [ "$a" = 1 ] || return 1; seen_ppid=1 ;;
            Uid:) [ "$a:$b:$c:$d" = 0:0:0:0 ] || return 1; seen_uid=1 ;;
        esac
    done < "$proc_root/$expected_pid/status"
    [ "$seen_name:$seen_pid:$seen_tgid:$seen_ppid:$seen_uid" = 1:1:1:1:1 ]
}
# /run is not cleared when iPadOS ends LinPad. The current launch began when
# guest PID 1 started; boot ticks share /proc/stat's btime base. A runtime file
# modified before then belongs to a previous launch whose processes are gone.
session_start=''
read_session_start() {
    boot_time=''
    [ -r "$proc_root/stat" ] || return 1
    while read -r key value _extra; do
        [ "$key" = btime ] && { boot_time=$value; break; }
    done < "$proc_root/stat"
    valid_tick "$boot_time" && read_start 1 || return 1
    session_start=$((boot_time + proc_start / 100))
}
predates_session() {
    [ -n "$session_start" ] || return 1
    mtime=$("$timeout" 2 stat -c %Y "$1" 2>/dev/null) || return 1
    valid_tick "$mtime" || return 1
    # Two seconds absorb btime/tick rounding; closer files are kept as current.
    [ "$mtime" -lt $((session_start - 2)) ]
}
has_banner() {
    # /dev/null stdin makes BusyBox nc exit before reading a healthy banner.
    "$timeout" 4 /bin/sh -c 'sleep 3 | "$1" -w 2 127.0.0.1 2222' sh "$nc" 2>/dev/null |
        { banner=''; IFS= read -r banner || :; case "$banner" in SSH-2.0-*) exit 0 ;; *) exit 1 ;; esac; }
}

# Resume never enables a service that was off, including an absent state dir.
[ ! -L "$state_dir" ] || fail 'state directory must not be a symlink'
if [ "$recover_only" = 1 ] && [ ! -d "$state_dir" ]; then
    printf '%s\n' 'LinPad SSH is off; left unchanged'
    exit 0
fi
mkdir -p "$state_dir"
[ "$("$timeout" 2 stat -c %u "$state_dir")" = 0 ] || fail 'state directory must belong to root'
chmod 700 "$state_dir"
[ ! -L "$pid_file" ] && [ ! -L "$log_file" ] || fail 'refusing symlinked runtime files'

# A lock is never stolen from the current launch. Unknown ownership fails
# closed; only a lock proven to predate this launch is discarded.
lock_dir=$state_dir/start.lock
if read_session_start; then
    if [ -d "$lock_dir" ] && [ ! -L "$lock_dir" ] && [ ! -L "$lock_dir/owner" ] &&
        [ "$("$timeout" 2 stat -c %u "$lock_dir" 2>/dev/null)" = 0 ] &&
        predates_session "$lock_dir"; then
        rm -f "$lock_dir/owner"
        rmdir "$lock_dir" 2>/dev/null || :
        printf '%s\n' 'Removed start.lock left by a previous LinPad launch'
    fi
    # Its PID may now name an unrelated process; never signal it, just forget it.
    if [ -f "$pid_file" ] && predates_session "$pid_file"; then
        rm -f "$pid_file"
        printf '%s\n' 'Removed PID file left by a previous LinPad launch'
    fi
fi
mkdir "$lock_dir" 2>/dev/null || fail 'another SSH operation or an unknown start.lock exists; left unchanged'
self_pid=$$
if ! read_start "$self_pid"; then
    rmdir "$lock_dir" 2>/dev/null || :
    fail 'cannot record this lock owner starttime'
fi
self_start=$proc_start
lock_owner="$self_pid:$self_start"
cleanup_lock() {
    [ -d "$lock_dir" ] && [ ! -L "$lock_dir" ] && [ ! -L "$lock_dir/owner" ] || return 0
    [ "$("$timeout" 2 stat -c %u "$lock_dir" 2>/dev/null)" = 0 ] || return 0
    owner=''
    IFS= read -r owner < "$lock_dir/owner" || return 0
    [ "$owner" = "$lock_owner" ] || return 0
    read_start "$self_pid" && start_matches "$proc_start" "$self_start" || return 0
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || :
}
trap cleanup_lock 0
trap 'exit 124' HUP INT TERM
(set -C; printf '%s\n' "$lock_owner" > "$lock_dir/owner") || fail 'cannot create lock owner marker'

existing=0
if [ -e "$pid_file" ]; then
    [ -f "$pid_file" ] || fail 'PID file is not a regular file; left unchanged'
    if [ -s "$pid_file" ]; then
        read_pid || fail 'invalid PID file; left unchanged'
        if kill -0 "$pid" 2>/dev/null; then
            expected_pid=$pid
            read_start "$pid" || fail 'cannot read server starttime; left unchanged'
            expected_start=$proc_start
            master_matches || fail 'PID is not the expected root Dropbear master; left unchanged'
            existing=1
        fi
    fi
fi
if [ "$existing" = 0 ] && [ "$recover_only" = 1 ]; then
    printf '%s\n' 'LinPad SSH is off; left unchanged'
    exit 0
fi
if [ "$existing" = 1 ]; then
    if has_banner; then
        master_matches || fail 'server identity changed during health check; left unchanged'
        printf 'LinPad SSH already running (PID %s, port 2222; banner verified)\n' "$expected_pid"
        exit 0
    fi
    master_matches || fail 'server identity changed after first missing banner; left unchanged'
    sleep 1
    if has_banner; then
        master_matches || fail 'server identity changed during health check; left unchanged'
        printf 'LinPad SSH already running (PID %s, port 2222; banner verified)\n' "$expected_pid"
        exit 0
    fi
    master_matches || fail 'server identity changed after second missing banner; left unchanged'
    printf 'LinPad SSH master PID %s has no banner; recovering once\n' "$expected_pid"
    # One TERM of this exact master only. Never signal a process group or child.
    kill -TERM "$expected_pid" || fail 'server TERM failed; no restart'
    attempt=0
    while [ -d "$proc_root/$expected_pid" ] && [ "$attempt" -lt 5 ]; do
        read_start "$expected_pid" && start_matches "$proc_start" "$expected_start" || fail 'PID changed after TERM; no restart'
        sleep 1
        attempt=$((attempt + 1))
    done
    [ ! -d "$proc_root/$expected_pid" ] || fail 'old master still present; no restart'
    if [ -e "$pid_file" ] || [ -L "$pid_file" ]; then
        pid_matches || fail 'PID file changed during recovery; no restart'
    fi
fi

[ -f "$host_key" ] && [ ! -L "$host_key" ] || fail 'host key missing or symlinked; rerun setup'
[ ! -e "$log_file" ] || [ -f "$log_file" ] || fail 'server log is not a regular file'
[ ! -L "$pid_file" ] && [ ! -L "$log_file" ] || fail 'runtime files changed; no startup'
touch "$log_file"
chmod 600 "$log_file"
# Reuse the existing key and options. -s disables password authentication;
# forwarding remains available for OAuth. This never reads key contents.
"$timeout" 5 /usr/sbin/dropbear -E -s -r "$host_key" -p 0.0.0.0:2222 -P "$pid_file" \
    < /dev/null >> "$log_file" 2>&1 || fail 'Dropbear startup failed'
# The daemon child writes its PID asynchronously after the parent returns.
attempt=0
started=0
while [ "$attempt" -le 5 ]; do
    if read_pid && kill -0 "$pid" 2>/dev/null; then
        expected_pid=$pid
        if read_start "$pid"; then
            expected_start=$proc_start
            if master_matches; then started=1; break; fi
        fi
    fi
    [ "$attempt" -lt 5 ] || break
    sleep 1
    attempt=$((attempt + 1))
done
[ "$started" = 1 ] || fail 'started master identity could not be verified'
has_banner || fail 'started master has no SSH banner; no further recovery attempted'
master_matches || fail 'started master identity changed during health check'
printf 'LinPad SSH started (PID %s, port 2222; banner verified)\n' "$expected_pid"
exit 0
HELPER
    chmod 755 "$helper_tmp"
    mv "$helper_tmp" "$helper"

    check_resume_destination
    mkdir -p "$resume_dir"
    check_resume_destination
    resume_tmp=$(mktemp "$resume_dir/.linpad-ssh-resume.XXXXXX")
    cat > "$resume_tmp" <<'RESUME_HOOK'
#!/bin/sh
# LinPad SSH resume hook
# Return within the lifecycle runner's five-second hook limit. This launches
# one transient health check, never a resident monitor or an off-to-on start.
case "${1-}" in resume) ;; *) exit 0 ;; esac
[ -x /usr/bin/nohup ] && [ -x /usr/bin/timeout ] &&
    [ -x /usr/local/bin/linpad-ssh-start ] || exit 1
# The helper owns the atomic operation lock and its own 35-second deadline.
/usr/bin/nohup /usr/bin/timeout 40 /usr/local/bin/linpad-ssh-start --recover-existing \
    < /dev/null > /dev/null 2>&1 &
exit 0
RESUME_HOOK
    chmod 755 "$resume_tmp"
    mv "$resume_tmp" "$resume_hook"

    install_boot_entry "$inittab"
    [ ! -e "$legacy_session_hook" ] || rm -f "$legacy_session_hook"
    if [ "$enable_autostart" = 1 ]; then
        [ ! -L "$autostart_flag" ] || fail 'autostart marker must not be a symlink'
        : > "$autostart_flag"
        chmod 600 "$autostart_flag"
    fi

    switch_tmp=$(mktemp /usr/local/bin/.linpad-ssh-autostart.XXXXXX)
    cat > "$switch_tmp" <<'AUTOSTART_SWITCH'
#!/bin/sh
# LinPad SSH autostart switch
# Whether SSH starts at every LinPad launch. BusyBox init runs
# "linpad-ssh-start --boot" from /etc/inittab, which follows LinPad's
# preference (iPad Settings > LinPad > Start SSH at Launch, also in the desktop
# Settings), seen here as /proc/ish/defaults/ssh_autostart. This command changes
# that preference and mirrors it in a marker file for apps without it.
# Output is one word, "on" or "off", for scripts.
set -eu
umask 077
flag=/etc/linpad-ssh/autostart
pref=/proc/ish/defaults/ssh_autostart
inittab=/etc/inittab
entry='::once:/usr/local/bin/linpad-ssh-start --boot </dev/null >/dev/null 2>&1'

fail() { printf '%s\n' "linpad-ssh-autostart: $*" >&2; exit 1; }
pref_state() {
    [ -r "$pref" ] || return 0
    value=$(tr -d ' \t\r\n' < "$pref" 2>/dev/null) || return 0
    case "$value" in true|1) printf '%s\n' on ;; false|0) printf '%s\n' off ;; esac
}
status() {
    [ ! -L "$flag" ] || fail "$flag must not be a symlink"
    state=$(pref_state)
    if [ -z "$state" ]; then
        if [ -f "$flag" ]; then state=on; else state=off; fi
    fi
    printf '%s\n' "$state"
    if [ "$state" = on ] && ! grep -Fqx "$entry" "$inittab" 2>/dev/null; then
        printf '%s\n' "linpad-ssh-autostart: $inittab has no boot entry; rerun tools/setup_guest_ssh.sh" >&2
    fi
}
set_pref() {
    [ -e "$pref" ] || return 0
    printf '%s\n' "$1" > "$pref" 2>/dev/null || fail "cannot change LinPad's SSH preference ($pref)"
}

case "$#:${1-}" in
    0: | 1:status) status ;;
    1:on)
        [ "$(id -u)" = 0 ] || fail 'run as guest root'
        [ ! -L "$flag" ] || fail "$flag must not be a symlink"
        [ -d "${flag%/*}" ] || fail "${flag%/*} is missing; rerun tools/setup_guest_ssh.sh"
        set_pref true
        : > "$flag"
        chmod 600 "$flag"
        status ;;
    1:off)
        [ "$(id -u)" = 0 ] || fail 'run as guest root'
        [ ! -L "$flag" ] || fail "$flag must not be a symlink"
        set_pref false
        rm -f "$flag"
        status ;;
    *) printf '%s\n' 'usage: linpad-ssh-autostart [on | off | status]' >&2; exit 2 ;;
esac
AUTOSTART_SWITCH
    chmod 755 "$switch_tmp"
    mv "$switch_tmp" "$switch"
    switch_tmp=''

    public_tmp=$(mktemp /tmp/.linpad-ssh-hostkey.XXXXXX)
    dropbearkey -y -f "$host_key" | awk '
        $1 == "ssh-ed25519" { print; found = 1 }
        END { exit !found }
    ' > "$public_tmp"
    chmod 600 "$public_tmp"
    mv "$public_tmp" /tmp/linpad-ssh-hostkey.pub
    "$helper"
    printf '%s\n' 'SSH_SETUP_OK'
    cat /tmp/linpad-ssh-hostkey.pub
}

main "$@"
# Keep a shell command after main for the native ISHShellExecutor launch path.
exit "$?"
