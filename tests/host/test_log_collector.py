"""agent_checks.LogCollector.wait_for: after mark(), an earlier app's log line
must not answer a wait for the app sent next (wii_agent sends one app after
another, each printing the same "running" lines)."""

import pathlib
import socket
import sys
import time
import unittest

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests" / "lib"))
import testenv  # noqa: E402,F401
import agent_checks  # noqa: E402


class LogCollectorTest(unittest.TestCase):
    def setUp(self):
        self.log = agent_checks.LogCollector(0)
        self.addCleanup(self.log.close)

    def send(self, conn, text):
        conn.sendall(text.encode())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            with self.log.lock:
                if self.log.text.endswith(text):
                    return
            time.sleep(0.02)
        self.fail("the collector did not take the text")

    def test_wait_ignores_text_before_the_mark(self):
        with socket.create_connection(("127.0.0.1", self.log.server.port)) as conn:
            self.send(conn, "agent_app: running, 5 s\n")
            self.log.wait_for("running, 5 s", 2)      # nothing marked: all text counts
            self.log.mark()
            with self.assertRaises(AssertionError):
                self.log.wait_for("running, 5 s", 0.5)  # the old line is not this app's
            self.send(conn, "agent_app: running, 5 s\n")
            self.log.wait_for("running, 5 s", 2)      # the new app's own line

    def test_wait_sees_a_line_that_arrives_during_the_wait(self):
        with socket.create_connection(("127.0.0.1", self.log.server.port)) as conn:
            self.log.mark()
            conn.sendall(b"agent_app: network 0\n")
            self.log.wait_for("network 0", 5)


if __name__ == "__main__":
    unittest.main()
