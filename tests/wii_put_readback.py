#!/usr/bin/env python3
"""hbc.py put and sync reading each upload back (PR 2), on a Wii, as a
queue job, against the HBC that is running (nothing is sent to it):

  put           8 MB: one checksum after the upload, "read back" in the line
  put -r        a small tree: each file read back
  sync          a changed file: uploaded and read back
  --no-verify   the same put: no read
  a bad read    the first read back made wrong: the file is uploaded again

sd:/apps/hbctest-readback is removed after.

queue: wiibench.py add --wii dev --name put-readback --cwd REPO --timeout 600 -- <python> tests/wii_put_readback.py
"""

import os
import pathlib
import shutil
import sys
import tempfile
import time
from unittest import mock

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402
import wii_mboard as wm  # noqa: E402

FOLDER = "sd:/apps/hbctest-readback"


def main():
    wii = wm.bench_wii()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="hbc-readback-"))
    failed = []

    def check(ok, what):
        print(("PASS " if ok else "FAIL ") + what, flush=True)
        if not ok:
            failed.append(what)

    real = hbc.checksum
    calls = []

    def counted(w, remote, size=0):
        calls.append(remote)
        return real(w, remote, size)

    try:
        print(f"HBC {hbc.version(wii)}", flush=True)
        big = tmp / "big.bin"
        big.write_bytes(os.urandom(8 << 20))
        lines = []
        with mock.patch.object(hbc, "checksum", counted):
            t0 = time.monotonic()
            hbc.upload(wii, str(big), f"{FOLDER}/big.bin", out=lines.append)
            took = time.monotonic() - t0
        check(calls == [f"{FOLDER}/big.bin"] and "read back" in lines[-1],
              f"put 8 MB, read back once in {took:.1f} s ({lines[-1]})")

        tree = tmp / "tree"
        (tree / "sub").mkdir(parents=True)
        (tree / "a.txt").write_bytes(b"a" * 100)
        (tree / "sub" / "b.txt").write_bytes(b"b" * 200)
        calls.clear()
        with mock.patch.object(hbc, "checksum", counted):
            hbc.put_tree(wii, str(tree), f"{FOLDER}/tree", out=lambda _: None)
        check(sorted(calls) == [f"{FOLDER}/tree/a.txt", f"{FOLDER}/tree/sub/b.txt"],
              f"put -r: each file read back ({calls})")

        (tree / "a.txt").write_bytes(b"A" * 100)   # same size: the CRC decides
        calls.clear()
        with mock.patch.object(hbc, "checksum", counted):
            got = hbc.sync(wii, str(tree), f"{FOLDER}/tree", out=lambda _: None)
        check(got[0] == 1 and calls.count(f"{FOLDER}/tree/a.txt") == 2,
              f"sync: a.txt compared, uploaded and read back ({got}, {calls})")
        check(hbc.get_file(wii, f"{FOLDER}/tree/a.txt") == b"A" * 100, "sync: a.txt on the Wii is new")

        calls.clear()
        with mock.patch.object(hbc, "checksum", counted):
            hbc.upload(wii, str(big), f"{FOLDER}/big.bin", out=lambda _: None, verify=False)
        check(calls == [], "--no-verify: no read")

        bad = []

        def wrong_once(w, remote, size=0):
            got = real(w, remote, size)
            bad.append(got)
            return (got[0], got[1] ^ 1) if len(bad) == 1 else got

        lines = []
        with mock.patch.object(hbc, "checksum", wrong_once):
            hbc.upload(wii, str(tree / "a.txt"), f"{FOLDER}/again.txt", out=lines.append)
        check(len(bad) == 2 and any("uploading again" in l for l in lines),
              f"a bad read back uploads again ({lines})")
    except (OSError, hbc.HBCError) as exc:
        check(False, f"error: {exc!r}")
    finally:
        try:
            hbc.remove_tree(wii, FOLDER, force=True)
            print("test folder removed", flush=True)
        except (OSError, hbc.HBCError) as exc:
            check(False, f"removing {FOLDER}: {exc!r}")
        shutil.rmtree(tmp, ignore_errors=True)
    if failed:
        raise SystemExit(f"failed: {len(failed)}")
    print("PASS")


if __name__ == "__main__":
    main()
