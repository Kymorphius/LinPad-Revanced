#!/bin/sh
# Give StikDebug its Developer Disk Image files over USB when the iPad cannot
# download them itself (StikDebug then fails with "The device connection or
# developer disk image wasn't ready in time"). See release/STIKDEBUG.md.
#
#   tools/stikdebug_push_ddi.sh
#
# StikDebug 3.2 keeps the files in Documents/DDI and only uses them when its
# recorded mount method matches the iPadOS version (cryptex on 26.4 and later,
# personalized before); otherwise it deletes and re-downloads them. A
# Documents/DDI/MountMethod file holding exactly that word is read once and
# then replaced by StikDebug's own preference.
set -eu

method=${STIKDEBUG_DDI_METHOD:-cryptex}
fail() { printf '%s\n' "stikdebug-ddi: $*" >&2; exit 1; }

udid=${LINPAD_DEVICE_UDID:-}
if [ -z "$udid" ] && [ -f "$HOME/.config/linpad/ssh-device-udid" ]; then
    IFS= read -r udid < "$HOME/.config/linpad/ssh-device-udid" || :
fi
[ -n "$udid" ] || fail 'set LINPAD_DEVICE_UDID or ~/.config/linpad/ssh-device-udid'

# Sideloaders append the team ID, so find the installed StikDebug unless given.
bundle_id=${STIKDEBUG_BUNDLE_ID:-}
if [ -z "$bundle_id" ]; then
    bundle_id=$(xcrun devicectl device info apps --device "$udid" 2>/dev/null |
        awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^com\.stik\.stikdebug(\.|$)/) print $i }' | sort -u)
    [ -n "$bundle_id" ] || fail 'StikDebug is not installed on the iPad'
    [ "$(printf '%s\n' "$bundle_id" | wc -l)" -eq 1 ] ||
        fail "several StikDebug copies installed; set STIKDEBUG_BUNDLE_ID to one of: $(printf '%s ' $bundle_id)"
fi

case "$method" in
    cryptex)
        base=https://github.com/doronz88/DeveloperDiskImage/raw/refs/heads/main/PersonalizedImages/Xcode_iOS_DDI_Cryptex
        files='BuildManifest.plist Image.dmg Image.dmg.trustcache Image.dmg.cryptex_info Image.dmg.root_hash' ;;
    personalized)
        base=https://github.com/doronz88/DeveloperDiskImage/raw/refs/heads/main/PersonalizedImages/Xcode_iOS_DDI_Personalized
        files='BuildManifest.plist Image.dmg Image.dmg.trustcache' ;;
    *) fail "unknown STIKDEBUG_DDI_METHOD $method" ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/stikdebug-ddi.XXXXXX")
trap 'rm -rf "$work"' 0
for f in $files; do
    curl -fsSL --retry 2 -o "$work/$f" "$base/$f" || fail "download failed: $f"
    [ -s "$work/$f" ] || fail "empty download: $f"
done
plutil -lint "$work/BuildManifest.plist" >/dev/null || fail 'BuildManifest.plist is not a valid plist'
# No trailing newline: StikDebug compares the file content with the raw value.
printf '%s' "$method" > "$work/MountMethod"

for f in $files MountMethod; do
    xcrun devicectl device copy to --device "$udid" --domain-type appDataContainer \
        --domain-identifier "$bundle_id" --source "$work/$f" --destination "Documents/DDI/$f" \
        >/dev/null 2>&1 || fail "could not copy $f into $bundle_id (is StikDebug installed and the iPad unlocked?)"
    printf 'pushed %s\n' "$f"
done
printf '%s\n' 'Close StikDebug from the app switcher, connect LocalDevVPN, then retry fast mode in LinPad.'
