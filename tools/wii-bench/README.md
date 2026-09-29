# wii-bench: one queue for the Wii on the bench

The Wii at 192.168.8.213 (`$WII_BENCH_IP`) sits in the Homebrew Channel. Only one thing can
use it at a time, and several agents and projects want it, so every test goes through
`wiibench.py`:

```bash
python tools/wii-bench/wiibench.py add --name "HBC devnet" --cwd . -- \
    python tests/wii_devnet.py --log-port 4300
python tools/wii-bench/wiibench.py wait <id>     # blocks; prints the log tail; exits with the job's code
python tools/wii-bench/wiibench.py status        # Wii free/busy, dispatcher, running, pending, last done
python tools/wii-bench/wiibench.py cancel <id>   # only a job that has not started
```

- `add` starts the dispatcher if none is running (`dispatcher.lock` holds its PID). It exits
  after 10 minutes with nothing to do.
- A job starts only when HBC has answered on TCP 4299 for 20 s in a row: HBC answers only in
  its menu, so a Wii busy with anything is left alone, including a test someone sent without
  the queue.
- Oldest job first. A job past its `--timeout` (default 3600 s) is stopped: its own process
  tree, nothing else.
- Each job: `queue/done/<id>.json` (command, times, exit code) and `<id>.log` (its output).

## One queue, wherever the script lives

This copy in hbc-reborn is the source of truth. The queue itself is shared state, kept in
one directory so every project and every copy of the script sees the same jobs:
`$WII_BENCH_HOME`, else `C:\tools\wii-bench` on Windows and `~/.wii-bench` elsewhere.

Other projects call `C:/tools/wii-bench/wiibench.py`. On the bench workstation that path is
a shim that runs this file, so there is only ever one dispatcher:

```python
# C:/tools/wii-bench/wiibench.py
import os, runpy, sys
src = os.environ.get("WII_BENCH_SRC", r"C:\projects\hbc-reborn\tools\wii-bench\wiibench.py")
os.environ.setdefault("WII_BENCH_HOME", os.path.dirname(os.path.abspath(__file__)))
sys.argv[0] = src
runpy.run_path(src, run_name="__main__")
```

Never point two copies at different state directories while they share a Wii: that makes
two dispatchers, and two jobs can reach the Wii at once. `WII_BENCH_NO_DISPATCH=1` queues
without starting a dispatcher; the unit tests use it.

## What a job must do

- Send its program itself (Wiiload); the Wii's address is in `WII_BENCH_IP`.
- Leave the Wii in HBC when it ends: exit through HBC's reload stub (`exit()` in libogc
  does it when the stub is there), not power-off or the system menu. Arm
  `__exception_setreload(10)` so a crash goes back to HBC too. A Wii left elsewhere stalls
  the queue for everyone until someone brings it back.
- Get its results back itself: hbc-reborn's jobs use the developer network log
  (`tools/hbc.py`, `sdk/hbc_netlog.h`) on TCP 4300; WiiStation's `Gamecube/lab_net.c` does
  the same. The Windows firewall rule "WiiStation bench Wii" allows inbound TCP 4300.

## The protocol HBC takes (wiiload 0.5)

TCP 4299: `HAXX`, version bytes `0 5`, args length (u16), compressed length (u32),
uncompressed length (u32), all big-endian; then the zlib data; then the arguments,
NUL-separated, the file name first, a final NUL (1024 bytes at most). The program gets them
as argv. A ZIP is not compressed again and is installed to the card instead. hbc-reborn
also answers `HBCV` + 12 zero bytes with its version string, and the rest of its developer
protocol (`docs/devnet.md`); the stock HBC resets those connections.

## Probing HBC: never connect and close

HBC's loader takes one connection at a time, and its listen backlog is 3. On a connection
that sends nothing, the stock HBC's tcp_read treats end of stream as "try again" for
TCP_BLOCK_RECV_TIMEOUT = 10 s (hbc-reborn 1.4.1 ends such a request at once). A bare
connect-and-close every 5 s filled the backlog of a stock HBC: it sat in its menu but timed
out every connection (2026-09-28). Every probe (`hbc_idle` here, `hbc_ready` in
WiiStation's wii_lab.py) sends a 16-byte header with the magic `PING`, which HBC rejects at
once ("invalid upload request", debug console only).
