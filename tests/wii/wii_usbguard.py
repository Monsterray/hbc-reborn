#!/usr/bin/env python3
"""sdk/usbguard on a Wii with a really stuck USB drive. ATTENDED: someone at
the Wii pulls and replugs the drive and presses buttons; d2x's usb-spin hangs
IOS on purpose. Queue it only through the bench (wiibench.py add --wii dev).

Each round: d2xdiag's usb-spin (d2x +dbg as IOS 250, its watchdog resets the
Wii mid-transfer and leaves the drive stuck) unless HBC is already in a stuck
start; then HBC's start-up status: the library's probe on its worker,
usbguard_free, the replug check. Then the person launches "usbguard test"
(tests/apps/usbguard_app with meta.xml, put on the SD card here) from the
menu with the drive still in:
  round 1: on the warning, pull the drive and plug it back in: the call
           waits for the replug (REPLUG) and the mount then works;
  round 2: on the warning, A on a GameCube controller: usbguard_free, FREED.
The app's report is fetched after each round. Every wait has a deadline.

usage: tests/wii/wii_usbguard.py --d2x D2X_CIOS_DIR [--install WAD]
  --d2x      the d2x-cios checkout with tests/wii/d2xdiag.py (else $D2X_CIOS)
  --install  first install this HBC WAD through tests/apps/wadinstall on IOS 249"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import time

WT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(WT / "tools"))
import hbc as H  # noqa: E402

a = argparse.ArgumentParser(description=__doc__.split("\n")[0])
a.add_argument("--d2x", default=os.environ.get("D2X_CIOS"))
a.add_argument("--install", metavar="WAD")
a.add_argument("--out", default="usbguard-run", help="where the reports go")
ARGS = a.parse_args()
if not ARGS.d2x:
    sys.exit("give --d2x DIR (the d2x-cios checkout) or set D2X_CIOS")
D2X = pathlib.Path(ARGS.d2x)
OUT = pathlib.Path(ARGS.out)
IP = os.environ.get("WII_BENCH_IP") or sys.exit("no WII_BENCH_IP: run it through the bench queue")
VERSION = (WT / "channel/title/Makefile").read_text().split("\n")[0].split("=")[1].strip()
WAD_SD = f"sd:/wad/hbc-reborn-{VERSION}-OHBC.wad"
ok = True


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def check(cond, what):
    global ok
    log(("PASS: " if cond else "FAIL: ") + what)
    ok = ok and bool(cond)
    return cond


def hbc(*args, timeout=180):
    r = subprocess.run([sys.executable, str(WT / "tools/hbc.py"), "--wii", IP, *args],
                       capture_output=True, text=True, timeout=timeout)
    out = (r.stdout + r.stderr).strip()
    log(f"hbc.py {' '.join(args)} -> {r.returncode}" + (f"\n{out[-1500:]}" if out else ""))
    return r


def version():
    try:
        return H.version(IP)
    except Exception:
        return None


def wait_hbc(seconds, what):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        v = version()
        if v and not v.endswith(" agent"):
            log(f"HBC answers: {v} ({what})")
            return v
        time.sleep(5)
    log(f"HBC did not answer in {seconds} s ({what})")
    return None


def usb_status(what):
    try:
        st = H.status(IP)
    except Exception as exc:
        log(f"status failed: {exc}")
        return {}
    keys = ("version", "usb_stuck", "usb_unplugged", "replug_needed", "remotes_off", "ehci")
    log(f"status {what}: " + ", ".join(f"{k}={st.get(k)}" for k in keys))
    calls = (st.get("calls") or {}).get("list", [])
    for c in calls:
        if "usb" in c.get("call", "") or "ehci" in c.get("call", "") or c.get("thread") == "usbguard":
            log(f"  call {c}")
    return st


def wait_report(seconds, path):
    """The app writes its report at the end; HBC answers again after it exits."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if version() and not version().endswith(" agent"):
            r = subprocess.run([sys.executable, str(WT / "tools/hbc.py"), "--wii", IP, "stat",
                                "sd:/usbguard_test/result.txt"], capture_output=True, text=True,
                               timeout=60)
            if r.returncode == 0:
                hbc("get", "sd:/usbguard_test/result.txt", str(path))
                hbc("rm", "sd:/usbguard_test/result.txt")
                return path.read_text(errors="replace") if path.exists() else ""
        time.sleep(10)
    return ""


