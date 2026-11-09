"""Checks shared by tests/dolphin/dolphin_replug.py and tests/wii/wii_replug.py: what HBC
must show after a storage device is unplugged and plugged back in.

"Comes back" means: the app list reloads from the device, file requests (ls,
get, put) work on it again, HBC keeps answering, and a device that stays out
is reported gone (not in status "inserted", file requests fail) rather than
stale. Everything written goes under <device>:/replug_test/ and only that.
"""

import os
import time

import hbc  # tests/lib/testenv.py put tools/ on sys.path

TEST_DIR = "replug_test"
POLL_S = 1.0


def inserted(st):
    """The devices a status reply lists as inserted."""
    return set(st.get("inserted") or [])


def is_gone(st, dev):
    """True when a status reply shows dev gone: not inserted, not the active
    device, and not listed as unmountable."""
    return dev not in inserted(st) and st.get("device") != dev and dev not in (st.get("unmountable") or {})


def is_present(st, dev):
    return dev in inserted(st) or st.get("device") == dev


def wait_for(wii, pred, seconds, what):
    """Poll status until pred(status) holds; the last status is returned. A
    reply that does not come within the time is a failure, so a hang shows."""
    deadline, last = time.monotonic() + seconds, None
    while time.monotonic() < deadline:
        try:
            last = hbc.status(wii)
            if pred(last):
                return last
        except (OSError, hbc.HBCError) as exc:
            last = {"error": str(exc)}
        time.sleep(POLL_S)
    raise AssertionError(f"timed out after {seconds} s waiting for {what}; last status: "
                         f"{ {k: last.get(k) for k in ('device', 'inserted', 'apps', 'unmountable', 'error')} if last else None}")


def alive(wii, expected_version=None):
    """HBC answers HBCV (it did not hang or crash), with its version if given."""
    version = hbc.version(wii)
    if expected_version is not None:
        assert version == expected_version, f"version {version!r}, expected {expected_version!r}"
    return version


def listing(wii, path):
    text = hbc.file_request(wii, "L", path).decode()
    return [hbc.parse_entry(line) for line in text.splitlines()[1:]]


def roundtrip(wii, dev, tag):
    """mkdir, put, ls, get and delete one small file under dev:/replug_test/."""
    folder = f"{dev}:/{TEST_DIR}"
    path = f"{folder}/{tag}.bin"
    data = os.urandom(2000) + tag.encode()
    hbc.file_request(wii, "M", folder)
    hbc.put_file(wii, path, data)
    names = [e["name"] for e in listing(wii, folder)]
    assert f"{tag}.bin" in names, (path, names)
    assert hbc.get_file(wii, path) == data, f"{path}: read back differs"
    hbc.file_request(wii, "D", path)
    assert f"{tag}.bin" not in [e["name"] for e in listing(wii, folder)], f"{path} not deleted"


def check_present(wii, dev, tag, seconds=60):
    """dev is back: status lists it, ls of its root works, and a file makes the
    round trip. For the app-list device the apps count is returned."""
    wait_for(wii, lambda st: is_present(st, dev), seconds, f"{dev} to show as inserted")
    deadline = time.monotonic() + seconds
    while True:
        try:
            listing(wii, f"{dev}:/")
            break
        except (OSError, hbc.HBCError) as exc:
            assert time.monotonic() < deadline, f"ls {dev}:/ never worked again: {exc}"
            time.sleep(POLL_S)
    roundtrip(wii, dev, tag)


def check_gone(wii, dev, seconds=30):
    """dev stays out: status stops listing it, and file requests to it fail
    (a stale mount would still answer ls from its cache)."""
    wait_for(wii, lambda st: is_gone(st, dev), seconds, f"{dev} to show as gone")
    for op, path in (("L", f"{dev}:/"), ("G", f"{dev}:/{TEST_DIR}/none.bin")):
        try:
            hbc.file_request(wii, op, path)
        except hbc.HBCError:
            continue
        raise AssertionError(f"file request {op} {path} succeeded on a removed device")
    # The failed requests must not wedge HBC or bring the device back.
    alive(wii)
    st = hbc.status(wii)
    assert is_gone(st, dev), f"{dev} reappeared after file requests: {st.get('inserted')}"
    return st
