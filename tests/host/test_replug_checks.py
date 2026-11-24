"""Host checks of the replug tests' own logic (tests/lib/replug_checks.py,
tests/wii/wii_replug.py): what counts as a device being gone or present, and
that the attended Wii test runs only as a bench job and never on a
production Wii."""

import os
import pathlib
import subprocess
import sys
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401  tools/ and every test layer on sys.path
import replug_checks as rc  # noqa: E402
import wii_replug  # noqa: E402


class StateTests(unittest.TestCase):
    def test_active_device_is_present(self):
        st = {"device": "sd", "inserted": ["sd"], "unmountable": {}}
        self.assertTrue(rc.is_present(st, "sd"))
        self.assertFalse(rc.is_gone(st, "sd"))

    def test_extra_device_is_present_when_listed(self):
        st = {"device": "sd", "inserted": ["sd", "usb"]}
        self.assertTrue(rc.is_present(st, "usb"))

    def test_removed_device_is_gone(self):
        st = {"device": None, "inserted": [], "unmountable": {}}
        self.assertTrue(rc.is_gone(st, "sd"))
        self.assertTrue(rc.is_gone(st, "usb"))

    def test_stale_extra_is_not_gone(self):
        # what an unplugged file-request mount used to keep saying
        st = {"device": "sd", "inserted": ["sd", "usb"]}
        self.assertFalse(rc.is_gone(st, "usb"))

    def test_unmountable_drive_is_not_gone(self):
        st = {"device": "sd", "inserted": ["sd"], "unmountable": {"usb": "NTFS volume"}}
        self.assertFalse(rc.is_gone(st, "usb"))

    def test_missing_keys(self):
        self.assertTrue(rc.is_gone({}, "sd"))
        self.assertFalse(rc.is_present({}, "sd"))


class WiiTestGuard(unittest.TestCase):
    def run_script(self, ip, job):
        env = {k: v for k, v in os.environ.items()
               if k not in ("WII_BENCH_IP", "WII_BENCH_JOB", "HBC_WII")}
        if ip:
            env["WII_BENCH_IP"] = ip
        if job:
            env["WII_BENCH_JOB"] = "test"
        return subprocess.run([sys.executable, str(root / "tests/wii/wii_replug.py")], env=env,
                              capture_output=True, text=True, timeout=30)

    def test_refuses_outside_the_bench(self):
        for ip in (None, "192.0.2.1"):
            res = self.run_script(ip, job=False)
            self.assertEqual(res.returncode, 1, ip)
            self.assertIn("refusing", res.stderr)

    def test_refuses_production_wiis(self):
        for ip in sorted(wii_replug.PRODUCTION):
            res = self.run_script(ip, job=True)
            self.assertEqual(res.returncode, 1, ip)
            self.assertIn("refusing", res.stderr)


if __name__ == "__main__":
    unittest.main()
