"""A scripted controller for Dolphin over the DSU (cemuhook) protocol, so a test
profile's emulated Wii Remote can be driven without touching the desktop.

    python dsu_pad.py PORT "STEP; STEP; ..."

A step is `wait S`, `press BUTTON [S]` (hold, default 0.25 s),
`down BUTTON` and `up BUTTON` (held across the steps between, as for a drag), or
`stick X Y` (right stick, -1..1, held until the next stick step). Buttons are
DSU names: Cross, Circle, Square, Triangle, L1, R1, PS, Options, Share, and
PadN/PadS/PadE/PadW. Exits after the last step (and a second of idle)."""
import socket, struct, sys, threading, time, zlib

PORT = int(sys.argv[1])
STEPS = [s.strip() for s in sys.argv[2].split(";") if s.strip()]
UID = 0x48424352  # 'HBCR'

state = {"buttons": set(), "rx": 0.0, "ry": 0.0}
clients = {}  # addr -> last request time
lock = threading.Lock()
counter = 0


def message(mtype, body):
    raw = struct.pack("<4sHHII", b"DSUS", 1001, 4 + len(body), 0, UID) + struct.pack("<I", mtype) + body
    crc = zlib.crc32(raw) & 0xffffffff
    return raw[:8] + struct.pack("<I", crc) + raw[12:]


def port_info(pad):
    connected = pad == 0
    return message(0x100001, struct.pack("<BBBB6sBB", pad, 2 if connected else 0, 2, 2,
                                         bytes([0, 0, 0, 0, 0, 1 + pad]), 5, 0))


def pad_data():
    global counter
    counter += 1
    with lock:
        b = state["buttons"]
        rx, ry = state["rx"], state["ry"]
    s1 = (0x01 if "Share" in b else 0) | (0x08 if "Options" in b else 0) | \
         (0x10 if "PadN" in b else 0) | (0x20 if "PadE" in b else 0) | \
         (0x40 if "PadS" in b else 0) | (0x80 if "PadW" in b else 0)
    s2 = (0x04 if "L1" in b else 0) | (0x08 if "R1" in b else 0) | (0x10 if "Triangle" in b else 0) | \
         (0x20 if "Circle" in b else 0) | (0x40 if "Cross" in b else 0) | (0x80 if "Square" in b else 0)
    analog = lambda name: 255 if name in b else 0
    stick = lambda v: max(0, min(255, int(round(128 + v * 127))))
    body = struct.pack("<BBBB6sB", 0, 2, 2, 2, bytes([0, 0, 0, 0, 0, 1]), 5)
    body += struct.pack("<BIBBBB", 1, counter, s1, s2, 1 if "PS" in b else 0, 0)
    body += struct.pack("<BBBB", 128, 128, stick(rx), 255 - stick(ry))
    body += bytes([analog("PadW"), analog("PadS"), analog("PadE"), analog("PadN"),
                   analog("Square"), analog("Cross"), analog("Circle"), analog("Triangle"),
                   analog("R1"), analog("L1"), 0, 0])
    body += bytes(12)  # two touches
    body += struct.pack("<Q6f", int(time.time() * 1e6), 0.0, 1.0, 0.0, 0.0, 0.0, 0.0)
    return message(0x100002, body)


sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", PORT))
sock.settimeout(0.01)
done = threading.Event()


def serve():
    next_send = 0.0
    while not done.is_set():
        try:
            data, addr = sock.recvfrom(1024)
            if len(data) >= 20 and data[:4] == b"DSUC":
                mtype = struct.unpack_from("<I", data, 16)[0]
                if mtype == 0x100000:
                    sock.sendto(message(0x100000, struct.pack("<HH", 1001, 0)), addr)
                elif mtype == 0x100001:
                    n = struct.unpack_from("<I", data, 20)[0]
                    for pad in data[24:24 + min(n, 4)]:
                        sock.sendto(port_info(pad), addr)
                elif mtype == 0x100002:
                    with lock:
                        clients[addr] = time.monotonic()
        except socket.timeout:
            pass
        now = time.monotonic()
        if now >= next_send:
            next_send = now + 0.008
            with lock:
                live = [a for a, t in clients.items() if now - t < 5]
            msg = pad_data()
            for a in live:
                sock.sendto(msg, a)


t = threading.Thread(target=serve, daemon=True)
t.start()
for step in STEPS:
    word = step.split()
    if word[0] == "wait":
        time.sleep(float(word[1]))
    elif word[0] == "press":
        hold = float(word[2]) if len(word) > 2 else 0.25
        with lock:
            state["buttons"].add(word[1])
        time.sleep(hold)
        with lock:
            state["buttons"].discard(word[1])
        time.sleep(0.15)
    elif word[0] in ("down", "up"):        # hold a button across later steps, let it go
        with lock:
            (state["buttons"].add if word[0] == "down" else state["buttons"].discard)(word[1])
    elif word[0] == "stick":
        with lock:
            state["rx"], state["ry"] = float(word[1]), float(word[2])
    print(time.strftime("%H:%M:%S"), step, "clients", len(clients), flush=True)
time.sleep(1)
done.set()
