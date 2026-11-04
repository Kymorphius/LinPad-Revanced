#!/usr/bin/env python3
"""Prove the generated resume hook detaches and installer preflight preserves data."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/setup_guest_ssh.sh"
TIMEOUT = shutil.which("timeout")
NOHUP = shutil.which("nohup")
assert TIMEOUT and NOHUP, "native timeout/nohup are required for the detached-hook fixture"


def executable(path, text):
    path.write_text(text)
    path.chmod(0o700)


def hook_source():
    setup = SOURCE.read_text()
    marker = "    cat > \"$resume_tmp\" <<'RESUME_HOOK'\n"
    assert setup.count(marker) == 1
    hook = setup.split(marker, 1)[1].split("\nRESUME_HOOK\n", 1)[0] + "\n"
    assert "/usr/bin/timeout 40 /usr/local/bin/linpad-ssh-start --recover-existing" in hook
    return hook


def run_event(event):
    with tempfile.TemporaryDirectory(prefix="linpad-ssh-resume-fixture-") as tmp:
        root = Path(tmp)
        helper, hook = root / "helper", root / "hook"
        calls, done = root / "calls", root / "done"
        executable(helper, f'''#!/bin/sh
printf '%s\\n' "$*" >> '{calls}'
sleep 1
: > '{done}'
''')
        script = hook_source().replace("/usr/bin/nohup", NOHUP).replace("/usr/bin/timeout", TIMEOUT)
        script = script.replace("/usr/local/bin/linpad-ssh-start", str(helper))
        executable(hook, script)
        subprocess.run(["/bin/sh", "-n", str(hook)], check=True)
        before = time.monotonic()
        result = subprocess.run([TIMEOUT, "5", str(hook), *event],
                                capture_output=True, text=True, timeout=6)
        elapsed = time.monotonic() - before
        assert result.returncode == 0, result
        assert elapsed < 0.8, (event, elapsed, "hook waited for helper")
        assert not done.exists(), "helper must still be running when the hook returns"
        if event == ["resume"]:
            deadline = time.monotonic() + 4
            while not done.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            assert done.exists(), "owned transient helper did not finish"
            assert calls.read_text() == "--recover-existing\n"
        else:
            time.sleep(0.05)
            assert not calls.exists(), (event, "unexpected helper launch")
        print(f"PASS event={event!r} returned={elapsed:.3f}s")


def boot_entry_source():
    setup = SOURCE.read_text()
    begin, end = "boot_entry='", "\nmain() {\n"
    assert setup.count(begin) == 1 and setup.count(end) == 1
    functions = setup[setup.index(begin):setup.index(end)]
    assert "::once:/usr/local/bin/linpad-ssh-start --boot " in functions
    return functions


def run_boot_entry(mode):
    """BusyBox init reads /etc/inittab at every launch, desktop or not."""
    with tempfile.TemporaryDirectory(prefix="linpad-ssh-inittab-fixture-") as tmp:
        root = Path(tmp)
        mockbin, inittab = root / "bin", root / "inittab"
        mockbin.mkdir()
        # Ownership is reported as root; modes come from the host stat.
        executable(mockbin / "stat", """#!/bin/sh
