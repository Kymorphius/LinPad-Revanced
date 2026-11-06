#!/bin/sh
# LinPad Store's Debian pack: Debian (arm64, glibc) at /srv/debian next to Alpine, entered
# with `debian`. Runs as root. A system already at /srv/debian is kept; only the commands and
# settings are (re)installed.
#   DEBIAN_RELEASE  bookworm (default; the release LinPad was tested with)
#   DEBIAN_IMAGES   LXC image server (default images.linuxcontainers.org, else a mirror)
#   DEBIAN_MIRROR   apt mirror (default deb.debian.org, or the image mirror's when it is used)
set -eu
P=/usr/local/share/linpad/packs/debian
R=/srv/debian
RELEASE=${DEBIAN_RELEASE:-bookworm}
IMAGES=${DEBIAN_IMAGES:-}
MIRROR=${DEBIAN_MIRROR:-}
TUNA=https://mirrors.tuna.tsinghua.edu.cn
log() { printf '%s\n' "debian: $*"; }
die() { printf '%s\n' "debian: $*" >&2; exit 1; }

if [ ! -x "$R/usr/bin/apt-get" ]; then
    for tool in curl xz; do
        command -v $tool >/dev/null || apk add -q $tool || die "$tool is missing and could not be installed"
    done
    if [ -z "$IMAGES" ]; then
        if curl -fsS -m 15 -o /dev/null https://images.linuxcontainers.org/meta/1.0/index-system; then
            IMAGES=https://images.linuxcontainers.org
        else
            IMAGES=$TUNA/lxc-images
            [ -n "$MIRROR" ] || MIRROR=$TUNA
        fi
    fi
    build=$(curl -fsS -m 30 "$IMAGES/meta/1.0/index-system" 2>/dev/null |
        grep "^debian;$RELEASE;arm64;default;" | cut -d';' -f5 | tail -n 1) || true
    # mirrors without the index: the newest build in the directory listing
    [ -n "$build" ] || build=$(curl -fsS -m 30 "$IMAGES/images/debian/$RELEASE/arm64/default/" |
        grep -o -E '20[0-9]{6}_[0-9]{2}(%3A|:)[0-9]{2}' | sed 's/%3A/:/' | sort -u | tail -n 1) || true
    [ -n "$build" ] || die "no Debian $RELEASE arm64 image found at $IMAGES"
    dir="$IMAGES/images/debian/$RELEASE/arm64/default/$(echo "$build" | sed 's/:/%3A/')"
    tmp=$(mktemp -d /tmp/debian-pack.XXXXXX)
    trap 'rm -rf "$tmp" "$R.new"' EXIT
    log "downloading Debian $RELEASE ($build) from $IMAGES"
    curl -fsS -m 3600 -o "$tmp/rootfs.tar.xz" "$dir/rootfs.tar.xz" || die "download failed"
    sum=$(curl -fsS -m 30 "$dir/SHA256SUMS" | grep ' rootfs.tar.xz$' | cut -d' ' -f1) || true
    [ -n "$sum" ] || die "no checksum for the image"
    echo "$sum  $tmp/rootfs.tar.xz" | sha256sum -c -s - || die "checksum mismatch"
    log "unpacking into $R"
    rm -rf "$R.new"
    mkdir -p "$R.new"
    xz -dc "$tmp/rootfs.tar.xz" | tar -xf - -C "$R.new"
    mv "$R.new" "$R"
    [ -n "$MIRROR" ] || MIRROR=https://deb.debian.org
    key=/usr/share/keyrings/debian-archive-keyring.gpg
    # signed-by: the image has no awk, which apt-key needs to read its .asc keys
    cat > "$R/etc/apt/sources.list" <<EOS
deb [signed-by=$key] $MIRROR/debian $RELEASE main contrib
deb [signed-by=$key] $MIRROR/debian $RELEASE-updates main contrib
deb [signed-by=$key] $MIRROR/debian-security $RELEASE-security main contrib
EOS
    chmod 644 "$R/etc/apt/sources.list"
fi

install -m 755 "$P/debian" /usr/local/bin/debian
install -D -m 755 "$P/debian-prepare" /usr/local/sbin/debian-prepare
# Where LinPad gates Claude Code, a claude in Debian would skip the gate: put a pointer first.
if [ -x /usr/local/bin/claude-gate ]; then
    install -D -m 755 "$P/claude" "$R/usr/local/linpad/bin/claude"
    printf '%s\n' '# LinPad: Claude Code runs gated in Alpine; this placeholder says so' \
        'export PATH="/usr/local/linpad/bin:$PATH"' > "$R/etc/profile.d/linpad.sh"
    chmod 644 "$R/etc/profile.d/linpad.sh"
fi
# Members of the group debian use it as themselves
grep -q '^debian:' /etc/group || addgroup -S debian
command -v doas >/dev/null || apk add -q doas
mkdir -p /etc/doas.d
printf '%s\n' "# members of debian enter the Debian chroot as themselves (/usr/local/bin/debian drops back)" \
    "permit nopass :debian as root cmd /usr/local/bin/debian" > /etc/doas.d/debian.conf
chmod 640 /etc/doas.d/debian.conf
grep -q 'debian-prepare' /etc/inittab || printf '%s\n%s\n' \
    "# Debian chroot (/srv/debian): mount /proc and /dev/pts at every launch" \
    "::once:/usr/local/sbin/debian-prepare </dev/null >/dev/null 2>&1" >> /etc/inittab

/usr/local/sbin/debian-prepare
chroot "$R" /usr/bin/env true 2>/dev/null ||
    die "this LinPad cannot run Debian's programs yet: update LinPad, then install again"
log "ready: run debian (root, or a member of the group debian: addgroup USER debian)"
