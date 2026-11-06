#!/bin/sh
# Removes LinPad Store's Debian pack: /srv/debian (everything installed in it) and its commands.
set -u
R=/srv/debian
for m in "$R/dev/pts" "$R/proc"; do
    mountpoint -q "$m" 2>/dev/null && umount "$m"
done
if grep -q 'debian-prepare' /etc/inittab; then
    grep -v -e 'debian-prepare' -e '^# Debian chroot (/srv/debian)' /etc/inittab > /etc/inittab.new &&
        cat /etc/inittab.new > /etc/inittab && rm -f /etc/inittab.new
fi
rm -f /usr/local/bin/debian /usr/local/sbin/debian-prepare /etc/doas.d/debian.conf
grep -q '^debian:' /etc/group && delgroup debian 2>/dev/null
rm -rf "$R" "$R.new"
echo "debian: removed"
