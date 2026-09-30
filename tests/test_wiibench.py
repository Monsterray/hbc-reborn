"""Check tools/wii-bench/wiibench.py's queue handling without a Wii."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

root = pathlib.Path(__file__).resolve().parents[1]
BENCH = root / "tools/wii-bench/wiibench.py"


class WiiBenchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = pathlib.Path(self.tmp.name)
        # No dispatcher, and a Wii address nothing answers on.
        self.env = dict(os.environ, WII_BENCH_HOME=str(self.home),
                        WII_BENCH_NO_DISPATCH="1", WII_BENCH_IP="127.0.0.1", WII_BENCH_SERVER="")

    def tearDown(self):
        self.tmp.cleanup()

    def bench(self, *args):
        return subprocess.run([sys.executable, str(BENCH), *args], env=self.env,
                              capture_output=True, text=True, timeout=30)

    def test_add_status_cancel(self):
        added = self.bench("add", "--name", "unit job", "--cwd", str(root), "--",
                           sys.executable, "-c", "print('hi')")
        self.assertEqual(added.returncode, 0, added.stderr)
        job_id = added.stdout.strip()
        job = json.loads((self.home / "queue/pending" / f"{job_id}.json").read_text())
        self.assertEqual(job["name"], "unit job")
        self.assertEqual(job["cmd"][1:], ["-c", "print('hi')"])
        self.assertFalse((self.home / "dispatcher.lock").exists())

        status = self.bench("status")
        self.assertIn("busy or off", status.stdout)
        self.assertIn(job_id, status.stdout)

        self.assertEqual(self.bench("cancel", job_id).returncode, 0)
        self.assertFalse((self.home / "queue/pending" / f"{job_id}.json").exists())
        self.assertNotEqual(self.bench("cancel", job_id).returncode, 0)

    def test_jobs_run_oldest_first(self):
        ids = [self.bench("add", "--cwd", str(root), "--", sys.executable, "-c", "0").stdout.strip()
               for _ in range(3)]
        sys.path.insert(0, str(BENCH.parent))
        try:
            os.environ["WII_BENCH_HOME"] = str(self.home)
            import importlib
            import wiibench
            importlib.reload(wiibench)
            order = [p.stem for p in wiibench.jobs(wiibench.PENDING)]
        finally:
            sys.path.remove(str(BENCH.parent))
            os.environ.pop("WII_BENCH_HOME", None)
        self.assertEqual(order, ids)

    def test_setup_writes_a_shim_and_the_server(self):
        env = dict(self.env)
        del env["WII_BENCH_SERVER"]                # read from HOME/server instead
        run = lambda *args: subprocess.run([sys.executable, *args], env=env, capture_output=True,
                                           text=True, timeout=60)
        r = run(str(BENCH), "setup", "--server", "http://127.0.0.1:9/")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("unreachable", r.stdout)
        self.assertEqual((self.home / "server").read_bytes(), b"http://127.0.0.1:9/\n")
        r = run(str(self.home / "wiibench.py"), "status")   # the path other projects call
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("lease http://127.0.0.1:9: unreachable", r.stdout)
        self.assertEqual(run(str(BENCH), "setup").returncode, 0)   # again: leaves the shim
        (self.home / "wiibench.py").write_text("print('mine')\n")
        self.assertNotEqual(run(str(BENCH), "setup").returncode, 0)

    def test_server_file_in_any_encoding(self):
        wb = import_wiibench()
        p = self.home / "server"
        for data in ("http://h:4310\r\n".encode("utf-16"),        # Windows PowerShell 5.1 echo >
                     b"\xef\xbb\xbfhttp://h:4310\r\n", b"http://h:4310\n"):
            p.write_bytes(data)
            self.assertEqual(wb.read_setting(p), "http://h:4310")


def import_wiibench():
    sys.path.insert(0, str(BENCH.parent))
    try:
        import wiibench
        return wiibench
    finally:
        sys.path.remove(str(BENCH.parent))


class LeaseTest(unittest.TestCase):
    def setUp(self):
        self.wb = import_wiibench()
        self.now = 0.0
        self.leases = self.wb.Leases(ttl=60, clock=lambda: self.now)
        self.now = 60.0                            # past the start-up grace

    def acq(self, t):
        return self.leases.acquire({"ticket": t, "host": t, "name": "job"})

    def test_first_come_first_served(self):
        self.assertTrue(self.acq("a")["granted"])
        self.assertEqual(self.acq("b"), {"granted": False, "position": 1, "holder": self.leases.status()["holder"]})
        self.assertEqual(self.acq("c")["position"], 2)
        self.assertFalse(self.acq("c")["granted"])  # polling keeps its place
        self.leases.release({"ticket": "a"})
        self.assertFalse(self.acq("c")["granted"])  # b is first in line
        self.assertTrue(self.acq("b")["granted"])
        self.assertTrue(self.acq("b")["granted"])   # asking again while holding it

    def test_dead_holder_and_waiter_expire(self):
        self.acq("a")
        self.acq("b")
        self.now += 30
        self.acq("c")
        self.now += 31                             # a and b silent for 61 s, c for 31
        self.assertTrue(self.acq("c")["granted"])
        self.assertEqual(self.leases.status()["waiters"], [])

    def test_renew_keeps_it_and_reclaims_after_a_restart(self):
        self.acq("a")
        for _ in range(5):
            self.now += 50
            self.assertTrue(self.leases.renew({"ticket": "a"})["ok"])
        self.assertFalse(self.acq("b")["granted"])
        self.assertFalse(self.leases.renew({"ticket": "b"})["ok"])   # a waiter cannot renew its way in

        restarted = self.wb.Leases(ttl=60, clock=lambda: self.now)
        self.assertFalse(restarted.acquire({"ticket": "b", "host": "b", "name": "j"})["granted"])  # grace
        self.assertTrue(restarted.renew({"ticket": "a", "host": "a", "name": "j"})["ok"])
        self.now += 61
        restarted.renew({"ticket": "a"})
        self.assertFalse(restarted.acquire({"ticket": "b", "host": "b", "name": "j"})["granted"])

    def test_http_round_trip(self):
        import threading
        srv = self.wb.make_server(0, ttl=0.5, host="127.0.0.1")
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        os.environ["WII_BENCH_SERVER"] = f"http://127.0.0.1:{srv.server_address[1]}"
        self.addCleanup(os.environ.pop, "WII_BENCH_SERVER")
        import time
        time.sleep(0.6)                            # the start-up grace
        with self.wb.Turn("first") as t:
            s = self.wb.lease_call("/status")
            self.assertEqual(s["holder"]["name"], "first")
            self.assertFalse(self.wb.lease_call("/acquire", {"ticket": "x", "host": "h", "name": "second"})["granted"])
        self.assertIsNone(self.wb.lease_call("/status")["holder"])
        self.assertTrue(self.wb.lease_call("/acquire", {"ticket": "x", "host": "h", "name": "second"})["granted"])


if __name__ == "__main__":
    unittest.main()
