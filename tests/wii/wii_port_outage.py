#!/usr/bin/env python3
"""Does HBC's port 4299 stop answering for minutes? (docs/research/ios-tcp.md)

On a real Wii, IOS's TCP stack used to stop answering SYNs and FINs on the
port for 108-190 s while ping worked and HBC kept running. Two loads that
reproduced it, as a queue job; the Wii keeps this tree's HBC.

  zip     send a ZIP without --yes and poll HBCS until HBC's "Extract the
          received ZIP file?" question times out ("declined"), about 12 s a
          cycle; 1 outage in 5-8 cycles before the fix.
  hammer  HBCV every GAP seconds with no popup; an outage within a minute
          before the fix.

An outage is a request that fails and is still refused for 8 s or more; a
request that times out and succeeds at once is a blip (counted apart). Exit
status 1 when there was an outage.

usage: wii_port_outage.py zip [--cycles N] [--cap S] [--fast] [--stop-after N]
       wii_port_outage.py hammer [--gap S] [--cap S]
queue: wiibench.py add --wii dev --name port-outage --timeout 1200 --cwd REPO -- <python> -u tests/wii/wii_port_outage.py zip --cycles 60 --cap 720
"""

import argparse
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import wii_checks  # noqa: E402
import hbc  # noqa: E402

T0 = time.monotonic()
OUTAGE_S = 8     # still refused this long after a failure: an outage


def log(msg):
    print(f"{time.monotonic() - T0:7.1f} {msg}", flush=True)


