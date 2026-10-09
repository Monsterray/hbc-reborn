"""Crash an agent app in Dolphin, and check the agent recorded it and handed
the crash on to libogc's crash screen. On libogc2 (or libogc 1.x) the
exception table needs the assembly entry in sdk/hbc_agent/ogc_exc.S, and a C
function there froze a Wii hard; on libogc 3 the hook is PPCExcptCurPanicFn.

    make -C tests/agent_app OGC=libogc2 MODE=crash DEVKITPPC=<devkitPPC r41-2>
    python tests/dolphin_ogc_crash.py [tests/agent_app/agent_app-libogc2-crash.dol]
    make -C tests/agent_app MODE=crash
    python tests/dolphin_ogc_crash.py [--bad-guard | --hook-fault] tests/agent_app/agent_app-crash.dol

Both options break the crash hook before the crash. A fault inside an
exception handler comes back to the hook on the stack it faulted on, so
before HBC 1.10.2 a hook that faulted recursed down through all of MEM1 into
the vectors, and the Wii froze with no crash screen (USB Loader GX did).
--bad-guard sets the stack guard's stk_lo (safety.c) to 0x388, the value GX's
freeze ended with: the hook must check the markers' addresses and not fault.
--hook-fault makes safety_on_death's first instruction a load from 0x388:
the app's crash must still be the one recorded. On libogc 3 the hook comes
back once, notes its fault in the reason and shows the app's crash; libogc2's
vector code sends a fault inside a handler straight to its own crash screen,
which shows the hook's fault. Either way libogc's crash screen must show.

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
# Version 2 (sdk/hbc_agent.h): then app[20], kind, code, reason[64], check.
CRASH_SIZE = CRASH_WORDS * 4 + 20 + 4 + 4 + 64 + 4
NAMES = {2: "machine check", 3: "DSI", 4: "ISI", 6: "alignment", 7: "program"}
REASON_AT = CRASH_WORDS * 4 + 20 + 4 + 4


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

    def write(self, addr, data):
        reply = self.send(f"M{addr:x},{len(data):x}:{data.hex()}")
        if reply != "OK":
            raise OSError(f"GDB stub write {addr:08x}: {reply!r}")

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
            "uptime_ms": w[11], "frames": [hexw(f) for f in w[12:24] if f], "app": app,
            "reason": raw[REASON_AT:REASON_AT + 64].split(b"\0")[0].decode(errors="replace"),
            "kind": ["exception", "fatal", "hang"][min(2, struct.unpack_from(">I", raw, CRASH_WORDS * 4 + 20)[0])]}


def check_block(raw):
    words = struct.unpack(f">{len(raw) // 4}I", raw)
    x = 0x5a17c0de
    for v in words[:-1]:
        x = (((x << 5) | (x >> 27)) & 0xffffffff) ^ v
    assert x == words[-1], f"crash block check {x:08x} != {words[-1]:08x}"


def symbols(elf):
    """{name: address} for every symbol in the ELF, from nm."""
    nm = agent_checks.hbc.devkit_tool("powerpc-eabi-nm")
    out = dolphin_smoke.subprocess.run([nm, str(elf)], capture_output=True, text=True).stdout
    return {m[2]: int(m[1], 16) for m in re.finditer(r"^([0-9a-f]+) \w (\S+)$", out, re.M)}


def break_the_hook(gdb, syms, how):
    """Once the stack guard is up (the agent has started), break the hook."""
    deadline = time.monotonic() + 30
    while not gdb.read(syms["marks"], 1)[0]:
        if time.monotonic() > deadline:
            raise OSError("the stack guard never put its markers down")
        gdb.send("c", reply=False)
        time.sleep(0.5)
        gdb.interrupt()
    if how == "--bad-guard":
        addr, word = syms["stk_lo"], 0x388
    else:
        # lwz r0,0x388(0): nothing has run safety_on_death yet, so no
        # translated copy of the old instruction is left to run instead.
        addr, word = syms["safety_on_death"], 0x80000388
    gdb.write(addr, struct.pack(">I", word))
    assert gdb.read(addr, 4) == struct.pack(">I", word), f"write to {addr:08x} did not stick"
    print(f"{how}: {word:08x} written at {addr:08x}")


def main():
    args = sys.argv[1:]
    breaks = [a for a in args if a in ("--bad-guard", "--hook-fault")]
    args = [a for a in args if a not in breaks]
    if len(breaks) > 1:
        raise SystemExit("one of --bad-guard and --hook-fault")
    how = breaks[0] if breaks else None
    dol = pathlib.Path(args[0] if args else
                       root / "tests/agent_app/agent_app-libogc2-crash.dol").resolve()
    elf = dol.with_suffix(".elf")
    mode = "trap" if "trap" in dol.stem else "crash"
    if not dol.is_file():
        raise SystemExit(f"no such DOL: {dol} (see this script's docstring)")
    agent_checks.ELF = elf
    syms = symbols(elf)
    # libogc 3 (tuxedo) calls the agent's panic function from its own
    # handler; the older libogc jumps to the agent's assembly entry.
    # Both end in waitForReload, the screen's loop (inlined in libogc2).
    tuxedo = "agent_exc_entry" not in syms
    screen = {"__libogc_panic" if tuxedo else "c_default_exceptionhandler", "waitForReload"}

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
        if how:
            break_the_hook(gdb, syms, how)
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
        if how == "--hook-fault" and tuxedo:
            note = f"agent crash hook faulted: pc {syms['safety_on_death']:08x} dar 00000388"
            assert crash["reason"] == note, f"reason {crash['reason']!r}, not {note!r}"
            print(f"reason: {crash['reason']}")
        elif crash["reason"]:
            raise AssertionError(f"reason {crash['reason']!r} for a crash the hook saw cleanly")

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
        assert screen & set(names), "not in libogc's crash screen"
        if tuxedo:
            # agent_panic and __libogc_panic end in tail calls, so neither is
            # on the stack; the hook must still be the one libogc calls.
            hook = struct.unpack(">I", gdb.read(syms["PPCExcptCurPanicFn"], 4))[0]
            assert hook == syms["agent_panic"], f"PPCExcptCurPanicFn is {hook:08x}"
        else:
            assert "agent_exc_entry" in names, "libogc's crash screen was not reached through the agent"
        print(f"libogc crash hook{f' ({how[2:]})' if how else ''}: PASS")
        result = 0
    except (AssertionError, OSError) as exc:
        print(f"FAIL: {exc!r}")
        try:   # where the CPU is: a nested-exception loop shows here
            print(f"cpu: pc {gdb.pc():08x} sp {int(gdb.send('p1'), 16):08x} "
                  f"msr {int(gdb.send('p41'), 16):08x}")
        except (NameError, OSError, ValueError):
            pass
    finally:
        run.stop()
    for line in run.faults([r"0x00000010", r"DSI", r"Program"])[:20]:
        print("log:", line)
    return result


if __name__ == "__main__":
    sys.exit(main())
