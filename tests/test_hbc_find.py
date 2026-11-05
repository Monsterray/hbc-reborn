#!/usr/bin/env python3
"""hbc.py find and the HBCU discovery query, against fake Wiis on localhost."""

import contextlib
import io
import json
import os
import pathlib
import socket
import struct
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import hbc  # noqa: E402


def reply(name, agent=False, ip=(192, 168, 1, 50)):
    name = name.encode("ascii")
    return b"HBCU" + bytes((1, 1 if agent else 0, len(name), 0)) + bytes(ip) + name


class FakeWii:
    """Answers each HBCU query on a UDP port with the given datagrams."""

    def __init__(self, answers):
        self.answers = answers
        self.queries = []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while True:
            try:
                data, src = self.sock.recvfrom(64)
            except OSError:
                return
            self.queries.append(data)
            if data[:4] == b"HBCU":
                for answer in self.answers:
                    self.sock.sendto(answer, src)

    def close(self):
        self.sock.close()


class FindTest(unittest.TestCase):
    def fake(self, answers):
        wii = FakeWii(answers)
        self.addCleanup(wii.close)
        patcher = mock.patch.object(hbc, "PORT", wii.port)
        patcher.start()
        self.addCleanup(patcher.stop)
        return wii

    def test_reply_parsing(self):
        self.assertEqual(hbc.parse_find_reply(reply("Homebrew Channel")),
                         ("192.168.1.50", False, "Homebrew Channel"))
        self.assertEqual(hbc.parse_find_reply(reply("Wii64", agent=True, ip=(10, 0, 0, 7))),
                         ("10.0.0.7", True, "Wii64"))
        self.assertEqual(hbc.parse_find_reply(reply("")), ("192.168.1.50", False, ""))
        for bad in (b"", b"HBCU", b"HBCD" + reply("x")[4:], reply("abc")[:14],
                    b"HBCU" + bytes((0, 0, 0, 0, 1, 2, 3, 4))):
            self.assertIsNone(hbc.parse_find_reply(bad), bad)

    def test_query_is_sixteen_bytes(self):
        wii = self.fake([reply("Homebrew Channel")])
        hbc.find_wiis(0.5, ["127.0.0.1"])
        self.assertEqual(wii.queries, [b"HBCU" + bytes(12)])

    def test_finds_and_dedupes(self):
        # The answer's source is the address. The console's own address in the
        # reply is the Wii: answers that repeat it (one per network the query
        # reached) are the same Wii; another address is another Wii.
        self.fake([reply("Homebrew Channel"), b"junk", reply("Homebrew Channel"),
                   reply("Wii64", agent=True, ip=(192, 168, 1, 51))])
        found = hbc.find_wiis(0.5, ["127.0.0.1"])
        self.assertEqual(found, [{"address": "127.0.0.1", "agent": False,
                                  "name": "Homebrew Channel"},
                                 {"address": "127.0.0.1", "agent": True, "name": "Wii64"}])

    def test_prefers_the_consoles_own_address(self):
        # One console, reached on two networks: the answer from its own
        # address wins, whichever arrives first.
        answers = [(reply("Homebrew Channel"), ("192.168.139.3", 4299)),
                   (reply("Homebrew Channel"), ("192.168.1.50", 4299))]

        class Scripted:
            def __init__(self, *a):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def setsockopt(self, *a):
                pass

            def bind(self, *a):
                pass

            def sendto(self, *a):
                pass

            def recvfrom(self, n):
                return answers.pop(0)

        ready = lambda r, w, x, t: (r if answers else [], [], [])
        with mock.patch.object(hbc.socket, "socket", Scripted), \
                mock.patch.object(hbc.select, "select", ready):
            found = hbc.find_wiis(0.5, ["192.168.255.255"])
        self.assertEqual(found, [{"address": "192.168.1.50", "agent": False,
                                  "name": "Homebrew Channel"}])

    def test_agent_app(self):
        self.fake([reply("Wii64", agent=True)])
        found = hbc.find_wiis(0.5, ["127.0.0.1"])
        self.assertEqual(hbc.find_line(found[0]), "127.0.0.1        app  Wii64")

    def test_no_answer(self):
        self.fake([])
        self.assertEqual(hbc.find_wiis(0.3, ["127.0.0.1"]), [])

    def test_unsendable_target_is_skipped(self):
        self.fake([reply("Homebrew Channel")])
        found = hbc.find_wiis(0.5, ["256.1.1.1", "127.0.0.1"])
        self.assertEqual(len(found), 1)

    def test_broadcast_targets(self):
        targets = hbc.broadcast_targets()
        self.assertEqual(targets[0], "255.255.255.255")
        self.assertFalse([t for t in targets if t.startswith("127.")])
        self.assertTrue(all(t.endswith(".255") for t in targets))

    def test_command(self):
        self.fake([reply("Homebrew Channel")])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            hbc.main(["find", "--timeout", "0.5", "127.0.0.1"])
        self.assertEqual(out.getvalue(), "127.0.0.1        HBC  Homebrew Channel\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            hbc.main(["--json", "find", "--timeout", "0.5", "127.0.0.1"])
        self.assertEqual(json.loads(out.getvalue()),
                         [{"address": "127.0.0.1", "agent": False, "name": "Homebrew Channel"}])

    def test_command_nothing_found(self):
        self.fake([])
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
            hbc.main(["find", "--timeout", "0.2", "127.0.0.1"])
        self.assertEqual(cm.exception.code, 1)
        self.assertIn("no Wii answered", err.getvalue())

    def test_default_timeout_is_finds(self):
        with mock.patch.object(hbc, "find_wiis", return_value=[{"address": "1.2.3.4",
                                                                 "agent": False, "name": "x"}]) as f:
            with contextlib.redirect_stdout(io.StringIO()):
                hbc.main(["find"])
            f.assert_called_once_with(hbc.FIND_TIMEOUT, None)


NO_ADDRESS = {k: "" for k in ("HBC_WII", "WII_BENCH_IP", "WIILOAD")}


class AddressTest(unittest.TestCase):
    def test_named_wii_skips_find(self):
        with mock.patch.dict(os.environ, NO_ADDRESS), \
                mock.patch.object(hbc, "find_wiis") as f:
            self.assertEqual(hbc.wii_address("10.0.0.2"), "10.0.0.2")
        f.assert_not_called()

    def test_one_wii_found(self):
        with mock.patch.dict(os.environ, NO_ADDRESS), \
                mock.patch.object(hbc, "find_wiis", return_value=[
                    {"address": "10.0.0.3", "agent": False, "name": "Homebrew Channel"}]):
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(hbc.wii_address(None, auto_find=True), "10.0.0.3")
        self.assertIn("using the Wii at 10.0.0.3", err.getvalue())

    def test_several_found(self):
        found = [{"address": "10.0.0.3", "agent": False, "name": "Homebrew Channel"},
                 {"address": "10.0.0.4", "agent": True, "name": "Wii64"}]
        with mock.patch.dict(os.environ, NO_ADDRESS), \
                mock.patch.object(hbc, "find_wiis", return_value=found), \
                self.assertRaises(SystemExit) as cm:
            hbc.wii_address(None, auto_find=True)
        self.assertIn("10.0.0.3", str(cm.exception))
        self.assertIn("10.0.0.4", str(cm.exception))

    def test_none_found(self):
        with mock.patch.dict(os.environ, NO_ADDRESS), \
                mock.patch.object(hbc, "find_wiis", return_value=[]), \
                self.assertRaises(SystemExit) as cm:
            hbc.wii_address(None)
        self.assertIn("Set HBC_WII", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
