"""Run tests/netblock on a Wii and print TCP throughput by IOS call size.

    python tests/netblock_run.py [WII] [--port 4300]

Sends netblock.dol through HBC with this PC's address, serves the app's
SEND/RECV requests, prints each result in MB/s, and waits for HBC to come
back. The port must accept inbound connections (the bench PC's firewall
allows 4300)."""

import argparse
import pathlib
import socket
import sys

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import hbc  # noqa: E402

DOL = root / "tests/netblock/netblock.dol"


def recv_exact(conn, n):
    got = 0
    while got < n:
        chunk = conn.recv(min(1 << 20, n - got))
        if not chunk:
            raise OSError("the Wii closed the connection")
        got += len(chunk)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wii", nargs="?")
    parser.add_argument("--port", type=int, default=4300)
    opts = parser.parse_args()
    wii = hbc.wii_address(opts.wii)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.connect((wii, 9))  # no packet is sent
        me = udp.getsockname()[0]

    srv = socket.create_server(("0.0.0.0", opts.port))
    srv.settimeout(90)
    hbc.hbc_wait(wii, 120)
    hbc.send(wii, str(DOL), [me, str(opts.port)])
    conn, _ = srv.accept()
    conn.settimeout(120)
    payload = bytes(range(256)) * 4096
    pending = b""
    print("round  how     block  Wii sends  Wii receives", flush=True)
    while True:
        while b"\n" not in pending:
            chunk = conn.recv(256)
            if not chunk:
                raise SystemExit("the Wii closed the connection early")
            pending += chunk
        text, pending = pending.split(b"\n", 1)
        word = text.decode().split()
        if word[0] == "SEND":
            n = int(word[1])
            if len(pending) >= n:
                pending = pending[n:]
            else:
                recv_exact(conn, n - len(pending))
                pending = b""
            conn.sendall(b"K")
        elif word[0] == "RECV":
            n = int(word[1])
            for i in range(0, n, len(payload)):
                conn.sendall(payload[:min(len(payload), n - i)])
        elif word[0] == "RESULT":
            rnd, how, kib, up, down = word[1], word[2], word[3], int(word[5]), int(word[7])
            mb = 4 * 1048576 / 1e6
            rate = lambda ms: f"{mb / (ms / 1000):6.2f} MB/s" if ms > 0 else "  failed  "
            print(f"{rnd:>5}  {how:6} {kib:>4} KiB  {rate(up)}  {rate(down)}", flush=True)
        elif word[0] == "DONE":
            break
    conn.close()
    print("back in HBC", hbc.hbc_wait(wii, 120), flush=True)


if __name__ == "__main__":
    main()
