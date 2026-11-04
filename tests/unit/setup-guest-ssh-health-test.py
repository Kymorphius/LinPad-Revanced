#!/usr/bin/env python3
"""Run the generated SSH helper against private fake /proc and commands.

Every signal and network command is replaced in a test copy. Real keys,
processes, services and device files are never accessed.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/setup_guest_ssh.sh"


def executable(path, text):
    path.write_text(text)
    path.chmod(0o700)


def helper_source():
    setup = SOURCE.read_text()
    marker = "    cat > \"$helper_tmp\" <<'HELPER'\n"
    assert setup.count(marker) == 1
    return setup.split(marker, 1)[1].split("\nHELPER\n", 1)[0] + "\n"


def process(root, pid, start=123, ppid=1):
    directory = root / "proc" / str(pid)
    directory.mkdir(parents=True)
    fields = ["0"] * 50
    fields[0], fields[1], fields[19] = "S", str(ppid), str(start)
    (directory / "stat").write_text(f"{pid} (dropbear) " + " ".join(fields) + "\n")
    (directory / "status").write_text(
        f"Name:\tdropbear\nTgid:\t{pid}\nPid:\t{pid}\nPPid:\t{ppid}\nUid:\t0\t0\t0\t0\n"
    )
    (directory / "exe").symlink_to("/usr/sbin/dropbear")


# Fixture modes with a readable launch start (1500) and their runtime-file mtime.
SESSION_MODES = {"stale-lock": 100, "current-lock": 1499, "stale-pid-reused": 100,
                 "boot-on": 1600, "boot-stale": 100, "boot-pref-on": 1600,
                 "boot-pref-off": 1600, "boot-pref-invalid": 1600}


def run_case(mode, args, code, events):
    with tempfile.TemporaryDirectory(prefix="linpad-ssh-health-fixture-") as tmp:
        root = Path(tmp)
        state, mockbin = root / "state", root / "bin"
        state.mkdir()
        mockbin.mkdir()
        process(root, 99, 900)  # Test copy's lock owner, never a real PID.
        process(root, 42, 123, 2 if mode in ("mismatched", "stale-pid-reused") else 1)
        process(root, 43, 124)  # Existing SSH child/session must survive.
        process(root, 999, 456)
        shutil.copytree(root / "proc/999", root / "new-master", symlinks=True)
        shutil.rmtree(root / "proc/999")
        (state / "server.pid").write_text("42\n")
        preserved = {
            root / "host-key": b"fixture-key-do-not-read-or-replace\n",
            root / "authorized_keys": b"fixture-public-data\n",
            root / "user-data": b"existing session and user files\n",
            root / "proc/43/stat": (root / "proc/43/stat").read_bytes(),
        }
        for path, contents in preserved.items():
            path.write_bytes(contents)
        (root / "events").write_text("")
        if mode == "off-missing-state":
            shutil.rmtree(state)
        elif mode == "off-missing":
            (state / "server.pid").unlink()
        elif mode == "off-empty":
            (state / "server.pid").write_text("")
        elif mode == "off-dead":
            shutil.rmtree(root / "proc/42")
        elif mode == "malformed":
            (state / "server.pid").write_text("not-a-pid\n")
        elif mode == "concurrent":
            (state / "start.lock").mkdir()
            (state / "start.lock/owner").write_text("unknown owner\n")
        elif mode in ("stale-lock", "current-lock"):
            (state / "start.lock").mkdir()
            (state / "start.lock/owner").write_text("unknown owner\n")
        if mode in SESSION_MODES:
            # This launch's PID 1 started at btime 1000 + 50000 ticks = 1500.
            (root / "proc/stat").write_text("cpu  0 0 0 0\nbtime 1000\n")
            process(root, 1, 50000, 0)
        if mode in ("boot-on", "boot-stale", "boot-pref-off", "boot-pref-invalid"):
            (root / "autostart").write_text("")
        prefs = {"boot-pref-on": "true\n", "boot-pref-off": "false\n", "boot-pref-invalid": "\"yes\"\n"}
        if mode in prefs:
            (root / "proc/ish/defaults").mkdir(parents=True)
            (root / "proc/ish/defaults/ssh_autostart").write_text(prefs[mode])
        if mode == "symlink-pid":
            (state / "server.pid").unlink()
            (state / "server.pid").symlink_to(root / "user-data")
        executable(mockbin / "id", "#!/bin/sh\nprintf '0\\n'\n")
        executable(mockbin / "stat", '#!/bin/sh\ncase "$2" in %Y) printf \'%s\\n\' "${STAT_MTIME:-0}" ;; '
                   "*) printf '0\\n' ;; esac\n")
        executable(mockbin / "timeout", '#!/bin/sh\nshift\nexec "$@"\n')
        executable(mockbin / "sleep", '#!/bin/sh\nexit 0\n')
        executable(mockbin / "nc", '''#!/bin/sh
+n=0
+[ ! -f "$FIXTURE/calls" ] || read -r n < "$FIXTURE/calls"
+n=$((n + 1))
+printf '%s\n' "$n" > "$FIXTURE/calls"
+if [ "$MODE" = healthy ] || [ "$MODE" = jitter ] || [ "$MODE" = boot-on ] || [ "$MODE" = boot-pref-on ] || [ "$MODE" = boot-pref-invalid ] || [ -f "$FIXTURE/restarted" ]; then
+    [ "$MODE" != missing-banner ] || exit 0
+    printf 'SSH-2.0-dropbear_fixture\r\n'
+fi
+if [ "$MODE" = reuse-before-term ] && [ "$n" = 2 ]; then
+    sed 's/ 123 / 1123 /' "$FIXTURE/proc/42/stat" > "$FIXTURE/stat.new"
+    mv "$FIXTURE/stat.new" "$FIXTURE/proc/42/stat"
+fi
+'''.replace("\n+", "\n"))
        executable(mockbin / "kill", '''#!/bin/sh
+case "$1" in
+ -0) [ -d "$FIXTURE/proc/$2" ] ;;
+ -TERM)
+    printf 'TERM %s\n' "$2" >> "$FIXTURE/events"
+    case "$MODE" in
+      reuse-after-term) sed 's/ 123 / 1123 /' "$FIXTURE/proc/$2/stat" > "$FIXTURE/stat.new"; mv "$FIXTURE/stat.new" "$FIXTURE/proc/$2/stat" ;;
+      master-stays) : ;;
+      *) rm -r "$FIXTURE/proc/$2" ;;
+    esac ;;
+ *) exit 99 ;;
+esac
+'''.replace("\n+", "\n"))
        executable(mockbin / "dropbear", '''#!/bin/sh
+printf 'START\n' >> "$FIXTURE/events"
+cp -R "$FIXTURE/new-master" "$FIXTURE/proc/999"
+printf '999\n' > "$FIXTURE/state/server.pid"
+: > "$FIXTURE/restarted"
+'''.replace("\n+", "\n"))
        script = helper_source()
        replacements = {
            "state_dir=/run/linpad-ssh": f"state_dir='{state}'",
            "host_key=/etc/linpad-ssh/dropbear_ed25519_host_key": f"host_key='{root / 'host-key'}'",
            "autostart_flag=/etc/linpad-ssh/autostart": f"autostart_flag='{root / 'autostart'}'",
            "proc_root=/proc": f"proc_root='{root / 'proc'}'",
            "timeout=/usr/bin/timeout": f"timeout='{mockbin / 'timeout'}'",
            "nc=/usr/bin/nc": f"nc='{mockbin / 'nc'}'",
            "self_pid=$$": "self_pid=99",
            'kill -0 "$expected_pid"': f'"{mockbin / "kill"}" -0 "$expected_pid"',
            'kill -TERM "$expected_pid"': f'"{mockbin / "kill"}" -TERM "$expected_pid"',
            '/usr/sbin/dropbear -E': f'"{mockbin / "dropbear"}" -E',
        }
        for old, new in replacements.items():
            assert script.count(old) == 1, (mode, old)
            script = script.replace(old, new)
        old = 'kill -0 "$pid"'
        assert script.count(old) == 2
        script = script.replace(old, f'"{mockbin / "kill"}" -0 "$pid"')
        if mode == "jitter":
            # read_start captures123, later reads124: accept the documented1tick.
            old = 'expected_start=$proc_start\n            master_matches'
            assert script.count(old) == 1
            script = script.replace(old, 'expected_start=$proc_start\n            sed \'s/ 123 / 124 /\' "$proc_root/$pid/stat" > "$state_dir/stat.new"\n            mv "$state_dir/stat.new" "$proc_root/$pid/stat"\n            master_matches')
        tested = root / "helper.sh"
        executable(tested, script)
        env = dict(os.environ, FIXTURE=str(root), MODE=mode, STAT_MTIME=str(SESSION_MODES.get(mode, 0)),
                   PATH=str(mockbin) + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["/bin/sh", str(tested), *args], env=env,
                                capture_output=True, text=True, timeout=8)
        assert result.returncode == code, (mode, result.returncode, result.stdout, result.stderr)
        assert (root / "events").read_text() == events, (mode, (root / "events").read_text())
        for path, contents in preserved.items():
            assert path.read_bytes() == contents, (mode, path)
        if mode in ("concurrent", "current-lock"):
            assert (state / "start.lock/owner").read_text() == "unknown owner\n"
        elif state.exists():
            assert not (state / "start.lock").exists(), (mode, "own lock leaked")
        if mode == "off-missing-state":
            assert not state.exists()
        if mode in ("boot-off", "boot-pref-off"):
            assert (state / "server.pid").read_text() == "42\n", "autostart off touched runtime state"
            assert "autostart is off" in result.stdout, (mode, result.stdout)
        if mode in ("boot-on", "boot-pref-on", "boot-pref-invalid"):
            assert "already running" in result.stdout, (mode, result.stdout)
        print(f"PASS {mode} {' '.join(args) or 'manual-start'}")


if __name__ == "__main__":
    subprocess.run(["/bin/sh", "-n", str(SOURCE)], check=True)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".sh") as file:
        file.write(helper_source())
        file.flush()
        subprocess.run(["/bin/sh", "-n", file.name], check=True)
    for case in (
        ("healthy", [], 0, ""),
        ("healthy", ["--recover-existing"], 0, ""),
        ("jitter", ["--recover-existing"], 0, ""),
        ("broken", [], 0, "TERM 42\nSTART\n"),
        ("broken", ["--recover-existing"], 0, "TERM 42\nSTART\n"),
        ("off-missing-state", ["--recover-existing"], 0, ""),
        ("off-missing", ["--recover-existing"], 0, ""),
        ("off-empty", ["--recover-existing"], 0, ""),
        ("off-dead", ["--recover-existing"], 0, ""),
        ("malformed", ["--recover-existing"], 1, ""),
        ("mismatched", ["--recover-existing"], 1, ""),
        ("reuse-before-term", [], 1, ""),
        ("reuse-after-term", [], 1, "TERM 42\n"),
        ("master-stays", [], 1, "TERM 42\n"),
        ("concurrent", [], 1, ""),
        ("symlink-pid", [], 1, ""),
        ("missing-banner", [], 1, "TERM 42\nSTART\n"),
        # Runtime files from a previous LinPad launch are discarded, never signalled.
        ("stale-lock", [], 0, "START\n"),
        ("current-lock", [], 1, ""),
        ("stale-pid-reused", [], 0, "START\n"),
        ("boot-off", ["--boot"], 0, ""),
        ("boot-on", ["--boot"], 0, ""),
        ("boot-stale", ["--boot"], 0, "START\n"),
        # The app preference wins over the marker; an unreadable value falls back to it.
        ("boot-pref-on", ["--boot"], 0, ""),
        ("boot-pref-off", ["--boot"], 0, ""),
        ("boot-pref-invalid", ["--boot"], 0, ""),
    ):
        run_case(*case)
