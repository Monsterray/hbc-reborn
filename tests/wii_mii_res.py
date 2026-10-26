#!/usr/bin/env python3
"""Where the bench Wii keeps its Mii face resource (RFL_Res.dat), and whether
an app reaches it through ES without NAND rights: tests/mii_res_probe, as a
queue job. The probe's report and the file go to the job's run folder only:
the file is Nintendo's, for the developer's own tools, never the repository.

queue: wiibench.py add --wii dev --name mii-res --cwd REPO --timeout 600 -- <python> tests/wii_mii_res.py
"""

import hashlib
import os
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import wii_mboard as wm  # noqa: E402

PROBE = root / "tests" / "mii_res_probe" / "mii_res_probe.dol"


def main():
    wm.bench_wii()
    if not PROBE.exists():
        raise SystemExit(f"build {PROBE.relative_to(root)} first")
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT", "."))
    wm.launch(PROBE)
    wm.tool("get", "sd:/mii_res/result.txt", str(out / "result.txt"))
    report = (out / "result.txt").read_text(errors="replace")
    print(report, flush=True)
    if "saved sd:/mii_res/RFL_Res.dat" in report:
        dest = out / "RFL_Res.dat"
        wm.tool("get", "sd:/mii_res/RFL_Res.dat", str(dest))
        data = dest.read_bytes()
        print(f"RFL_Res.dat: {len(data)} bytes, sha1 {hashlib.sha1(data).hexdigest()}, "
              f"in {dest}", flush=True)
    if not report.rstrip().endswith("done"):
        raise SystemExit("the probe did not finish")


if __name__ == "__main__":
    main()
