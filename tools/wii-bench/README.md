# wii-bench: one queue for the Wii on the bench

The Wii at 192.168.8.213 (`$WII_BENCH_IP`) sits in the Homebrew Channel. Only one thing can
use it at a time, and several agents and projects want it, so every test goes through
`wiibench.py`:

```bash
python tools/wii-bench/wiibench.py add --name "HBC devnet" --cwd . -- \
    python tests/wii_devnet.py
python tools/wii-bench/wiibench.py wait <id>     # blocks; prints the log tail; exits with the job's code
python tools/wii-bench/wiibench.py status        # Wii free/busy, dispatcher, running, pending, last done
python tools/wii-bench/wiibench.py cancel <id>   # only a job that has not started
python tools/wii-bench/wiibench.py report        # the last 12 h: every workstation's jobs, waits, trouble
bash tools/wii-bench/monitor.sh                  # live: the queue, every workstation, every error
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
- Oldest job first. A job past its `--timeout` (default 1800 s) is stopped: its own process
  tree, nothing else. Pass a longer `--timeout` for a long job; the slowest 1% of the jobs
  so far took about 2,100 s.
- Each job: `queue/done/<id>.json` (command, times, exit code, agent, `hbc_back_s`) and
  `<id>.log` (its output).
- **The queue never touches the Wii during a run.** It probes HBC only between jobs, while
  this dispatcher holds the Wii and nothing runs. `status` and `setup` don't probe when a job
  runs here or another workstation holds the lease; they say who has the Wii instead
  (`in use: HOST has it for NAME (not probed)`). A busy Wii found between jobs (an app run
  outside the queue) is probed only every 15 s.
- **After each job, the dispatcher checks that HBC came back,** still holding the Wii, for up
  to 120 s. The job's record gets `hbc_back_s`. If HBC doesn't come back, the record gets
  `wii_left` (`busy: <app> is running`, or `busy or off`), the chain ends, and the release
  tells the lease server: `status` everywhere shows `left  HOST's NAME left the Wii ...` until
  a job ends with HBC back, and the next holder does the full idle wait.
- `wait` notices a finished job within 0.5 s. Once a minute it prints why the job hasn't
  finished yet on stderr, for example `next here; waiting for the lease: place 2 in line,
  held by HOST for NAME`, `Wii busy: WiiStation is running for 12 min`, or `running`.
  `--every S` changes the interval.

### The idle wait, and back-to-back jobs

The full 20 s idle wait is there for someone using the Wii outside the queue. When the queue
itself just used the Wii, it's shorter:

