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
- A job starts only when HBC's own menu has answered on TCP 4299 for 20 s in a row. Apps
  that link hbc-reborn's in-app agent (`sdk/hbc_agent`) answer on 4299 too, so the check asks
  for the version: an agent app replies `<version> agent`, and counts as busy. `status` says
  `busy: <app> is running` then. So a Wii busy with anything is left alone, including a test
  someone sent without the queue.
- A job never exits someone else's app: each job gets `WII_BENCH_JOB_START`, and
  `tools/hbc.py` (`run`, `send`, `exit`, and the tests' `exit_app()`) refuses with `Wii busy:
  <app> is running` when the agent app running is older than the job. An app the job started
  itself it still exits. By hand, outside a job, `hbc.py` exits a running app as before.
- A dispatcher keeps the code it started with. After changing `wiibench.py`, the next
  dispatcher runs the new code: the running one exits after 10 idle minutes.
- Oldest job first. A job past its `--timeout` (default 3600 s) is stopped: its own process
  tree, nothing else.
- Each job: `queue/done/<id>.json` (command, times, exit code) and `<id>.log` (its output).

## One queue per workstation, wherever the script lives

This copy in hbc-reborn is the source of truth, on Windows, Linux and macOS alike. A
workstation's queue is kept in one state directory so every project on it sees the same
jobs: `$WII_BENCH_HOME`, else `C:\tools\wii-bench` on Windows and `~/.wii-bench` elsewhere.

Set a workstation up once with `wiibench.py setup --server URL`, run from its hbc-reborn
checkout (see [Setting up each workstation](#setting-up-each-workstation)). `setup` writes `<state dir>/wiibench.py`, a shim that runs this file (the path other projects
call, so there is only ever one dispatcher per workstation), and `<state dir>/server`, the
lease server's URL. It then checks the server and the Wii. It's safe to run again, including
after the checkout moves. `$WII_BENCH_SERVER` overrides the file; `--server ''` means no
server, which leaves the Wii to this workstation alone.

Never point two copies on one workstation at different state directories, and never put a
state directory on a share used by several workstations. The dispatcher lock holds a local
process ID, so either mistake makes two dispatchers. `WII_BENCH_NO_DISPATCH=1` queues without
starting a dispatcher; the unit tests use it.

## Several workstations: the lease server

A job runs on the workstation where it was added, using that machine's build, cwd and
firewall rule for the network log. Workstations take turns at the Wii through a lease
server: `wiibench.py serve`, stdlib Python only, on TCP 4310.

- Before each job, the dispatcher waits in line for the lease (first come, first served
  across workstations). It renews the lease every 15 s while the job runs and hands it
  back afterwards. A workstation with more jobs queued goes to the back of the line, so
  turns alternate. Between turns there's a gap of up to 5 s (the poll interval).
- A lease or a place in line that goes 60 s without renewal is dropped, so a crashed or
  switched-off workstation frees the Wii by itself. After a server restart, nothing is
  granted for 60 s, which gives a job that's still running time to reclaim its lease.
- If the server can't be reached, the dispatcher waits and logs it in `dispatcher.log`
  instead of skipping the lease. `status` shows the holder and the line.
- The HBC-idle check still runs after the lease is granted, so a test someone sent
  outside the queue is still left alone.
- The server has no authentication. Keep it on the LAN.
- Every workstation that uses the Wii must be set up with a server. One running an older
  `wiibench.py`, or none, is held off only by the HBC-idle check.

## Setting up the lease server: from clone to a running container

These steps are for a Linux homeserver (Debian or Ubuntu commands; use your distribution's
package manager elsewhere). Replace `homeserver` everywhere below with the server's host
name or LAN IP.

1. Install Git and Docker with the Compose plugin:

   ```bash
   sudo apt update
   sudo apt install -y git docker.io docker-compose-v2
   sudo systemctl enable --now docker
   ```

   Docker's own packages work as well (<https://docs.docker.com/engine/install/>). If
   `docker compose version` fails, the Compose plugin is missing; see step 4's fallback.

2. Clone the repository:

   ```bash
   git clone https://github.com/Monsterray/hbc-reborn.git ~/hbc-reborn
   cd ~/hbc-reborn/tools/wii-bench
   ```

3. Build and start the container. `compose.yaml` publishes TCP 4310 and restarts the
   container with Docker, including after a reboot:

   ```bash
   sudo docker compose up -d --build
   ```

4. Without the Compose plugin, the same thing with plain Docker:

   ```bash
   sudo docker build -t wii-bench-lease .
   sudo docker run -d --name wii-bench --restart unless-stopped -p 4310:4310 wii-bench-lease
   ```

5. Let the workstations reach port 4310, if the server has a firewall:

   ```bash
   sudo ufw allow 4310/tcp
   # or, with firewalld:
   sudo firewall-cmd --permanent --add-port=4310/tcp && sudo firewall-cmd --reload
   ```

6. Check that it's running. The log line is `wii-bench lease server on 0.0.0.0:4310, ttl 60 s`,
   and an idle server answers `{"holder": null, "waiters": []}`:

   ```bash
   sudo docker compose ps
   sudo docker compose logs
   curl http://localhost:4310/status
   ```

   With plain Docker, use `sudo docker ps --filter name=wii-bench` and
   `sudo docker logs wii-bench`. From a workstation, `curl http://homeserver:4310/status`
   should give the same answer.

7. To update it after a change to `wiibench.py`:

   ```bash
   cd ~/hbc-reborn && git pull && cd tools/wii-bench && sudo docker compose up -d --build
   ```

   After a restart, the server grants nothing for 60 s so a running job can reclaim its lease.

To run it without Docker, on any OS, use `python3 tools/wii-bench/wiibench.py serve` under a
supervisor (a systemd unit, launchd, or a Windows scheduled task at startup). `--host` and
`--port` change where it listens.

## Setting up each workstation

Every workstation that queues bench jobs needs Python 3.8 or newer, Git, a clone of
hbc-reborn, the Wii (192.168.8.213) on its LAN, and inbound TCP 4300 open for the network
log. The queue itself needs nothing else. hbc-reborn's own test jobs also need the toolchain;
see "Building" in the main [README](../../README.md). Replace `homeserver` with the lease
server's name or IP.

The last two commands on each OS are a check: `status` should show the lease as `free` (or
held by another workstation). The test job sends nothing to the Wii, but it waits for its
turn and for HBC to be idle for 20 s like any other job, so it exercises the whole path.

### Windows (PowerShell)

```powershell
winget install --id Git.Git -e
winget install --id Python.Python.3.12 -e
# open a new terminal so git and python are on PATH
git clone https://github.com/Monsterray/hbc-reborn.git C:\projects\hbc-reborn
cd C:\projects\hbc-reborn
python tools\wii-bench\wiibench.py setup --server http://homeserver:4310
```

In an administrator PowerShell, allow the network log in:

```powershell
New-NetFirewallRule -DisplayName "wii-bench netlog" -Direction Inbound -Protocol TCP -LocalPort 4300 -Action Allow
```

Check:

```powershell
python C:\tools\wii-bench\wiibench.py status
python C:\tools\wii-bench\wiibench.py wait (python C:\tools\wii-bench\wiibench.py add --name hello -- python -c "print('hi')")
```

The state directory is `C:\tools\wii-bench`, and other projects call
`C:\tools\wii-bench\wiibench.py`.

### Linux (bash)

```bash
sudo apt install -y git python3            # Fedora: sudo dnf install -y git python3
git clone https://github.com/Monsterray/hbc-reborn.git ~/projects/hbc-reborn
cd ~/projects/hbc-reborn
python3 tools/wii-bench/wiibench.py setup --server http://homeserver:4310
```

If a firewall is on, allow the network log in:

```bash
sudo ufw allow 4300/tcp
# or, with firewalld:
sudo firewall-cmd --permanent --add-port=4300/tcp && sudo firewall-cmd --reload
```

Check:

```bash
python3 ~/.wii-bench/wiibench.py status
python3 ~/.wii-bench/wiibench.py wait "$(python3 ~/.wii-bench/wiibench.py add --name hello -- python3 -c "print('hi')")"
```

The state directory is `~/.wii-bench`, and other projects call `~/.wii-bench/wiibench.py`.

### macOS (zsh)

```bash
xcode-select --install                     # Git and python3; or: brew install git python@3.12
git clone https://github.com/Monsterray/hbc-reborn.git ~/projects/hbc-reborn
cd ~/projects/hbc-reborn
python3 tools/wii-bench/wiibench.py setup --server http://homeserver:4310
```

If the application firewall is on (System Settings > Network > Firewall), allow Python to
accept the network log:

```bash
PY="$(python3 -c 'import os, sys; print(os.path.realpath(sys.executable))')"
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "$PY"
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "$PY"
```

Check:

```bash
python3 ~/.wii-bench/wiibench.py status
python3 ~/.wii-bench/wiibench.py wait "$(python3 ~/.wii-bench/wiibench.py add --name hello -- python3 -c "print('hi')")"
```

The state directory is `~/.wii-bench`, and other projects call `~/.wii-bench/wiibench.py`.

### On every OS

- Re-run `setup` after moving the checkout, or with `--server http://newhost:4310` to change
  servers. `$WII_BENCH_SERVER` overrides the saved URL for one shell.
- A job's command runs on the workstation that queued it, so write it for that OS
  (`python` or `python3`, and its paths).
- If `status` says the lease server is unreachable, check `curl http://homeserver:4310/status`
  from that workstation, and the server's firewall (step 5 above).

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
