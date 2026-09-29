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
                        WII_BENCH_NO_DISPATCH="1", WII_BENCH_IP="127.0.0.1")

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


if __name__ == "__main__":
    unittest.main()
