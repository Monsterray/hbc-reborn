#!/usr/bin/env python3
"""Run every Dolphin test one at a time and print a table of results and seconds.

Only one Dolphin can hold the guest's port 4299 on a PC, so the tests run in a
row. Before each one this waits (--wait minutes) while any Dolphin runs: it is
somebody else's (another worker's test) and is never killed. Only a Dolphin that
one of this script's own test processes left behind (after a time-out) is killed
with that test. A failing test
runs once more; the table says "FAIL then PASS" for a flaky one. What a test
needs and is not built yet (mboard_app's Dolphin DOL, agent_app's crash DOLs, the
other test apps) is built first, in devkitPro's MSYS2 shell on Windows, with
devkitPPC r41-2 for the libogc2 DOLs (see tests/README.md).

--special also builds the two fault-injection channels in a temporary git
worktree of HEAD (EXTRA_CFLAGS=-DHBC_TEST_INIT_FAILURE for a DOL, then
-DHBC_TEST_STUCK_NET_CLEANUP for a WAD; loader.o is deleted between the two
builds) and runs dolphin_network_retry.py and dolphin_network_recovery.py (default
and --reload) on them. Never put those builds on a Wii. They are built from the
last commit, so commit a change first.

usage: tests/dolphin/run_all.py [--only NAME,NAME] [--skip NAME,NAME] [--special]
                                [--no-build] [--wait MINUTES] [--out DIR] [--list]
Logs go to --out (default: a folder in the temp directory); WII_BENCH_LOG_PORT
(the network-log port) is passed on, so set it when other workers run tests too.
Exit status 0 only when every test passed.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path

DOLPHIN_DIR = root / "tests" / "dolphin"
APPS = root / "tests" / "apps"
AGENT_CRASH2 = APPS / "agent_app/agent_app-libogc2-crash.dol"
AGENT_CRASH = APPS / "agent_app/agent_app-crash.dol"

# name, script, arguments, files that must exist, seconds allowed
TESTS = [
    ("smoke", "dolphin_smoke.py", [], [], 300),
    ("slotled", "dolphin_slotled.py", [], ["apps/slotled_app/slotled_app.dol"], 300),
    ("usbguard", "dolphin_usbguard.py", [], ["apps/usbguard_app/usbguard_app.dol"], 300),
    ("wpad_queue", "dolphin_wpad_queue.py", [],
     ["apps/wpad_queue_app/wpad_queue_app-fixed.dol"], 300),
    ("mii", "dolphin_mii.py", [], ["apps/mii_app/mii_app.dol"], 300),
    ("replug", "dolphin_replug.py", [], [], 300),
    ("options_edges", "dolphin_options_edges.py", [], [], 300),
    ("app_badges", "dolphin_app_badges.py", [], ["apps/agent_app/agent_app.dol"], 600),
    ("transfer_progress", "dolphin_transfer_progress.py", [], ["apps/agent_app/agent_app.dol"], 600),
    ("exit_menu", "dolphin_exit_menu.py", [], [], 300),
    ("meta", "dolphin_meta.py", [], ["apps/agent_app/agent_app.dol"], 600),
    ("mii_picker", "dolphin_mii_picker.py", [], ["apps/agent_app/agent_app.dol"], 600),
    ("mii_playground", "dolphin_mii_playground.py", [], [], 300),
    ("way_out", "dolphin_way_out.py", [], ["apps/agent_app/agent_app.dol"], 300),
    ("ogc_crash", "dolphin_ogc_crash.py", [], ["apps/agent_app/agent_app-libogc2-crash.dol"], 300),
    ("ogc_crash --bad-guard", "dolphin_ogc_crash.py", ["--bad-guard", str(AGENT_CRASH)],
     ["apps/agent_app/agent_app-crash.dol"], 300),
    ("ogc_crash --hook-fault", "dolphin_ogc_crash.py", ["--hook-fault", str(AGENT_CRASH)],
     ["apps/agent_app/agent_app-crash.dol"], 300),
    ("mboard --restore", "dolphin_mboard.py", ["--restore"],
     ["apps/mboard_app/mboard_app_dolphin.dol", "apps/nandput/nandput.dol"], 600),
]

# what builds a file: (make directory under tests/apps, make arguments)
BUILDS = {
    "apps/slotled_app/slotled_app.dol": ("slotled_app", []),
    "apps/usbguard_app/usbguard_app.dol": ("usbguard_app", []),
    "apps/wpad_queue_app/wpad_queue_app-fixed.dol": ("wpad_queue_app", []),
    "apps/mii_app/mii_app.dol": ("mii_app", []),
    "apps/agent_app/agent_app.dol": ("agent_app", []),
    "apps/agent_app/agent_app-crash.dol": ("agent_app", ["MODE=crash"]),
    "apps/agent_app/agent_app-libogc2-crash.dol": ("agent_app", ["OGC=libogc2", "MODE=crash"]),
    "apps/mboard_app/mboard_app_dolphin.dol": ("mboard_app", ["dolphin"]),
    "apps/nandput/nandput.dol": ("nandput", []),
}


def msys_bash():
    """devkitPro's MSYS2 bash on Windows ($MSYS2_BASH, or msys2/ beside $DEVKITPRO's
    Windows folder, or devkitPro's standard install folder); None elsewhere."""
    if os.name != "nt":
        return None
    named = os.environ.get("MSYS2_BASH")
    cands = [named] if named else []
    for var in ("DEVKITPRO_WIN", "DEVKITPRO"):
        if os.environ.get(var):
            cands.append(str(pathlib.Path(os.environ[var]) / "msys2/usr/bin/bash.exe"))
    cands.append("C:/devkitPro/msys2/usr/bin/bash.exe")
    for c in cands:
        if c and pathlib.Path(c).is_file():
            return c
    raise SystemExit("devkitPro's MSYS2 bash not found: set MSYS2_BASH to its bash.exe")