| Before the job | Idle wait |
| --- | --- |
| Nothing known about the Wii (the dispatcher's first job, or a long gap) | 20 s |
| Up to 60 s after a queue job ended: this workstation's, or the lease's last holder released it | 2 s |
| The same agent's next job, stacked in the same turn | one answer from HBC |

Any probe that finds the Wii busy or off goes back to the full 20 s.

### Receive order

Jobs run in the order they were received, across every workstation. `add` tells the lease
server about a job at once, and the server numbers each job as it first hears of it. Then:

- The Wii goes to the workstation with the earliest-received job.
- A workstation keeps its turn while its next job is also the next one received: jobs
  received one after the other **stack up** and run back to back. The turn ends as soon as
  another workstation's job came in first. With a1 and a2 on one workstation and then b1 on
  another, the order is a1, a2, b1. With a1, then b1, then a2, it is a1, b1, a2.
- With nothing queued anywhere, the turn is held up to 15 s for this workstation's next
  `add` (the usual `add`, `wait`, `add`). It's let go within about a second of another
  workstation's job arriving.
- Without a lease server, this workstation's queue runs oldest first.

A job's agent is `add --agent NAME`, else `$WII_BENCH_AGENT`, else the Claude Code session
(`$CLAUDE_CODE_SESSION_ID`). It decides only the idle wait between stacked jobs: one answer
from HBC for the same agent's next job, 2 s for anyone else's.

A dispatcher or server before 1.10.2 can't number jobs. With an old server, the turns go one
job each while anyone waits; an old dispatcher counts as one job, received when it joined
the line. One long job still has the Wii for its whole length.

### Cancelling and stopping, anywhere

```bash
python tools/wii-bench/wiibench.py cancel ID [ID...]       # here, or on another workstation
python tools/wii-bench/wiibench.py cancel --agent AGENT     # every queued job of an agent
python tools/wii-bench/wiibench.py cancel --host HOST       # ... of a workstation
python tools/wii-bench/wiibench.py cancel --all             # every queued job, everywhere
python tools/wii-bench/wiibench.py stop ID                  # a running job: ends like a timeout
```

The monitor's manage page does the same with a click and a key, and asks `y` first: click a
job (or `j`/`k`, or the arrow keys), then `c` cancels it (or stops it, when it runs), `a`
cancels its agent's, `w` its workstation's, `X` everything queued, `g` gives up waiting for a
frozen Wii.

- A job on another workstation is cancelled by that workstation's dispatcher. The lease server
  hands the request on with the dispatcher's next call, within a few seconds, and drops the job
  from every list at once.
- A cancelled job leaves a record with exit `cancelled`: an agent waiting on it gets
  "cancelled ..." and exit 1, not a job that vanished. It is not counted as a failure.
- A stopped job ends like a timeout (`stopped`). Then HBC is checked and the job's data is
  collected as after any job.
- `add` refuses a command whose program is not there (not on PATH, or a path that does not
  exist from `--cwd`): it would only fail at once on the Wii's time. `--no-check` queues it
  anyway, for a program a build makes before the job runs.
- A job given no `--name` is named after its script and first argument (`wii_lab.py fmt_hw`),
  not its interpreter. Until 1.10.4 that was `python.exe`, and an agent could not tell its own
  jobs. Name them anyway, and the manage page also shows each job's agent and project folder.

### When the Wii freezes

After a job, the dispatcher checks that HBC came back. When it did not, what happens depends
on what still answers:

| After the job | The queue |
| --- | --- |
| HBC | as normal: collects the job's data, and the next job goes |
| The app's agent (`sdk/hbc_agent`): the app is stuck, the agent thread is not | saves `freeze/agent/` in the job's run folder (status, the app's output so far, a picture of the screen), and collects `--collect` paths through the agent |
| Nothing: a hard freeze, or the Wii is off | keeps the Wii and waits. When someone resets or power-cycles the Wii and HBC runs again, it saves `freeze/after/` (HBC's kept log and crash report, which a RESET keeps and a power cut loses), and collects the job's Wii paths. Only then does the next job go |

The monitor shows such a wait in red on the manage page, and the errors page says what
happened.

Two more things the errors page (and `report`) flags:

- **Outside the queue:** a turn found the Wii out of HBC, although the last queue job left it
  there. Someone used the Wii without the queue: a person, or an agent's `hbc.py run` straight
  to the Wii. The entry names the job that last left the Wii in HBC. HBC does not record who
  sent an app, so that is as close as the queue can get. Queue everything that uses the Wii.
  It takes two probes in a row without HBC's answer: one alone (a probe that timed out) only
  restarts the idle wait. Events from before this rule count when they lasted longer than one
  probe interval (15 s).
- **Lease gap:** the dispatcher could not renew its lease for over 30 s during a job (the
  job's record says how long). Past 60 s the server lets the lease go, and another workstation
  could get the Wii mid-run. `dispatcher.log` says why the renewals failed. `stop ID`, or `g` on the manage page, gives the wait up. After a hard freeze only
what is already on the PC, or written to the SD card and flushed, survives. So an app that
may freeze should:

- stream what matters to the PC as it happens: the agent's network log
  (`sdk/hbc_netlog.h`), or the job's own TCP link, as WiiStation's `lab_net.c` does;
- close or `fsync` its log files on the SD card often, not only at exit;
- arm `__exception_setreload(10)` and the agent's hang watchdog, so a crash or a hang comes
  back to HBC on its own, and the agent keeps the log.

### Each job's data, kept

Every job gets its own run folder, `<state dir>/runs/<id>/`, named in `$WII_BENCH_OUT`. No
other run writes there, so agents can queue as many runs as they like and fetch the data
whenever they want.

- **Write into it** when the job can: `$WII_BENCH_OUT/whatever`.
- **`add --collect PATH`** (repeatable) copies data the job left elsewhere into the run
  folder. It runs after the job and before the next job gets the Wii, so the next run
  can't overwrite it:
  - `sd:/...` or `usb:/...`: from the Wii (a file or a folder), into `wii/`. This needs HBC
    back after the job.
  - Any other path: from this workstation, relative to `--cwd`, into `local/`. Use it for a
    fixed output folder such as `.bench/out`.

  A path that can't be collected is recorded in the job's record (`collected`) and warned
  about on the monitor's errors page.

```bash
python tools/wii-bench/wiibench.py add --name "chain 5" --cwd . \
    --collect sd:/wiisxrx/lab.log --collect .bench/out -- python scripts/run_chain.py 5
python tools/wii-bench/wiibench.py runs --mine            # this agent's jobs, newest first
python tools/wii-bench/wiibench.py results ID             # record, collected data, the log's end
python tools/wii-bench/wiibench.py results ID --to DIR    # copy the run folder, job.json, job.log
```

**Rollover.** A finished job older than 14 days, or the oldest jobs while the live runs pass
20 GB, is packed into `runs/archive/YYYY-MM/<id>.tar.gz`: its record, log and run folder.
`runs/archive/index.jsonl` lists everything there. `runs` still shows archived jobs, and
`results ID --to DIR` unpacks one. The oldest archived months are dropped once the archive
passes 100 GB. The dispatcher rolls over at most every 10 minutes, and `wiibench.py rollover`
does it at once. The limits are `WII_BENCH_KEEP_DAYS`, `WII_BENCH_RUNS_MAX_GB` and
`WII_BENCH_ARCHIVE_MAX_GB`, read by the dispatcher.

### The history and `report`

The lease server writes every event as one JSON line: a workstation joining the line or
leaving it, each grant (with how long it waited), each release (how long it held the Wii,
and whether HBC was back), expiries, and each job's record (sent by its dispatcher: times,
exit, agent, `hbc_back_s`, whether it was chained). Each dispatcher also keeps its own jobs in
`<state dir>/history`.

| Where | What |
| --- | --- |
| `history/events-YYYY-MM-DD.jsonl` | today's and the last 7 days' events (UTC days) |
| `history/events-YYYY-MM-DD.jsonl.gz` | older days of the current month, gzipped |
| `history/archive/YYYY-MM.tar` | the deep archive: each finished month's gzipped days |

Rotation, compression and archiving happen at the server's start and at its first event each
day. Archives are kept forever; `KEEP_MONTHS` in `compose.yaml` (`serve --keep-months N`) keeps
only the last N months. On the homeserver the history is `tools/wii-bench/history/`, outside
the image, so a rebuild or restart keeps it; back that folder up like any other.

```bash
python tools/wii-bench/wiibench.py report                     # the last 12 h, every workstation
python tools/wii-bench/wiibench.py report --days 7
python tools/wii-bench/wiibench.py report --since 2026-09-01  # local time; reads the archives too
python tools/wii-bench/wiibench.py report --local             # this workstation's own jobs
python tools/wii-bench/wiibench.py report --json              # the events themselves
```

`report` shows the Wii's use by queue jobs, a line per workstation (jobs, failed jobs, Wii
minutes, turns, median and longest wait for the Wii), how fast hand-overs were, the longest
waits, and anything that needs a look: a Wii left out of HBC, an expired lease, a timeout.

### The monitor

`monitor.sh` is a live dashboard of the same facts, redrawn in place every 3 s:

| Page | Shows |
| --- | --- |
| 1 queue | a card per Wii (below), the jobs running here, every workstation waiting for the Wii, every job queued on every workstation in the order the turns will come, and the last jobs here (exit, seconds, HBC after the job, chained or not) |
| 2 errors | every problem in the window, newest first: failed and timed-out jobs with their log's last line, a Wii left out of HBC, an expired lease, a workstation that left the line, the lease server unreachable, a dispatcher that crashed, jobs queued with no dispatcher, a workstation still on a dispatcher before 1.9.1 |
| 3 history | a lane per Wii over the window (below); per workstation: jobs, failures, Wii minutes, turns, median and longest wait; hand-over speed; the longest waits |
| 4 log | the end of this workstation's `dispatcher.log`, problems in colour |
| 5 manage | every job running, waiting or queued, on every workstation, with its agent and project folder; cancel or stop one, an agent's, a workstation's or all; give up waiting for a frozen Wii |

The two lines under the tabs, on every page:

- **Every Wii on one line**, problems first:

  ```text
  ▲ prod waiting 6m40s for HBC: Wii busy or off  │  ▶ dev vbagx tests (12) 3m12s/30m00s +2 queued  │  ● lrwii free (pinned)
  ```

  `✖` left out of HBC, `▲` waiting for HBC, `▶` running, `●` free, `○` the lease server
  doesn't answer. When the line doesn't fit, each Wii gets a shorter form (`▲ prod HBC? 6m40s`),
  then only its times (`▲ prod 6m40s  ▶ dev 3m12s +2  ● lrwii`).
- Whether the lease server answers, what this dispatcher is doing, and how many errors and
  warnings the window holds.

**The cards** (queue page) give one box per Wii, as many across as the terminal fits (three
need about 140 columns; fewer stack). Each card shows:

- its name and address, its state, and for how long;
- the job (its name wraps, up to three lines), its id, and the workstation and agent that
  queued it;
- a progress bar against the job's `--timeout` (for a job running here), or what the Wii is
  doing instead;
- the lease: held how long, renewed how recently;
- the next two jobs that may run on it;
- the last job on it: exit, and the HBC it came back to (a stock HBC shows here);
- the window's jobs, failures and how busy the Wii was;
- at the bottom, notes: whether it takes any job or pinned jobs only, and its network-log port.

Cards side by side keep the same height: a shorter one gets blank lines above its notes.

**The lanes** (history page) give one line per Wii across the window, oldest on the left:

- `█` green for a job that went well, red for one that failed or a Wii held after a freeze,
  cyan for a job running now;
- `░` yellow for a turn waiting for HBC;
- `⚑` for use outside the queue;
- `·` for idle.

A block long enough gets its job's name underneath (a freeze says how long the Wii was held).
On the right: jobs, failures, how busy the Wii was, and the longest wait for it. Each cell
covers the window divided by the terminal's width (24 h on 150 columns: about 13 minutes).

The HBC column is the dispatcher's check after each job: `ok` when HBC answered as the job
ended (most jobs wait for HBC themselves before they end), `12s` when it came back that much
later, `LEFT` (red) when it did not come back within 120 s, and `-` for a job from before the
check. A failed job's detail is the last line of its log that reads like an error, else its
last line. A lease server that does not answer is given 2 s, then left alone for 15 s, so a
homeserver that is off does not stall the dashboard.

**It never touches the Wii.** Everything comes from this workstation's queue files and
`dispatcher.log` and from the lease server, so a dashboard left open all day can't disturb a
run. The Wii's state shown is what the queue last recorded.

```bash
bash tools/wii-bench/monitor.sh                      # 1-4 or a click: page; q: quit
bash tools/wii-bench/monitor.sh 5 --page=errors --hours=72
bash tools/wii-bench/monitor.sh --once --page=errors # one frame, for a script or a quick look
bash tools/wii-bench/monitor.sh --filter=errors:Wii64 --sort=done:secs:desc
bash tools/wii-bench/monitor.sh --help
```

- Click a column header to sort by it (ascending, descending, off), and click a cell to copy
  its full value (a job id, a whole error message) to the clipboard. Clicks need a terminal
  with mouse reporting: Windows Terminal, mintty, iTerm2, most Linux terminals.
- **Windows:** run it from Git Bash, or from PowerShell with
  `& 'C:\Program Files\Git\bin\bash.exe' tools/wii-bench/monitor.sh`.
- **Linux:** any bash 4.3 or newer.
- **macOS:** the system bash is 3.2, too old: `brew install bash`, then
  `"$(brew --prefix)/bin/bash" tools/wii-bench/monitor.sh`. The monitor says so if started
  with the old one.
- It needs Python 3.8+ (as `wiibench.py` does), and no `jq`. One long-running
  `wiibench.py snapshot --serve` collects, sorts and filters every refresh: it looks up the
  lease server once and keeps its connection, fetches only new history, and re-reads only job
  files that changed. `snapshot --json` shows what it collects.
- A refresh takes about 110 ms on Git Bash (86 ms of it the snapshot) and about 10 ms on
  Linux. Drawing forks nothing: on Git Bash a fork costs 14-23 ms, and the first version's
  `tput` per line and `$(...)` per cell made a frame take about 4 s.
  `WII_BENCH_MONITOR_TIMES=FILE` appends each frame's collect, build and draw milliseconds.
- Under `timeout`, use `timeout --foreground`: without it the dashboard can't read the
  terminal and stops at its first frame.

`lib/monitor_lib.sh` is the AI server's `/ai/bin/lib/monitor_lib.sh`, copied unchanged
(git `686fcbb`, 2026-09-30) so it can be refreshed with a plain copy. Don't edit it here;
change it on the AI server and copy it again:

```bash
scp ai-server:/ai/bin/lib/monitor_lib.sh tools/wii-bench/lib/monitor_lib.sh
```

## One queue per workstation, wherever the script lives

This copy in hbc-reborn is the source of truth, on Windows, Linux and macOS alike. A
workstation's queue is kept in one state directory so every project on it sees the same
jobs: `$WII_BENCH_HOME`, else `C:\tools\wii-bench` on Windows and `~/.wii-bench` elsewhere.

Set a workstation up once with `wiibench.py setup --server URL`, run from its hbc-reborn
checkout (see [Setting up each workstation](#setting-up-each-workstation)). `setup` writes
`<state dir>/wiibench.py`, a shim that runs this file (the path other projects call, so
there is only ever one dispatcher per workstation), and `<state dir>/server`, the
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
  turns alternate, apart from a same-agent chain (above).
- Waiting in line is a long poll: the server holds each `/acquire` open for up to 25 s and
  answers as soon as the lease is granted, so a hand-over takes well under a second. An
  older server answers at once, and the client then asks again every 5 s.
- A lease or a place in line that goes 60 s without renewal is dropped, so a crashed or
  switched-off workstation frees the Wii by itself. After a server restart, nothing is
  granted for 60 s, which gives a job that's still running time to reclaim its lease.
- Each dispatcher sends the server its whole queue (up to 50 jobs, with the total) with every
  lease request, renew and release, and `add` and `cancel` send it at once. So `status` and the
  monitor show every workstation's queued jobs, not only the one job each waits with. The
  monitor orders them the way the turns will come: the workstations in line take turns one job
  each, a workstation with more goes to the back after each job, and the one holding the Wii
  rejoins at the back. A list not refreshed for 60 s is dropped. A dispatcher before 1.10.0 sends
  none, and shows as the one job it waits with.
- If the server can't be reached, the dispatcher waits and logs it in `dispatcher.log`
  instead of skipping the lease. `status` shows the holder and the line.
- The HBC-idle check still runs after the lease is granted, so a test someone sent
  outside the queue is still left alone.
- The server has no authentication. Keep it on the LAN.
- Every workstation that uses the Wii must be set up with a server. One running an older
  `wiibench.py`, or none, is held off only by the HBC-idle check.

## Several Wiis

The queue can drive more than one Wii. Each Wii has its own lease, and a job runs on
whichever Wii is free first, unless it names one.

- **The pool** is a list of `name=ip[:port] [pinned]`, separated by commas or newlines, in
  `$WII_BENCH_WIIS`. This bench's, in `compose.yaml`:

  ```bash
  WII_BENCH_WIIS="dev=192.168.8.213, lrwii=192.168.8.243 pinned, prod=192.168.8.200 pinned"
  ```

  | Name | Address | Takes |
  |---|---|---|
  | `dev` | 192.168.8.213 | any job: the bench Wii |
  | `lrwii` | 192.168.8.243 | only `add --wii lrwii`: the living-room Wii, real data |
  | `prod` | 192.168.8.200 | only `add --wii prod`: the everyday Wii, real data |

  - **`pinned`:** a pinned Wii takes only jobs added with `--wii` and its name. A job for
    any Wii never lands there, and never waits for one. The production consoles are on the
    queue so that work there goes one job at a time, waits for HBC to be idle (so a game
    being played is never interrupted), and shows in the monitor. They are not extra test
    Wiis. Pin a job there only when that console is the point of the test, and only when
    asked to.
  - The first Wii in the list gets the jobs of dispatchers from before 1.10.8, so it must
    not be a pinned one.

  - With a lease server, set it on the server (`WII_BENCH_WIIS` in `compose.yaml`, or
    `serve --wii bench=192.168.8.213 --wii second=192.168.8.214`). Every workstation then
    learns the pool from the server, and nothing changes on the workstations.
  - Without a server, set it on the workstation, either in the environment or in a file
    named `wiis` in the state folder, one Wii per line.
  - With no pool set, there is one Wii, named `bench`, at each workstation's
    `$WII_BENCH_IP`, just as before 1.10.8.
- **Choosing a Wii:** `add --wii lrwii` runs a job on that Wii only. `add` refuses a name
  that isn't in the pool. Without `--wii`, the job runs on any free Wii.
- **Order:** a free Wii goes to the earliest-received job that may use it. A job pinned to a
  busy Wii doesn't hold up later jobs that can run on another one.
- **Parallel jobs:** a dispatcher runs one turn per Wii at once. Each turn waits for a Wii
  with a job of its own, runs it, and runs the jobs that follow on that Wii in receive order,
  just as one turn did before. The idle wait, the HBC check, `--collect`, and the wait after
  a freeze all apply to the turn's own Wii. A freeze on one Wii doesn't stop the others.
- **What a job gets:**
  - `WII_BENCH_WII`: the Wii's name.
  - `WII_BENCH_IP` and `WII_BENCH_PORT`: the Wii's address.
  - `HBC_WII` and `HBC_PORT`: the same address, so the job's `tools/hbc.py` calls go to that
    Wii without being told.
  - `WII_BENCH_LOG_PORT`: a TCP port for the network log. It is 4300 for the first Wii,
    4301 for the second, and so on, so two jobs on one workstation never listen on the same
    port. `tools/hbc.py` (and the tests built on it) uses it by default inside a job, so leave
    `--log-port` out of the command. A job that opens its own listener should read
    `WII_BENCH_LOG_PORT` instead of a fixed 4300. Jobs run without a shell, so
    `$WII_BENCH_LOG_PORT` in the `add` command line is not expanded. The firewall rule in
    [Setting up each workstation](#setting-up-each-workstation) opens 4300-4315, enough for
    16 Wiis.
- **Showing it:**
  - `status` and `setup` print a line per Wii, and each job says which Wii it is on.
  - The monitor shows a line per Wii under the tabs, and a WII column for running, queued
    and finished jobs. A job's record and its history event carry `wii`.
  - `dispatcher.log` prefixes each turn's lines with its Wii, for example `[second]`.
- **Older dispatchers:** a dispatcher before 1.10.8 sends no choice of Wii and doesn't know
  about other addresses. The server only ever gives it the first Wii in the pool.

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

3. Build and start the container. `compose.yaml` publishes TCP 4310, keeps the history in
   `tools/wii-bench/history/` (see [The history and `report`](#the-history-and-report)), and
   restarts the container with Docker, including after a reboot:

   ```bash
   sudo docker compose up -d --build
   ```

4. Without the Compose plugin, the same thing with plain Docker:

   ```bash
   sudo docker build -t wii-bench-lease .
   sudo docker run -d --name wii-bench --restart unless-stopped -p 4310:4310        -v "$PWD/history:/data" wii-bench-lease
   ```

5. Let the workstations reach port 4310, if the server has a firewall:

   ```bash
   sudo ufw allow 4310/tcp
   # or, with firewalld:
   sudo firewall-cmd --permanent --add-port=4310/tcp && sudo firewall-cmd --reload
   ```

6. Check that it's running. The log line is
   `wii-bench lease server on 0.0.0.0:4310, ttl 60 s, history in /data (archives kept forever)`,
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
   The history stays in `history/` across rebuilds.

To run it without Docker, on any OS, use
`python3 tools/wii-bench/wiibench.py serve --history DIR` under a supervisor (a systemd unit,
launchd, or a Windows scheduled task at startup). `--host` and `--port` change where it
listens, and `--keep-months N` limits the archives.

## Setting up each workstation

Every workstation that queues bench jobs needs Python 3.8 or newer, Git, a clone of
hbc-reborn, the Wii (192.168.8.213) on its LAN, and inbound TCP 4300-4315 open for the network
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
New-NetFirewallRule -DisplayName "wii-bench netlog" -Direction Inbound -Protocol TCP -LocalPort 4300-4315 -Action Allow
# a rule from before 1.10.8 that opens only 4300: widen it instead (an older setup
# named it "WiiStation bench Wii"; Get-NetFirewallRule -DisplayName "*bench*" finds it)
# Set-NetFirewallRule -DisplayName "wii-bench netlog" -LocalPort 4300-4315
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
sudo ufw allow 4300:4315/tcp
# or, with firewalld:
sudo firewall-cmd --permanent --add-port=4300-4315/tcp && sudo firewall-cmd --reload
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
brew install bash                          # only for monitor.sh: macOS's own bash is 3.2
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

- Send its program itself (Wiiload); the Wii's address is in `WII_BENCH_IP` (and `HBC_WII`,
  which `tools/hbc.py` reads). With [several Wiis](#several-wiis), that is the Wii the job
  got, and its network log port is `WII_BENCH_LOG_PORT`.
- Leave the Wii in HBC when it ends: exit through HBC's reload stub (`exit()` in libogc
  does it when the stub is there), not power-off or the system menu. Arm
  `__exception_setreload(10)` so a crash goes back to HBC too. A Wii left elsewhere stalls
  the queue for everyone until someone brings it back.
- Put its data in `$WII_BENCH_OUT`, or name it with `add --collect`, rather than in a
  folder or SD path every run shares. See "Each job's data, kept".
- Get its results back itself: hbc-reborn's jobs use the developer network log
  (`tools/hbc.py`, `sdk/hbc_netlog.h`) on `$WII_BENCH_LOG_PORT` (4300 for the first Wii);
  WiiStation's `Gamecube/lab_net.c` does the same. The "wii-bench netlog" firewall rule
  allows inbound TCP 4300-4315.

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
