# Optional guest SSH

`setup_guest_ssh.sh` configures a key-only Dropbear service inside the LinPad
Linux guest. Run it as guest root and provide one client `ssh-ed25519` public
key on standard input. Never provide a private key.

Copy the script and the public key into the guest, then run:

```sh
sh /path/to/setup_guest_ssh.sh < /path/to/client_ed25519.pub
```

The installer uses Alpine's `apk` to install Dropbear if necessary. The guest
must also provide the BusyBox-compatible `/usr/bin/timeout`, `/usr/bin/nc` and
`/usr/bin/nohup` commands, plus the standard shell utilities. The installer
preserves existing authorized keys and reuses the existing SSH host key.

Successful setup starts SSH on IPv4 port **2222** and prints `SSH_SETUP_OK`
followed by the server's public host key. Password authentication is disabled.
Verify that public host key through the trusted guest console before accepting
the server on the first network connection.
The service listens on all IPv4 interfaces, so use the iPad's reachable address
and port 2222 when connecting with the matching client key.

## Start at LinPad launch

Setup appends one entry to `/etc/inittab`, preserving every existing line,
mode and owner:

```
::once:/usr/local/bin/linpad-ssh-start --boot </dev/null >/dev/null 2>&1
```

BusyBox init (guest PID 1) reads it at every LinPad launch, whether or not the
desktop starts, and does not wait for it. The helper bounds its own runtime.
`--boot` follows LinPad's preference **Start SSH at Launch** (iPad Settings ›
LinPad › SSH, and Settings › SSH in the desktop; on by default), which the guest
sees as `/proc/ish/defaults/ssh_autostart` (`true`/`false`). Apps without that
preference fall back to the marker `/etc/linpad-ssh/autostart`, which the first
installation creates. `linpad-ssh-autostart on`, `off` or `status` changes or
prints the same setting (one word, `on` or `off`): it writes the preference and
mirrors it in the marker; changes need root and refuse a symlinked marker.
Changes take effect at the next launch. Reruns keep the existing choice and
never add a second entry. If `/etc/inittab` is not a
root-owned regular file, setup leaves it alone and reports that SSH will not
start at launch. An earlier desktop-only hook,
`/etc/ishwl/session.d/60-linpad-ssh.sh`, is removed when recognized.

The guest's `/run` survives LinPad relaunches. Every helper mode therefore
reads the current launch's start time (`/proc/stat` `btime` plus guest PID 1's
start ticks) and discards a root-owned `start.lock` or PID file only when its
modification time is more than two seconds older. Such a file belongs to a
previous launch, whose processes no longer exist; its recorded PID is never
signalled. Files from the current launch keep the fail-closed rules below.

## Start and resume recovery

Start the configured service manually with:

```sh
linpad-ssh-start
```

The helper verifies an existing master's identity and SSH banner before
reporting it healthy. After two failed banner checks, it sends one `TERM` only
to the verified root Dropbear master, waits for that process to exit, and starts
one replacement using the same host key and options. It does not signal SSH
session children or other Linux tasks. Operations have a fixed deadline and an
atomic lock; there is no resident polling process.

Setup also installs `/etc/linpad/lifecycle.d/50-linpad-ssh`. On the desktop
lifecycle's `resume` event, this hook detaches a bounded check equivalent to:

```sh
linpad-ssh-start --recover-existing
```

This mode leaves an absent or stopped service off. It only recovers a service
whose live master can be identified from the managed PID file and `/proc`.
Unknown process identities, changed PID files, symlinked runtime files, or an
existing `start.lock` from the current launch are refused rather than
overwritten. Such a lock requires inspection; the helper removes a lock only
when its age proves that it predates this launch.

This recovery is limited to SSH configured by this installer and the existing
desktop lifecycle hook. It does not repair arbitrary listeners, restart LinPad,
or change the Linux image or background-execution settings.
