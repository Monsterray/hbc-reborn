"""Checks for sdk/hbc_agent, shared by tests/wii_agent.py and
tests/dolphin_smoke.py --agent.

check_agent() needs HBC answering on `wii` with an SD card, and ends with
HBC answering again. It runs tests/agent_app three times: once to exercise
status, files, logging and `exit`; once to show that a plain Wiiload upload
to a running agent app lands in HBC; and once to crash it, returning the
crash report HBC saw (or None when that HBC predates crash reports).
"""

import os
import pathlib
import re
import socket
import subprocess
import sys
import threading
import time
import zlib

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

APP = root / "tests/agent_app/agent_app.dol"
ELF = root / "tests/agent_app/agent_app.elf"


class LogCollector:
    """Accept any number of app log connections and keep their text."""

    def __init__(self, port):
        self.server = hbc.LogServer(port)
        self.lock = threading.Lock()
        self.text = ""
        self.closed = 0
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                conn, _ = self.server.sock.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            threading.Thread(target=self.read, args=(conn,), daemon=True).start()

    def read(self, conn):
        conn.settimeout(1)
        with conn:
            while True:
                try:
                    chunk = conn.recv(4096)
                except TimeoutError:
                    continue
                except (ConnectionResetError, OSError):
                    break  # IOS closes sockets with a reset
                if not chunk:
                    break
                with self.lock:
                    self.text += chunk.decode("utf-8", "replace")
        with self.lock:
            self.closed += 1

    def wait_for(self, needle, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            with self.lock:
                if needle in self.text:
                    return
            time.sleep(0.2)
        raise AssertionError(f"no {needle!r} in the app log; got {self.text!r}")

    def close(self):
        self.server.close()


def wait_agent(wii, seconds=30):
    deadline, last = time.monotonic() + seconds, None
    while time.monotonic() < deadline:
        try:
            last = hbc.version(wii, hbc.POLL_TIMEOUT)
            if hbc.is_agent(last):
                return last
        except (OSError, hbc.HBCError) as exc:
            last = exc
        time.sleep(0.5)
    raise AssertionError(f"the agent did not answer within {seconds} s: {last}")


def symbol_range(name):
    """(start, end) of a function in agent_app.elf, from nm."""
    nm = hbc.devkit_tool("powerpc-eabi-nm")
    if not nm:
        return None
    out = subprocess.run([nm, "-S", str(ELF)], capture_output=True, text=True).stdout
    m = re.search(rf"^([0-9a-f]+) ([0-9a-f]+) T {name}$", out, re.M)
    return (int(m[1], 16), int(m[1], 16) + int(m[2], 16)) if m else None


def check_agent(wii, log_port=0, crash_mode="crash", back_in_hbc=None):
    """back_in_hbc(wii) runs whenever an app has exited, and must leave an
    HBC with crash reports answering; the default waits for HBC."""
    back = back_in_hbc or (lambda w: hbc.hbc_wait(w, 60))
    back(wii)
    reports_crash = hbc.status(wii).get("proto", 1) >= 3
    if reports_crash:
        hbc.request(wii, b"HBCC")

    log = LogCollector(log_port)
    try:
        log.server.register(wii)

        # 1. Status, files and logging inside a running app.
        hbc.send(wii, str(APP), ["stay", "180"])
        text = wait_agent(wii)
        log.wait_for("agent_app: network 0", 30)
        st = hbc.status(wii)
        print("agent status:", st)
        assert text == st["version"] + hbc.AGENT_SUFFIX, (text, st)
        assert st["agent"] is True and st["app"] == "agent_app" and st["app_version"] == "1", st
        assert st["device"] == "sd" and "sd" in st["inserted"], st
        assert 0 < st["agent_stack_used"] < st["agent_stack_size"], st
        assert st["exit_requested"] is False and st["uptime_ms"] > 0, st
        heap_before = st["heap_free"]

        samples = {"random": os.urandom(300_001), "zeros": bytes(200_000),
                   "exact": os.urandom(65536), "tiny": b"x"}
        hbc.file_request(wii, "M", "sd:/hbctest/agent")
        for name, blob in samples.items():
            path = f"sd:/hbctest/agent/{name}.bin"
            hbc.put_file(wii, path, blob)
            up = hbc.status(wii)["last"]
            assert hbc.get_file(wii, path) == blob, name
            down = hbc.status(wii)["last"]
            assert hbc.checksum(wii, path) == (len(blob), zlib.crc32(blob)), name
            assert hbc.file_request(wii, "G", path) == blob, f"{name} (protocol 1 read)"
            print(f"  {name}: {len(blob)} bytes, wire up {up['wire']} / down {down['wire']}, "
                  f"up {up['ms']} ms / down {down['ms']} ms")
        raw = os.urandom(50_000)
        hbc.file_request(wii, "P", "sd:/hbctest/agent/raw.bin", len(raw), raw)
        listing = hbc.file_request(wii, "L", "sd:/hbctest/agent").decode()
        for name in list(samples) + ["raw"]:
            assert f" {name}.bin" in listing, listing
        hbc.remove_tree(wii, "sd:/hbctest", out=lambda *a: None)
        try:
            hbc.file_request(wii, "L", "sd:/hbctest/../x")
            raise AssertionError("the agent accepted a bad path")
        except hbc.HBCError:
            pass
        st = hbc.status(wii)
        print(f"  heap {heap_before} free before transfers, {st['heap_free']} after; "
              f"agent stack {st['agent_stack_used']} of {st['agent_stack_size']}")
        print("agent files and status: PASS")

        check_overlay(wii, log, st)

        # 2. `hbc.py exit`: the app sees the request and exits to HBC.
        start = time.monotonic()
        assert hbc.exit_app(wii)
        log.wait_for("agent_app: exit requested", 10)
        print(f"agent exit to HBC: PASS ({time.monotonic() - start:.1f} s until HBC answered)")
        back(wii)

        # 3. A plain Wiiload upload to a running agent app ends up in HBC, so
        # a retry works (hbc.py send/run do that exit first themselves).
        hbc.send(wii, str(APP), ["stay", "180"])
        wait_agent(wii)
        try:
            hbc.send(wii, str(APP), ["exit"])
        except OSError:
            pass  # the agent resets the connection
        back(wii)
        print("agent Wiiload upload returns to HBC: PASS")

        # 4. A crash: libogc's screen, then back to HBC with a report.
        # The app lives about a second, too short to poll for reliably; its
        # log line shows it ran.
        hbc.send(wii, str(APP), [crash_mode])
        log.wait_for(f"agent_app: crashing ({crash_mode})", 30)
        hbc.hbc_wait(wii, 60)
        # The first HBC back takes the report (an installed 1.5.0 or later
        # does too), before back() may send this tree's DOL.
        first = hbc.status(wii)
        crash = first.get("crash") if first.get("proto", 1) >= 3 else None
        back(wii)
        if crash is None and reports_crash:
            crash = hbc.status(wii).get("crash")
        if reports_crash:
            check_crash(crash, crash_mode)
            hbc.request(wii, b"HBCC")
            assert hbc.status(wii)["crash"] is None
            print("agent crash report: PASS")
        return crash
    finally:
        hbc.LogServer.unregister(wii)
        log.close()


def strip_luma(wii):
    """Mean luma of the bottom tenth of the TV picture, where the overlay's
    strip sits over agent_app's colour bars."""
    width, height, data = hbc.screen(wii)
    rows = data[width * 2 * (height * 9 // 10):]
    return sum(rows[0::2]) / (len(rows) // 2)


def overlay_keys(wii, keys, settle=1.0):
    hbc.send_keys(wii, keys)
    time.sleep(settle + 0.25 * len(keys))


def check_overlay(wii, log, st):
    """The HOME overlay, driven with `hbc.py key` and seen with `hbc.py screen`."""
    shots_before = {n for _, _, n in hbc.list_dir(wii, "sd:/screenshots")[0]} \
        if "d screenshots" in hbc.file_request(wii, "L", "sd:/").decode() else set()
    app = strip_luma(wii)
    overlay_keys(wii, "h")
    log.wait_for("agent_app: overlay\n", 10)
    shown = strip_luma(wii)
    assert shown < app - 20, f"the strip did not appear (luma {app:.0f} -> {shown:.0f})"

    # Shot is right of Exit, which has the focus when the overlay opens.
    overlay_keys(wii, "ra", settle=3)
    after = {n for _, _, n in hbc.list_dir(wii, "sd:/screenshots")[0]}
    new = sorted(after - shots_before)
    assert new, f"Shot saved nothing: {sorted(after)}"
    size = next(s for _, s, n in hbc.list_dir(wii, "sd:/screenshots")[0] if n == new[-1])
    width, height, _ = hbc.screen(wii)
    assert size == 54 + width * height * 3, (new[-1], size)

    # DEV > Save runs the app's hook: left past the app slot to DEV, then
    # down from the Actions tab (Restart app is greyed out) to Save.
    overlay_keys(wii, "lll" "a" "d" "a")
    log.wait_for("agent_app: saved", 10)
    assert hbc.get_file(wii, "sd:/hbctest/agent_save.txt") == b"saved by agent_app\n"

    # HOME closes everything and gives the app its picture back.
    overlay_keys(wii, "h", settle=2)
    log.wait_for("agent_app: overlay closed (0)", 10)
    closed = strip_luma(wii)
    assert abs(closed - app) < 8, f"the app's picture did not come back ({app:.0f} -> {closed:.0f})"

    # The app's own slot, left of Exit, closes the overlay and runs in the app.
    overlay_keys(wii, "h")
    overlay_keys(wii, "la", settle=2)
    log.wait_for("agent_app: hello from the app slot", 10)

    for name in new:
        hbc.file_request(wii, "D", f"sd:/screenshots/{name}")
    if not shots_before and not (after - set(new)):
        hbc.file_request(wii, "D", "sd:/screenshots")
    hbc.remove_tree(wii, "sd:/hbctest", out=lambda *a: None)
    print(f"agent overlay: PASS (strip luma {app:.0f} -> {shown:.0f} -> {closed:.0f}, "
          f"shot {new[-1]}, remote handles {'found' if st.get('wpad_handles') else 'not found'})")


def check_crash(crash, crash_mode="crash"):
    print(hbc.crash_report(crash, str(ELF)))
    assert crash and crash["app"] == "agent_app", crash
    fn = "agent_app_crash" if crash_mode == "crash" else "agent_app_trap"
    if crash_mode == "crash":
        assert crash["name"] == "DSI" and crash["dar"] == "00000010", crash
    else:
        assert crash["name"] == "program", crash
    where = symbol_range(fn)
    if where:
        assert where[0] <= int(crash["pc"], 16) < where[1], (fn, where, crash["pc"])
        main = symbol_range("main")
        frames = [int(f, 16) for f in crash["frames"]] + [int(crash["lr"], 16)]
        assert any(main[0] <= f < main[1] for f in frames), (main, crash)