def install():
    log("== install HBC " + VERSION)
    hbc("rm", "sd:/wadinstall/result.txt")
    hbc("put", ARGS.install, WAD_SD, timeout=300)
    # HBC refuses a sent app while a drive that was stuck at its start is in.
    end = time.monotonic() + 600
    while True:
        r = hbc("send", "--no-wait", str(WT / "tests/apps/wadinstall/wadinstall.dol"), WAD_SD, "249")
        if r.returncode == 0:
            break
        if time.monotonic() > end:
            return check(False, "wadinstall sent")
        if "usb_stuck" in r.stdout + r.stderr:
            log(">>> At the Wii: HBC has a stuck USB drive from before: replug it, then press Reset")
        time.sleep(15)
        wait_hbc(600, "before sending wadinstall")
    end = time.monotonic() + 300
    text = ""
    while time.monotonic() < end:
        time.sleep(10)
        v = version()
        if not v or v.endswith(" agent"):
            continue
        res = OUT / "wadinstall.txt"
        r = hbc("get", "sd:/wadinstall/result.txt", str(res))
        if r.returncode == 0 and res.exists():
            text = res.read_text(errors="replace")
            if text.strip().endswith(("done", "failed")):
                break
    log("wadinstall result:\n" + text[-800:])
    if not check(text.strip().endswith("done"), "wadinstall installed the WAD"):
        return False
    v = wait_hbc(120, "after the install")
    if v and not v.startswith(VERSION):
        log("the running HBC is the old one; sending the new DOL to reload into the installed title")
        hbc("send", "--no-wait", str(WT / "channel/channelapp/channelapp-channel.dol"))
        time.sleep(20)
        v = wait_hbc(120, "after sending HBC")
    return check(v and v.startswith(VERSION), f"HBC {VERSION} runs ({v})")


def spin():
    log("== d2xdiag usb-spin: the watchdog resets the Wii in about 30 s; press through BootMii if it waits")
    try:
        r = subprocess.run([sys.executable, "tests/wii/d2xdiag.py", "--attended", "--timeout", "900",
                            "--", "--ios=250", "--wd=30", "--scenario=usb-spin"],
                           cwd=D2X, capture_output=True, text=True, timeout=1300)
        log(f"d2xdiag.py exit {r.returncode}\n{(r.stdout + r.stderr)[-2000:]}")
    except subprocess.TimeoutExpired:
        log("d2xdiag.py timed out")


def round_(n, how):
    log(f"=== round {n}: {how}")
    pre = usb_status(f"round {n}, before")
    if pre.get("replug_needed") is True:
        log("HBC is already in a stuck start (drive in, cut off): no usb-spin needed")
    else:
        spin()
    if not wait_hbc(900, f"round {n}, after the reset"):
        return check(False, f"round {n}: HBC back after the reset")
    st = usb_status(f"round {n}, start-up")
    check(st.get("usb_stuck") is True, f"round {n}: HBC saw USB stuck at start-up")
    check(st.get("usb_unplugged") is True, f"round {n}: usbguard_free freed HBC's start-up")
    check(any(c.get("thread") == "usbguard" for c in (st.get("calls") or {}).get("list", [])),
          f"round {n}: HBC's probes ran on usbguard's worker")
    log(f">>> At the Wii: launch 'usbguard test' from the HBC menu, drive still in; "
        f"press A at HBC's warning. Then: {how}")
    text = wait_report(1800, OUT / f"result-round{n}.txt")
    log(f"round {n} report:\n{text or '(none)'}")
    if not check(bool(text) and "done" in text.split("\n"), f"round {n}: the app finished and wrote its report"):
        return
    m = re.search(r"phase STUCK at (\d+) ms", text)
    check(m and int(m.group(1)) <= 5600, f"round {n}: the warning came within 5 s ({m and m.group(1)} ms)")
    if n == 1:
        check("phase REPLUG" in text, "round 1: the pull was seen (REPLUG)")
        check(re.search(r"^call IOS_Open /dev/usb/ven: \d+ in", text, re.M) or
              re.search(r"^mount usb: 0 in", text, re.M), "round 1: a call succeeded on the retry")
    else:
        check(re.search(r"^call IOS_Open /dev/usb/ven: FREED in", text, re.M), "round 2: usbguard_free freed the call (FREED)")
    usb_status(f"round {n}, after the app")


def main():
    OUT.mkdir(exist_ok=True)
    log(f"Wii {IP}, running {version()}")
    for p in ("boot.dol", "meta.xml"):
        src = WT / "tests/apps/usbguard_app" / ("usbguard_app.dol" if p == "boot.dol" else p)
        hbc("put", str(src), f"sd:/apps/usbguard_test/{p}")
    hbc("rm", "sd:/usbguard_test/result.txt")
    if not ARGS.install:
        check((version() or "").startswith(VERSION), f"HBC {VERSION} runs")
    elif not install():
        return 1
    usb_status("healthy, before the rounds")
    round_(1, "pull the drive on the warning, plug it back in when it says so")
    round_(2, "press A on the GameCube controller on the warning; after the run, pull and replug the drive")
    log("=== " + ("ALL PASS" if ok else "SOME FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