def posix(path):
    """A Windows path as MSYS2 writes it (C:/x -> /c/x)."""
    p = pathlib.Path(path).resolve().as_posix()
    return f"/{p[0].lower()}{p[2:]}" if len(p) > 2 and p[1] == ":" else p


def make(directory, args, cwd_root=root):
    """make -C directory args, with devkitPro's environment; raises on failure."""
    bash = msys_bash()
    py = sys.executable
    margs = " ".join(args)
    if bash:
        script = ("export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC; "
                  "export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH; "
                  f"make -C '{posix(directory)}' {margs}")
        cmd = [bash, "-lc", script]
    else:
        cmd = ["make", "-C", str(directory)] + list(args)
    print(f"build: make -C {pathlib.Path(directory).relative_to(cwd_root)} {margs}", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print((r.stdout + r.stderr)[-3000:])
        raise RuntimeError(f"make -C {directory} {margs} failed")
    return py


def build_missing(tests):
    for _name, _script, _args, files, _t in tests:
        for rel in files:
            if (root / "tests" / rel).is_file():
                continue
            if rel not in BUILDS:
                raise SystemExit(f"{rel} is missing and run_all.py does not know how to build it")
            folder, margs = BUILDS[rel]
            make(APPS / folder, margs)
            if not (root / "tests" / rel).is_file():
                raise SystemExit(f"built {folder} {margs} but {rel} is still missing")


def dolphin_processes():
    """[(pid, parent pid, command line)] of every running Dolphin on this PC."""
    if os.name == "nt":
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='Dolphin.exe'\" | "
              "ForEach-Object { \"$($_.ProcessId)`t$($_.ParentProcessId)`t$($_.CommandLine)\" }")
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True).stdout
    else:
        out = subprocess.run("ps -eo pid=,ppid=,args= | grep -i '[d]olphin' | tr -s ' ' '\\t'",
                             shell=True, capture_output=True, text=True).stdout
    found = []
    for line in out.splitlines():
        parts = line.strip().split("\t", 2)
        if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit():
            found.append((int(parts[0]), int(parts[1]), parts[2]))
    return found


def kill_tree(pid):
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)
    else:
        subprocess.run(["kill", "-9", str(pid)], capture_output=True)


def wait_for_no_dolphin(wait_minutes):
    """True once no Dolphin runs; False if one still does after the wait. Whatever
    runs is somebody else's (another worker's test): it is never killed."""
    deadline = time.monotonic() + wait_minutes * 60
    while dolphin_processes():
        if time.monotonic() > deadline:
            return False
        print("a Dolphin runs (not this script's); waiting", flush=True)
        time.sleep(20)
    return True


def run_test(name, script, args, seconds, out, wait, attempt=1):
    """(passed, seconds, note) of one run; a rerun's log is kept beside the first."""
    if not wait_for_no_dolphin(wait):
        return False, 0, "another Dolphin kept running"
    log = out / (name.replace(" ", "_").replace("--", "") + (".log" if attempt == 1 else ".rerun.log"))
    started = time.monotonic()
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.Popen([sys.executable, "-u", str(DOLPHIN_DIR / script), *args],
                                stdout=f, stderr=subprocess.STDOUT, cwd=root)
        try:
            rc = proc.wait(timeout=seconds)
            note = f"rc={rc}"
        except subprocess.TimeoutExpired:
            kill_tree(proc.pid)
            rc, note = 1, f"timed out after {seconds} s"
        # a test that died hard can leave its Dolphin: ours by parent, never another's
        for pid, ppid, _cmd in dolphin_processes():
            if ppid == proc.pid:
                print(f"killing the Dolphin this test left (pid {pid})", flush=True)
                kill_tree(pid)
    took = time.monotonic() - started
    if rc:
        tail = log.read_text(errors="replace").strip().splitlines()[-1:]
        note += f"; {tail[0][:100]}" if tail else ""
    return rc == 0, took, note


