"""Check that every Wii Remote slot has a command queue, in Dolphin.

libogc 3.1 builds each of its six Bluetooth slots a queue of 256 commands in
its 64 KB kernel heap. Only three fit, so on a Wii a fourth Wii Remote, or a
Balance Board left on, sends its first command (its LEDs) to a queue that was
never built and the program crashes as it connects. Dolphin cannot connect
that fourth device, but the queues are there to read at start-up.

    make -C tests/wpad_queue_app
    python tests/dolphin_wpad_queue.py            # the fixed app: PASS
    python tests/dolphin_wpad_queue.py --stock    # libogc as it is: slots 3-5 empty

The app (tests/wpad_queue_app) starts WPAD and waits. This boots it with
Dolphin's GDB stub on, finds wiiuse's table of Wii Remotes (`__wm`, from the
ELF) and reads each one's queue head. A built queue has a first free block;
an empty one has none. --stock reports the slots and fails when the queues
are as the bug leaves them (3-5 empty), so a libogc that fixes this shows up.
"""

import pathlib
import socket
import struct
import sys
import time

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests"))
import dolphin_ogc_crash  # noqa: E402
import dolphin_smoke  # noqa: E402

SLOTS = 6   # WPAD_MAX_DEVICES in libogc 3.1
CMDQ = 4    # offset of wiimote_t's cmdq, after unid


def main():
    args = sys.argv[1:]
    stock = "--stock" in args
    app = root / "tests/wpad_queue_app" / ("wpad_queue_app" if stock else "wpad_queue_app-fixed")
    dol = app.with_suffix(".dol")
    if not dol.is_file():
        raise SystemExit(f"no such DOL: {dol} (make -C tests/wpad_queue_app)")
    wm_at = dolphin_ogc_crash.symbols(app.with_suffix(".elf"))["__wm"]

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    dolphin_smoke.SETTINGS.append(f"Dolphin.General.GDBPort={port}")
    run = dolphin_smoke.Dolphin(dol, net=False)
    result = 1
    try:
        gdb = dolphin_ogc_crash.GDB(port)
        gdb.send("?")
        gdb.send("c", reply=False)
        time.sleep(8)   # WPAD_Init has run long before this
        gdb.interrupt()
        table = struct.unpack(">I", gdb.read(wm_at, 4))[0]
        assert table, "wiiuse has no table of Wii Remotes: WPAD_Init did not run"
        built = []
        for i in range(SLOTS):
            wm = struct.unpack(">I", gdb.read(table + 4 * i, 4))[0]
            first = struct.unpack(">I", gdb.read(wm + CMDQ, 4))[0] if wm else 0
            built.append(bool(first))
        print("command queue built, slots 0-5:", " ".join("yes" if b else "NO" for b in built))
        if stock:
            assert built[:3] == [True] * 3, "the first three queues should fit"
            assert not any(built[3:]), "slots 3-5 have queues: libogc no longer has the bug"
            print("libogc as it is: slots 3-5 have no command queue (the bug): PASS")
        else:
            assert all(built), "a slot has no command queue"
            print("every slot has a command queue: PASS")
        result = 0
    except (AssertionError, OSError) as exc:
        print(f"FAIL: {exc!r}")
    finally:
        run.stop()
    return result


if __name__ == "__main__":
    sys.exit(main())
