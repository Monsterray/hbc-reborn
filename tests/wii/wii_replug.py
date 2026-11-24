#!/usr/bin/env python3
"""ATTENDED test on the dev Wii: pull and replug the SD card and the USB drive
while HBC runs. A person stands at the Wii; this script tells them what to do
(printed in the job's log, with a beep) and waits for the change to show.

Needs both an SD card and a USB drive in the dev Wii at the start, with HBC
running and its app list on screen (not an app's dialog or the Options page:
the list reloads only on the app list).

Steps (each waits up to 3 minutes for the person, polling HBC):
  1  pull the SD card         2  plug the SD card in
  3  pull the USB drive       4  plug the USB drive in
  5  pull the SD card, then the USB drive          (both out)
  6  plug the SD card in, then the USB drive       (both in)
  7  pull the USB drive, then the SD card          (both out)
  8  plug the USB drive in, then the SD card       (both in, the other order)

After the person's action shows in HBC (status "inserted" and a failing or
working `ls`), the script waits 3 s and checks, for every step:
  - HBC answers HBCV all along, never silent for 45 s (no hang, no crash)
  - a device that is out: ls and get of it fail, and status no longer lists it
    (not stale)
  - a device that is in: ls of its root works, and a small file makes the
    round trip (put, ls, get, delete) under <dev>:/replug_test/
  - the app list: status "device" is one that is in (none when both are out),
    and the apps count is that device's again (the count first seen for it)
Each step's TV picture is saved if the capture card tool is there. Only
<dev>:/replug_test/<tag>.bin files are written, and only those are deleted.

Run it only as a bench job on the dev Wii, with its owner at the Wii (the
queue holds the Wii until it ends); it refuses to run outside a bench job
and on a production Wii. From the repository:

  python "$(python tools/wii_paths.py wii_bench)" add --wii devwii \\
      --name "replug attended" --cwd . -- python tests/wii/wii_replug.py

Watch the prompts with `wiibench.py wait ID`. Exit code 0 only if every
step passed. The Wii is left in HBC.
"""

import os
import pathlib
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import replug_checks as rc  # noqa: E402
import wii_paths  # noqa: E402
hbc = rc.hbc

PRODUCTION = {"192.168.8.200", "192.168.8.243"}  # real data: never run here
USER_S = 180          # time for the person to act
SILENT_S = 45         # HBC unanswered this long is a hang

STEPS = [
    ("1 SD out", "Pull the SD card now", {"sd": False, "usb": True}),
    ("2 SD in", "Plug the SD card back in now", {"sd": True, "usb": True}),
    ("3 USB out", "Pull the USB drive now", {"sd": True, "usb": False}),
    ("4 USB in", "Plug the USB drive back in now", {"sd": True, "usb": True}),
    ("5 both out", "Pull the SD card now, then the USB drive", {"sd": False, "usb": False}),
    ("6 both in", "Plug the SD card in now, then the USB drive", {"sd": True, "usb": True}),
    ("7 both out", "Pull the USB drive now, then the SD card", {"sd": False, "usb": False}),
    ("8 both in", "Plug the USB drive in now, then the SD card", {"sd": True, "usb": True}),
]

last_ok = time.monotonic()
apps_seen = {}      # device -> its app count when it was the app list's device


def beep():
    try:
        import winsound
        winsound.Beep(1000, 400)
    except Exception:
        print("\a", end="", flush=True)


def said(wii, call, *args):
    """call(wii, ...) with the silence clock: HBC not answering for SILENT_S
    since its last answer is a hang."""
    global last_ok
    try:
        result = call(wii, *args)
    except (OSError, hbc.HBCError):
        if time.monotonic() - last_ok > SILENT_S:
            raise AssertionError(f"HBC has not answered for {SILENT_S} s: hung or crashed")
        raise
    last_ok = time.monotonic()
    return result


def ls_works(wii, dev):
    try:
        said(wii, lambda w: rc.listing(w, f"{dev}:/"))
        return True
    except (OSError, hbc.HBCError):
        return False


