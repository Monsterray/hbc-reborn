#!/usr/bin/env python3
"""What a Wii's DNS answers (tests/dnscheck), as a bench queue job.

Sends tests/dnscheck/dnscheck.dol (build it first: make -C tests/dnscheck)
to the job's Wii, waits for HBC to come back, and prints the app's report:
the Wii's address and what each name resolved to from a fresh network
start. Read-only: the app writes only sd:/dnscheck/result.txt. With no
names it checks cfh.wapp.wii.com (the WiiConnect24 user agreement, which
WiiLink answers with 167.235.229.36), nwcs.wapp.wii.com, rc24.xyz and
pi.hole (answered only by a Pi-hole).

usage: tests/wii_dns.py [NAME...]
queue: wiibench.py add --wii lrwii --name dns --cwd REPO -- python tests/wii_dns.py
"""

import os
import pathlib
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

DOL = root / "tests" / "dnscheck" / "dnscheck.dol"
WIILINK = "167.235.229.36"


def main():
    wii = hbc.wii_address(None)
    print(f"Wii {os.environ.get('WII_BENCH_WII') or wii}: HBC {hbc.version(wii)}", flush=True)
    hbc.send(wii, str(DOL), sys.argv[1:])
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:     # the app takes HBC's place while it runs
        try:
            hbc.version(wii, hbc.POLL_TIMEOUT)
        except (OSError, hbc.HBCError):
            break
        time.sleep(0.3)
    hbc.hbc_wait(wii, 120)
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT") or tempfile.mkdtemp()) / "dns_result.txt"
    out.write_bytes(hbc.get_file(wii, "sd:/dnscheck/result.txt"))
    report = out.read_text(errors="replace")
    print(report, flush=True)
    if "done" not in report:
        sys.exit("the app did not finish")
    if not sys.argv[1:]:
        agreement = [l for l in report.splitlines() if l.startswith("cfh.wapp.wii.com")]
        ok = bool(agreement) and agreement[0].endswith(WIILINK)
        print(("PASS" if ok else "FAIL") + f": the user agreement's server is WiiLink's ({WIILINK})",
              flush=True)
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
