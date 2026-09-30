"""Crash an agent app built on libogc2 (or libogc 1.x) in Dolphin, and check
the agent recorded it: the older libogc's exception table needs the assembly
entry in sdk/hbc_agent/ogc_exc.S, and a C function there froze a Wii hard.

    make -C tests/agent_app OGC=libogc2 MODE=crash DEVKITPPC=<devkitPPC r41-2>
    python tests/dolphin_ogc_crash.py [tests/agent_app/agent_app-libogc2-crash.dol]

The DOL crashes by itself (no Wiiload argv here). Dolphin boots it with its
GDB stub on; after the crash this reads the crash block from MEM2, checks it
as tests/agent_checks.py does for a crash HBC reports, and checks the CPU is
in libogc's crash screen rather than stuck in a nested exception. A run
beside another Dolphin is fine: nothing here uses the network."""

import pathlib
import re
import socket
import struct
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import agent_checks  # noqa: E402
import dolphin_smoke  # noqa: E402

CRASH_ADDR = 0x91800020
CRASH_WORDS = 3 + 5 + 2 + 2 + 12   # magic .. uptime_ms, frames
CRASH_SIZE = CRASH_WORDS * 4 + 20 + 4
NAMES = {2: "machine check", 3: "DSI", 4: "ISI", 6: "alignment", 7: "program"}


class GDB:
    """Just enough of the GDB remote protocol for Dolphin's stub."""

    def __init__(self, port, seconds=60):
        deadline = time.monotonic() + seconds
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 2)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.5)
        self.s.settimeout(30)
        self.buf = b""

    def _read_packet(self):
        while True:
            m = re.search(rb"\$([^#]*)#[0-9a-fA-F]{2}", self.buf)
            if m:
                self.buf = self.buf[m.end():]
                self.s.sendall(b"+")
                return m[1].decode()
            chunk = self.s.recv(4096)
            if not chunk:
                raise OSError("GDB stub closed the connection")
            self.buf += chunk

    def send(self, data, reply=True):
        body = data.encode()
        self.s.sendall(b"$%s#%02x" % (body, sum(body) & 0xff))
        return self._read_packet() if reply else None

    def interrupt(self):
        self.s.sendall(b"\x03")
        stop = self._read_packet()
        # Dolphin follows the stop reply with an empty packet (the 'c' it
        # was still answering); drop it so replies line up with requests.
        self.s.settimeout(1)
        try:
            while True:
                self._read_packet()
        except socket.timeout:
            pass
        self.s.settimeout(30)
        return stop

    def read(self, addr, size):
        reply = self.send(f"m{addr:x},{size:x}")
        if len(reply) != size * 2:
            raise OSError(f"GDB stub read {addr:08x}: {reply!r}")
        return bytes.fromhex(reply)

    def pc(self):
        # Dolphin's stub numbers the PC 64 (after r0-r31 and f0-f31).
        return int(self.send("p40"), 16)


def crash_dict(raw):
    w = struct.unpack(f">{CRASH_WORDS}I", raw[:CRASH_WORDS * 4])
    app = raw[CRASH_WORDS * 4:CRASH_WORDS * 4 + 20].split(b"\0")[0].decode(errors="replace")
    hexw = lambda v: f"{v:08x}"
    return {"magic": w[0], "version": w[1], "exception": w[2], "name": NAMES.get(w[2], "unknown"),
            "pc": hexw(w[3]), "msr": hexw(w[4]), "lr": hexw(w[5]), "cr": hexw(w[6]),
            "ctr": hexw(w[7]), "dar": hexw(w[8]), "dsisr": hexw(w[9]), "sp": hexw(w[10]),
            "uptime_ms": w[11], "frames": [hexw(f) for f in w[12:24] if f], "app": app}


def check_block(raw):
    words = struct.unpack(f">{len(raw) // 4}I", raw)
    x = 0x5a17c0de
    for v in words[:-1]:
        x = (((x << 5) | (x >> 27)) & 0xffffffff) ^ v
    assert x == words[-1], f"crash block check {x:08x} != {words[-1]:08x}"


def main():
    dol = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                       root / "tests/agent_app/agent_app-libogc2-crash.dol").resolve()
    elf = dol.with_suffix(".elf")
    mode = "trap" if "trap" in dol.stem else "crash"
    if not dol.is_file():
        raise SystemExit(f"no such DOL: {dol} (see this script's docstring)")
    agent_checks.ELF = elf

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    dolphin_smoke.SETTINGS.append(f"Dolphin.General.GDBPort={port}")
    # MMU on, as dolphin_smoke.py --agent: a store to 0x10 faults only then.
    run = dolphin_smoke.Dolphin(dol, mmu=True)
    result = 1
    try:
        gdb = GDB(port)
        gdb.send("?")
        # The app waits up to 10 s for the network, then crashes 1 s later.
        deadline = time.monotonic() + 60
        while True:
            gdb.send("c", reply=False)
            time.sleep(3)
            gdb.interrupt()
            raw = gdb.read(CRASH_ADDR, CRASH_SIZE)
            crash = crash_dict(raw)
            if crash["magic"] == 0x48424343 or time.monotonic() > deadline:
                break
        # Let libogc's crash screen run a moment before looking at the CPU.
        gdb.send("c", reply=False)
        time.sleep(2)
        gdb.interrupt()
        assert crash["magic"] == 0x48424343, f"no crash block: {raw[:16].hex()}"
        check_block(raw)
        agent_checks.check_crash(crash, mode)

        # Where the CPU is now: in libogc's crash screen, which waits for a
        # button or its reload timer, called from the agent's entry. A frame
        # the entry built wrongly would have faulted again inside it instead.
        chain = [gdb.pc(), int(gdb.send("p43"), 16)]
        sp = int(gdb.send("p1"), 16)
        for _ in range(16):
            if not 0x80000000 <= sp < 0x81800000:
                break
            back = struct.unpack(">I", gdb.read(sp, 4))[0]
            if not sp < back < 0x81800000:
                break
            chain.append(struct.unpack(">I", gdb.read(back + 4, 4))[0])
            sp = back
        out = dolphin_smoke.subprocess.run(
            [agent_checks.hbc.devkit_tool("powerpc-eabi-addr2line"), "-f", "-e", str(elf)] +
            [f"0x{a:08x}" for a in chain], capture_output=True, text=True).stdout.split("\n")
        names = out[0::2][:len(chain)]
        print("cpu after the crash:", " <- ".join(f"{n} ({a:08x})" for a, n in zip(chain, names)))
        # (waitForReload is the screen's loop, inlined into the handler.)
        assert {"c_default_exceptionhandler", "waitForReload"} & set(names),             "not in libogc's crash screen"
        assert "agent_exc_entry" in names, "libogc's crash screen was not reached through the agent"
        print("libogc crash hook: PASS")
        result = 0
    except (AssertionError, OSError) as exc:
        print(f"FAIL: {exc!r}")
    finally:
        run.stop()
    for line in run.faults([r"0x00000010", r"DSI", r"Program"])[:20]:
        print("log:", line)
    return result


if __name__ == "__main__":
    sys.exit(main())