def wait_state(wii, want, label):
    """Poll until every device is as wanted: in means ls of its root works, out
    means ls fails and status does not list it."""
    deadline = time.monotonic() + USER_S
    seen, st = {}, {}
    while time.monotonic() < deadline:
        seen = {d: ls_works(wii, d) for d in want}
        try:
            st = said(wii, hbc.status)
        except (OSError, hbc.HBCError):
            time.sleep(1)
            continue
        if all(seen[d] == want[d] and (rc.is_present(st, d) if want[d] else rc.is_gone(st, d))
               for d in want):
            return st
        time.sleep(1)
    raise AssertionError(f"{label}: after {USER_S} s the state is not as asked: ls works {seen}, "
                         f"status inserted {sorted(rc.inserted(st))}, device {st.get('device')}")


def picture(out, name):
    """The TV through the capture card, when this PC has one ($WIICAP or
    ~/.wii-paths.json)."""
    try:
        capture = str(wii_paths.wiicap())
    except wii_paths.NotFound:
        return
    try:
        subprocess.run([sys.executable, capture, "still", "--preset", "wii", "--out",
                        str(out / f"{name.replace(' ', '_')}.png")], timeout=60, check=False,
                       capture_output=True)
    except (OSError, subprocess.SubprocessError):
        pass


def check_step(wii, name, want, out):
    time.sleep(3)
    rc.alive(wii)
    st = hbc.status(wii)
    for dev, inn in want.items():
        if inn:
            rc.check_present(wii, dev, name.split()[0] + dev)
        else:
            rc.check_gone(wii, dev)
    # the app list: on one of the devices that are in, and that device's own list
    present = [d for d, inn in want.items() if inn]
    st = rc.wait_for(wii, lambda s: (s.get("device") in present) if present
                     else s.get("device") is None, 60, "the app list's device")
    if present:
        dev = st["device"]
        st = rc.wait_for(wii, lambda s: s.get("device") == dev, 30,
                         "a steady app list")
        if dev in apps_seen:
            assert st["apps"] == apps_seen[dev], \
                f"{dev}: app list has {st['apps']} apps, it had {apps_seen[dev]} (not reloaded?)"
        else:
            apps_seen[dev] = st["apps"]
        detail = f"app list from {dev}: {st['apps']} apps"
    else:
        assert st.get("apps") == 0, f"both out but the app list still has {st.get('apps')} apps"
        detail = "app list empty"
    rc.alive(wii)
    print(f"  checked: {detail}; HBC answers", flush=True)
    picture(out, name)


def main():
    ip = os.environ.get("WII_BENCH_IP")
    if not ip or not os.environ.get("WII_BENCH_JOB"):
        raise SystemExit("refusing: this attended test runs only as a bench job "
                         "(wiibench.py add --wii devwii ...)")
    if ip in PRODUCTION:
        raise SystemExit(f"refusing: {ip} is a production Wii; this test pulls its drives")
    wii = ip
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT") or ".")
    out.mkdir(parents=True, exist_ok=True)

    st = hbc.status(wii)
    start = {d: ls_works(wii, d) for d in ("sd", "usb")}
    if not all(start.values()):
        raise SystemExit(f"start with both the SD card and the USB drive in: ls works {start}")
    apps_seen[st["device"]] = st["apps"]
    print(f"start: HBC {st['version']}, app list from {st['device']} ({st['apps']} apps), "
          f"inserted {sorted(rc.inserted(st))}", flush=True)
    for dev in ("sd", "usb"):
        rc.roundtrip(wii, dev, "start")
    picture(out, "0 start")

    failed = []
    for name, prompt, want in STEPS:
        beep()
        print(f"\n=== {name}: >>> {prompt.upper()} <<<  (up to {USER_S} s)", flush=True)
        try:
            wait_state(wii, want, name)
            print(f"  change seen: {want}", flush=True)
            check_step(wii, name, want, out)
            print(f"PASS {name}", flush=True)
        except AssertionError as exc:
            failed.append(name)
            print(f"FAIL {name}: {exc}", flush=True)
            picture(out, name + " FAIL")
            # keep going only if the next step can still be asked of the person
            try:
                rc.alive(wii)
            except (OSError, hbc.HBCError):
                print("HBC does not answer: stopping", flush=True)
                break
    beep()
    print("\nleave both devices plugged in." if not failed else f"\nFAILED: {failed}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