def pc_states(wii):
    """The PC's TCP states for connections to the Wii's port 4299 (netstat),
    e.g. {'TIME_WAIT': 30}: the PC holds TIME_WAIT when it closed first, and
    FIN_WAIT_2 when the Wii never sent its FIN."""
    try:
        out = subprocess.run(["netstat", "-n", "-p", "tcp"], capture_output=True,
                             text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    states = {}
    for line in out.splitlines():
        cols = line.split()
        if len(cols) >= 4 and cols[2] == f"{wii}:{hbc.PORT}":
            states[cols[3]] = states.get(cols[3], 0) + 1
    return states


def stat_line(wii):
    try:
        st = hbc.status(wii)
    except Exception as e:  # noqa: BLE001
        return f"status failed {e!r}"
    return (f"net_longest={st.get('net_longest')} accepts={st.get('accepts')} "
            f"net_starts={st.get('net_starts')} uptime={st.get('uptime_ms')} pc={pc_states(wii)}")


def other_flow(wii):
    """An HBCV request from WSL (NAT: the same PC address, another source
    port), in an outage: 'answered' means the stack still serves other flows
    and only this one is stuck; 'silent' means the whole port is."""
    cmd = (f"exec 3<>/dev/tcp/{wii}/{hbc.PORT} || exit 1; printf 'HBCV\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0' >&3; "
           "timeout 3 head -c 4 <&3")
    try:
        r = subprocess.run(["wsl.exe", "-e", "timeout", "6", "bash", "-c", cmd],
                           capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        return f"wsl failed {e!r}"
    return "answered" if r.stdout.strip() else f"silent (rc {r.returncode})"


def wait_back(wii, probe, limit=600):
    """Seconds until probe() works again (limit if it never does). Every 25 s
    of outage it also tries a flow from another source port."""
    start = time.monotonic()
    last = start
    while time.monotonic() - start < limit:
        try:
            probe()
            break
        except Exception:  # noqa: BLE001
            time.sleep(1)
        if time.monotonic() - last > 25:
            last = time.monotonic()
            log(f"  {last - start:.0f}s into the outage: WSL flow {other_flow(wii)}; pc={pc_states(wii)}")
    return time.monotonic() - start


def run_zip(wii, args):
    work = pathlib.Path(tempfile.mkdtemp(prefix="hbc-outage-"))
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.write(root / "tests/apps/agent_app/agent_app.dol", "hbcpopup/boot.dol")
        z.writestr("hbcpopup/meta.xml",
                   '<?xml version="1.0"?>\n<app version="1"><name>Popup test</name></app>\n')
    zpath = work / "hbcpopup.zip"
    zpath.write_bytes(buf.getvalue())
    if args.fast:
        hbc.POLL_TIMEOUT = 2
        hbc.UPLOAD_POLL_S = 0.2
    outages = done = 0
    try:
        for i in range(1, args.cycles + 1):
            if time.monotonic() - T0 > args.cap:
                log("cap reached")
                break
            try:
                before = hbc.upload_seq(wii)
                if before is None:
                    raise hbc.HBCError("no upload_seq")
                start = time.monotonic()
                hbc.send(wii, str(zpath), [], yes=False)
                up = hbc.upload_outcome(wii, before, 60)
                log(f"cycle {i}: {up.get('result')} in {time.monotonic() - start:.1f}s")
                if up.get("result") != "declined":
                    log(f"UNEXPECTED {up}")
                done += 1
            except Exception as e:  # noqa: BLE001
                log(f"cycle {i}: FAILED {e!r}")
                back = wait_back(wii, lambda: hbc.status(wii))
                if back > OUTAGE_S:
                    outages += 1
                    log(f"OUTAGE {outages}: port back after {back:.0f}s; {stat_line(wii)}")
                else:
                    log(f"blip: back after {back:.0f}s")
                if args.stop_after and outages >= args.stop_after:
                    break
            if i % 5 == 0:
                log(f"STAT {stat_line(wii)}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    log(f"FINAL {stat_line(wii)}")
    log(f"DONE cycles={done} outages={outages}")
    return outages


def run_hammer(wii, args):
    outages = blips = n = total = 0
    while time.monotonic() - T0 < args.cap:
        try:
            hbc.version(wii, args.timeout)
            n += 1
            total += 1
            if n % 100 == 0:
                log(f"{n} requests ok; pc={pc_states(wii)}")
        except Exception as e:  # noqa: BLE001
            log(f"FAIL after {n} ok requests: {e!r}; pc={pc_states(wii)}")
            back = wait_back(wii, lambda: hbc.version(wii, args.timeout), 400)
            if back > OUTAGE_S:
                outages += 1
                log(f"OUTAGE {outages}: back after {back:.0f}s")
            else:
                blips += 1
                log(f"blip: back after {back:.0f}s")
            n = 0
        time.sleep(args.gap)
    log(f"FINAL {stat_line(wii)}")
    log(f"DONE requests={total} outages={outages} blips={blips}")
    return outages


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dol", default=str(root / "channel/channelapp/channelapp-channel.dol"),
                    help="the HBC to send first (default: this tree's build); to compare builds")
    ap.add_argument("--expected", help="the build tag --dol reports (x.y.z+N.gHASH[.dirty])")
    sub = ap.add_subparsers(dest="mode", required=True)
    z = sub.add_parser("zip")
    z.add_argument("--cycles", type=int, default=60)
    z.add_argument("--cap", type=float, default=720)
    z.add_argument("--fast", action="store_true",
                   help="poll HBCS every 0.2 s with 2 s timeouts, as hbc.py did before 1.16.1")
    z.add_argument("--stop-after", type=int, default=0)
    h = sub.add_parser("hammer")
    h.add_argument("--gap", type=float, default=0.05)
    h.add_argument("--cap", type=float, default=300)
    h.add_argument("--timeout", type=float, default=3,
                   help="seconds a request waits before the PC gives up on it")
    args = ap.parse_args()

    wii = os.environ.get("WII_BENCH_IP")
    if not wii or not os.environ.get("WII_BENCH_JOB_START"):
        sys.exit("run this as a wii-bench queue job")
    wii_checks.refuse_production(wii, "it sends the Wii an HBC and loops for minutes")
    wii_checks.ensure_hbc(wii, args.dol, expected=args.expected, fresh=True)
    log(f"wii {wii} HBC {hbc.version(wii)}")
    outages = run_zip(wii, args) if args.mode == "zip" else run_hammer(wii, args)
    sys.exit(1 if outages else 0)


if __name__ == "__main__":
    main()