def build_special(out):
    """The fault-injection channels, built from HEAD in a temporary worktree.
    Returns (init-failure DOL, stuck-cleanup WAD) copied into `out`."""
    tree = pathlib.Path(tempfile.mkdtemp(prefix="hbc-special-"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(root), "worktree", "add", "--detach", str(tree), "HEAD"],
                   check=True, capture_output=True)
    try:
        pyarg = f"PYTHON={posix(sys.executable) if msys_bash() else sys.executable}"
        make(tree / "wiipax", [], tree)
        make(tree / "channel/channelapp", ["channel", "EXTRA_CFLAGS=-DHBC_TEST_INIT_FAILURE"], tree)
        dol = out / "init_failure-channel.dol"
        shutil.copy(tree / "channel/channelapp/channelapp-channel.dol", dol)
        for o in (tree / "channel/channelapp").rglob("loader.o"):
            o.unlink()
        make(tree / "channel/channelapp", ["channel", "EXTRA_CFLAGS=-DHBC_TEST_STUCK_NET_CLEANUP"], tree)
        make(tree / "channel", [pyarg], tree)
        wad = out / "stuck_net_cleanup-channel.wad"
        shutil.copy(tree / "channel/title/channel_retail.wad", wad)
        return dol, wad
    finally:
        subprocess.run(["git", "-C", str(root), "worktree", "remove", "--force", str(tree)],
                       capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--only", default="", help="comma-separated names (see --list)")
    ap.add_argument("--skip", default="")
    ap.add_argument("--special", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--wait", type=float, default=10, help="minutes to wait for another Dolphin")
    ap.add_argument("--out", type=pathlib.Path)
    ap.add_argument("--list", action="store_true")
    opts = ap.parse_args()
    tests = [t for t in TESTS]
    if opts.list:
        print("\n".join(t[0] for t in tests) + ("\nnetwork_retry\nnetwork_recovery\n"
              "network_recovery --reload  (with --special)"))
        return 0
    only = [s.strip() for s in opts.only.split(",") if s.strip()]
    skip = [s.strip() for s in opts.skip.split(",") if s.strip()]
    if only:
        tests = [t for t in tests if any(t[0] == o or t[0].startswith(o + " ") for o in only)]
    tests = [t for t in tests if t[0] not in skip]
    out = opts.out or pathlib.Path(tempfile.mkdtemp(prefix="hbc-dolphin-run-"))
    out.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("WII_BENCH_LOG_PORT", "4300")

    special = []
    if opts.special:
        if not opts.no_build:
            dol, wad = build_special(out)
        else:
            dol, wad = out / "init_failure-channel.dol", out / "stuck_net_cleanup-channel.wad"
        special = [("network_retry", "dolphin_network_retry.py", [str(dol)], 300),
                   ("network_recovery", "dolphin_network_recovery.py", [str(wad)], 300),
                   ("network_recovery --reload", "dolphin_network_recovery.py",
                    ["--reload", str(wad)], 300)]
        if only:
            special = [s for s in special if any(s[0] == o or s[0].startswith(o + " ") for o in only)]
    if not opts.no_build:
        build_missing(tests)

    rows, begun = [], time.monotonic()
    work = [(n, s, a, t) for n, s, a, _f, t in tests] + special
    for name, script, args, seconds in work:
        print(f"== {name}", flush=True)
        ok, took, note = run_test(name, script, args, seconds, out, opts.wait)
        first = (took, note)
        if not ok:
            print(f"   failed ({note}); running once more", flush=True)
            ok2, took2, note2 = run_test(name, script, args, seconds, out, opts.wait, 2)
            result = "FAIL then PASS" if ok2 else "FAIL x2"
            rows.append((name, result, f"{first[0]:.0f} / {took2:.0f}", note2 if not ok2 else first[1]))
            ok = ok2
        else:
            rows.append((name, "PASS", f"{took:.0f}", ""))
        print(f"   {rows[-1][1]} in {rows[-1][2]} s", flush=True)

    width = max(len(r[0]) for r in rows) if rows else 4
    print(f"\n{'name':{width}}  {'result':15} {'s':9} note")
    for name, result, secs, note in rows:
        print(f"{name:{width}}  {result:15} {secs:9} {note}")
    total = time.monotonic() - begun
    print(f"\ntotal {total:.0f} s ({total / 60:.1f} min); logs in {out}")
    return 0 if all(r[1] in ("PASS", "FAIL then PASS") for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
