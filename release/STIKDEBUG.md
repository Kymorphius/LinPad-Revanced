# Fast mode on the iPad: SideStore and StikDebug

LinPad's fast mode (native JIT) needs a debugger to prepare executable memory.
Besides [USB JIT from a Mac](USB-JIT.md), StikDebug can do this on the iPad
itself. With **Settings › Fast Mode › Automatic (StikDebug)**, every launch
opens
`stikdebug://enable-jit?bundle-id=<runtime bundle ID>&pid=<PID>&script-name=universal.js`;
StikDebug attaches with `universal.js`, completes the TXM protocol and returns
to LinPad.

## Apps

| App | Source | Role |
| --- | --- | --- |
| LocalDevVPN | App Store | Loopback VPN that SideStore and StikDebug need. iOS keeps one VPN at a time, so other VPNs or proxies disconnect while it is on |
| SideStore | Nightly build, installed with iloader's **SideStore (Nightly)** | Signs and refreshes apps with a free Apple ID; the stable build cannot refresh on iPadOS 26.4 and later |
| StikDebug | [Official release](https://github.com/StikDebug/StikDebug/releases) (no longer on the App Store), installed through SideStore | On-device debugger |
| LinPad | LinPad IPA imported into SideStore | Same bundle ID and App Group as the existing install, so Linux data is kept |

A free Apple ID allows three active sideloaded apps; SideStore, LinPad and
StikDebug use all three.

## Setup notes

1. **SideStore sign-in fails with -80045.** Apple rejects older anisette client
   data. In SideStore: Settings › Advanced Settings › User Customizations, turn
   off On-Device Anisette and restart; in Anisette Servers choose
   SideStore (New) and turn off automatic rotation; in Anisette Client
   Configuration › Remote Server Sync tap Fetch, then Save as Overrides; tap
   Reset adi.pb; force-quit, reopen, and sign in once.
2. **"The pairing file format is ambiguous".** A pairing file holding both
   Remote Pairing and Lockdown credentials is refused. Import a copy that keeps
   only `public_key`, `private_key`, `identifier` and `alt_irk`; StikDebug can
   use the same file. Pairing files contain private keys: keep any copies in a
   private directory and delete them afterwards.
3. **Refresh SideStore itself** before importing other apps.
4. **Memory.** With a free team SideStore does not request the increased memory
   limit capability. LinPad keeps its higher memory budget only when that
   capability is already enabled on its App ID by a client that requests it,
   and the IPA's binary declares `com.apple.developer.kernel.increased-memory-limit`.
   Check `allowance_mb` in `/proc/ish/memory` after installing.
5. **Close LinPad before installing it over itself.** While LinPad keeps running,
   SideStore's `installIpa()` can wait for hours.
6. **StikDebug reports "The device connection or developer disk image wasn't
   ready in time".** The iPad could not download the Developer Disk Image. Run
   `tools/stikdebug_push_ddi.sh` on a Mac connected over USB; it downloads the
   DDI files and copies them, with the mount-method marker StikDebug checks,
   into StikDebug's `Documents/DDI`. Then force-quit StikDebug and retry fast
   mode in LinPad.

## Refreshing

SideStore and its apps expire after seven days with a free Apple ID. Before
then, connect LocalDevVPN, open SideStore and tap **Refresh All**. If SideStore
itself has expired it cannot open: reinstall it from a computer with iloader
(same bundle ID, installed over the old copy), then refresh. After a refresh,
check `allowance_mb` again; whether a refresh keeps the memory capability
depends on the App ID, not on SideStore.
