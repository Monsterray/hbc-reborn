#!/usr/bin/env python3
"""Run every tests/host/test_*.py at once, one process per file: the same tests
as `python -m unittest discover -s tests/host`, in about the time of the
slowest file (about 25 s against 2 minutes one after another). Each file
already works alone (its own temporary folders and ports). Prints one line
per file, slowest first, then the output of any that failed; exits 1 if any
failed.

usage: python tests/host/run_parallel.py [-j JOBS] [NAME ...]
  NAME  only the files whose name contains it (e.g. mii, usbguard)
"""

import argparse
import concurrent.futures
import os
import pathlib
import subprocess
import sys
import time

here = pathlib.Path(__file__).resolve().parent


def run(path):
    t = time.monotonic()
    r = subprocess.run([sys.executable, "-m", "unittest", str(path)], cwd=here.parents[1],
                       capture_output=True, text=True)
    return path.name, time.monotonic() - t, r.returncode, r.stdout + r.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("names", nargs="*")
    opts = parser.parse_args()
    files = [f for f in sorted(here.glob("test_*.py"))
             if not opts.names or any(n in f.name for n in opts.names)]
    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(opts.jobs) as pool:
        results = list(pool.map(run, files))
    failed = [r for r in results if r[2]]
    for name, secs, rc, out in sorted(results, key=lambda r: -r[1]):
        last = (out.strip().splitlines() or ["?"])[-1]
        print(f"{secs:6.1f} s  {'FAIL' if rc else 'ok  '}  {name}  {last}")
    for name, secs, rc, out in failed:
        print(f"\n===== {name} =====\n{out}")
    print(f"\n{len(files)} files, {len(failed)} failed, {time.monotonic() - start:.1f} s")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