+case "$2" in
+  %u) printf '0\\n' ;;
+  %a) /usr/bin/stat -c %a "$3" 2>/dev/null || /usr/bin/stat -f %Lp "$3" ;;
+  *) exit 2 ;;
+esac
+""".replace("\n+", "\n"))
        original = "::sysinit:/sbin/openrc sysinit\ntty1::respawn:/sbin/getty 38400 tty1"
        target = root / "user-inittab"
        if mode in ("fresh", "rerun"):
            inittab.write_text(original)  # no final newline
            inittab.chmod(0o644)
        elif mode == "symlink":
            target.write_text(original + "\n")
            inittab.symlink_to(target)
        script = boot_entry_source() + f'install_boot_entry "{inittab}"\n'
        if mode == "rerun":
            script += f'install_boot_entry "{inittab}"\n'
        env = dict(os.environ, PATH=str(mockbin) + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["/bin/sh", "-c", "set -eu\n" + script], env=env,
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, (mode, result)
        if mode in ("fresh", "rerun"):
            lines = inittab.read_text().split("\n")
            assert lines[:2] == original.split("\n"), (mode, lines)
            assert lines[2].startswith("# LinPad SSH:"), (mode, lines)
            assert lines[3] == "::once:/usr/local/bin/linpad-ssh-start --boot </dev/null >/dev/null 2>&1"
            assert lines[4:] == [""], (mode, "entry added more than once")
            assert inittab.stat().st_mode & 0o777 == 0o644, (mode, oct(inittab.stat().st_mode))
            assert not list(root.glob("inittab.linpad-ssh.*")), (mode, "temporary file left")
        elif mode == "symlink":
            assert inittab.is_symlink() and target.read_text() == original + "\n"
            assert "will not start at launch" in result.stderr, result.stderr
        else:
            assert not inittab.exists() and "will not start at launch" in result.stderr
        print(f"PASS inittab={mode}")


def switch_source():
    setup = SOURCE.read_text()
    marker = "    cat > \"$switch_tmp\" <<'AUTOSTART_SWITCH'\n"
    assert setup.count(marker) == 1
    switch = setup.split(marker, 1)[1].split("\nAUTOSTART_SWITCH\n", 1)[0] + "\n"
    # The switch must look for exactly the entry the installer writes.
    installer_entry = setup.split("boot_entry='", 1)[1].split("'\n", 1)[0]
    assert f"entry='{installer_entry}'" in switch
    return switch


def run_switch():
    """linpad-ssh-autostart on/off/status: one word out, root only, no symlinks."""
    with tempfile.TemporaryDirectory(prefix="linpad-ssh-switch-fixture-") as tmp:
        root = Path(tmp)
        etc, mockbin = root / "etc", root / "bin"
        (etc / "linpad-ssh").mkdir(parents=True)
        mockbin.mkdir()
        flag, inittab = etc / "linpad-ssh/autostart", etc / "inittab"
        switch = root / "linpad-ssh-autostart"
        pref = root / "proc-ish-defaults-ssh_autostart"
        executable(switch, switch_source().replace("/etc/linpad-ssh/autostart", str(flag))
                   .replace("flag=/etc", f"flag={etc}").replace("inittab=/etc/inittab", f"inittab={inittab}")
                   .replace("pref=/proc/ish/defaults/ssh_autostart", f"pref={pref}"))
        subprocess.run(["/bin/sh", "-n", str(switch)], check=True)
        uid = mockbin / "uid"

        def run(*args, as_root=True):
            uid.write_text("0" if as_root else "501")
            executable(mockbin / "id", f"#!/bin/sh\ncat '{uid}'\n")
            env = dict(os.environ, PATH=str(mockbin) + os.pathsep + os.environ["PATH"])
            return subprocess.run([str(switch), *args], env=env, capture_output=True, text=True, timeout=5)

        r = run()
        assert (r.returncode, r.stdout) == (0, "off\n"), r
        inittab.write_text("tty1::respawn:/sbin/getty 38400 tty1\n")
        r = run("on")
        assert (r.returncode, r.stdout) == (0, "on\n") and flag.is_file(), r
        assert flag.stat().st_mode & 0o777 == 0o600
        assert "no boot entry" in r.stderr, r.stderr
        inittab.write_text(inittab.read_text() +
                           "::once:/usr/local/bin/linpad-ssh-start --boot </dev/null >/dev/null 2>&1\n")
        r = run("status")
        assert (r.returncode, r.stdout, r.stderr) == (0, "on\n", ""), r
        r = run("off", as_root=False)
        assert r.returncode == 1 and flag.exists(), (r, "non-root changed the switch")
        r = run("off")
        assert (r.returncode, r.stdout) == (0, "off\n") and not flag.exists(), r
        target = root / "user-file"
        target.write_text("kept\n")
        flag.symlink_to(target)
        for args in (("on",), ("off",), ("status",)):
            r = run(*args)
            assert r.returncode == 1 and target.read_text() == "kept\n" and flag.is_symlink(), (args, r)
        flag.unlink()
        r = run("toggle")
        assert r.returncode == 2 and "usage" in r.stderr, r
        # With LinPad's preference present: it is the source of truth and both are written.
        pref.write_text("false\n")
        flag.write_text("")
        r = run()
        assert r.stdout == "off\n", (r, "preference must win over the marker")
        r = run("on")
        assert (r.returncode, r.stdout) == (0, "on\n") and pref.read_text() == "true\n" and flag.exists(), r
        r = run("off")
        assert (r.returncode, r.stdout) == (0, "off\n") and pref.read_text() == "false\n" and not flag.exists(), r
        pref.write_text("garbage\n")
        r = run()
        assert r.stdout == "off\n", (r, "unreadable preference falls back to the marker")
        print("PASS autostart switch on/off/status, root-only, symlink-safe, follows the app preference")


def preflight_case(mode):
    with tempfile.TemporaryDirectory(prefix="linpad-ssh-install-fixture-") as tmp:
        root = Path(tmp)
        etc, home, mockbin = root / "etc", root / "root", root / "bin"
        hooks = etc / "linpad/lifecycle.d"
        hooks.mkdir(parents=True)
        (home / ".ssh").mkdir(parents=True)
        (etc / "linpad-ssh").mkdir()
        mockbin.mkdir()
        auth, key = home / ".ssh/authorized_keys", etc / "linpad-ssh/dropbear_ed25519_host_key"
        auth.write_text("preserved-auth-fixture\n")
        key.write_text("preserved-key-fixture-never-read\n")
        custom = hooks / "10-custom"
        custom.write_text("preserved user hook\n")
        target = root / "custom-target"
        target.write_text("preserved symlink target\n")
        destination = hooks / "50-linpad-ssh"
        if mode == "unknown-hook":
            destination.write_text("#!/bin/sh\n# User custom hook\n")
        elif mode == "symlink-hook":
            destination.symlink_to(target)
        elif mode == "symlink-directory":
            hooks.rename(etc / "saved-hooks")
            hooks.symlink_to(etc / "saved-hooks", target_is_directory=True)
        elif mode == "symlink-parent":
            (etc / "linpad").rename(etc / "saved-linpad")
            (etc / "linpad").symlink_to(etc / "saved-linpad", target_is_directory=True)
        elif mode == "unknown-session-hook":
            (etc / "ishwl/session.d").mkdir(parents=True)
            destination = etc / "ishwl/session.d/60-linpad-ssh.sh"
            destination.write_text("#!/bin/sh\n# User custom hook\n")
        elif mode == "symlink-session-hook":
            (etc / "ishwl/session.d").mkdir(parents=True)
            destination = etc / "ishwl/session.d/60-linpad-ssh.sh"
            destination.symlink_to(target)
        executable(mockbin / "id", "#!/bin/sh\nprintf '0\\n'\n")
        executable(mockbin / "stat", "#!/bin/sh\nprintf '0\\n'\n")
        script = SOURCE.read_text().replace("/etc", str(etc)).replace("/root/.ssh", str(home / ".ssh"))
        script = script.replace("/usr/local/bin", str(root / "local-bin"))
        tested = root / "setup.sh"
        executable(tested, script)
        env = dict(os.environ, PATH=str(mockbin) + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["/bin/sh", str(tested)], input="ssh-ed25519 AAAA fixture\n",
                                capture_output=True, text=True, timeout=5, env=env)
        assert result.returncode == 1, (mode, result)
        assert "refusing" in result.stderr, (mode, result.stderr)
        assert auth.read_text() == "preserved-auth-fixture\n"
        assert key.read_text() == "preserved-key-fixture-never-read\n"
        assert custom.read_text() == "preserved user hook\n"
        assert target.read_text() == "preserved symlink target\n"
        if mode in ("unknown-hook", "unknown-session-hook"):
            assert destination.read_text() == "#!/bin/sh\n# User custom hook\n"
        assert not (etc / "linpad-ssh/autostart").exists(), (mode, "autostart enabled before refusal")
        print(f"PASS preflight={mode} authentication and hooks preserved")


if __name__ == "__main__":
    for event in (["resume"], ["suspend"], ["other"], []):
        run_event(event)
    for mode in ("fresh", "rerun", "symlink", "missing"):
        run_boot_entry(mode)
    run_switch()
    for mode in ("unknown-hook", "symlink-hook", "symlink-directory", "symlink-parent",
                 "unknown-session-hook", "symlink-session-hook"):
        preflight_case(mode)
