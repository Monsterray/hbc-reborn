#!/usr/bin/env python3
"""HOME > HBC > Save Mii faces on a Wii, as a queue job: this tree's HBC
(channel DOL, through the installed HBC) copies the Wii Menu's RFL_Res.dat
to sd:/mii/RFL_Res.dat (`hbc.py mii-faces`, the button's request), twice;
the copy is fetched to the job's run folder, checked with tools/mii_res.py
and dumped there (OBJ, PNG). Then tests/mii_res_probe shows IOS's NAND
permission check is back: an app is refused /shared1 again.

The file is Nintendo's: it stays in the run folder and on the Wii's SD card,
never in the repository.

queue: wiibench.py add --wii dev --name mii-faces --cwd REPO --timeout 900 -- <python> tests/wii_mii_faces.py
"""

import hashlib
import os
import pathlib
import shutil
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import dolphin_smoke  # noqa: E402
import hbc  # noqa: E402
import mii_res  # noqa: E402
import wii_mboard as wm  # noqa: E402

PROBE = root / "tests" / "mii_res_probe" / "mii_res_probe.dol"


def main():
    wii = wm.bench_wii()
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    if not PROBE.exists():
        raise SystemExit(f"build {PROBE.relative_to(root)} first")
    snap = pathlib.Path(tempfile.mkdtemp(prefix="hbc-mii-")) / "hbc-mii.dol"
    shutil.copy(root / "channel/channelapp/channelapp-channel.dol", snap)
    failed = []
    try:
        hbc.send(wii, str(snap), [])
        hbc.relaunch_wait(wii, dolphin_smoke.expected)
        time.sleep(3)
        for n in (1, 2):
            t0 = time.monotonic()
            ok, text = hbc.mii_faces(wii)
            print(f"save {n}: {'ok' if ok else 'FAILED'} in {time.monotonic() - t0:.1f} s: {text}", flush=True)
            if not ok:
                failed.append(f"save {n}: {text}")
        if not failed:
            dest = out / "RFL_Res.dat"
            dest.write_bytes(hbc.get_file(wii, "sd:/mii/RFL_Res.dat"))
            data = dest.read_bytes()
            print(f"RFL_Res.dat: {len(data)} bytes, sha1 {hashlib.sha1(data).hexdigest()}", flush=True)
            try:
                mii_res.info(data)
                mii_res.dump(data, out / "dump")
            except (mii_res.ResError, ValueError) as exc:
                failed.append(f"the copy does not parse: {exc}")
    finally:
        # The probe, from this HBC: ends in the installed one through the stub.
        wm.launch(PROBE)
        shutil.rmtree(snap.parent, ignore_errors=True)
    wm.tool("get", "sd:/mii_res/result.txt", str(out / "probe_after.txt"))
    probe = (out / "probe_after.txt").read_text(errors="replace")
    print(probe, flush=True)
    if "/shared1/content.map: open -102" not in probe:
        failed.append("after the save, an app still reads /shared1: the NAND check did not come back")
    for f in failed:
        print("FAIL", f)
    if failed:
        raise SystemExit(1)
    print("PASS")


if __name__ == "__main__":
    main()
