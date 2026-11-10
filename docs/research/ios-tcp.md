# IOS networking: what HBC has measured on real Wiis

Research note for a future networking cIOS (or an IOS-side fix). It collects what
HBC's tools and tests found about the stock IOS TCP/IP stack on real Wiis, with
the evidence behind each point. Dolphin uses the PC's sockets, so none of this
shows up there; every finding below is from real hardware.

Status: the port-4299 outage (section 1) is under investigation (2026-11-10).
Update this note as results come in.

Interim results (2026-11-10, second session; to be folded into section 1):
- A hammer of `HBCV` requests every 50 ms with no popup reproduces an outage
  (devwii: after 56 requests; later runs 1-6 outages per 360-400 s on both
  Wiis), so the popup is not needed. At 2 requests a second it still happened
  (devwii 1 outage in 780 requests, lrwii 4 in 558).
- Letting the PC close first (`tcp_close` waits 250 ms for the peer's FIN
  before `shutdown`) did not help; the PC's netstat showed hundreds of its own
  TIME_WAIT sockets even with the old close, so the Wii was not the active
  closer. The TIME_WAIT-on-the-Wii idea is not supported. A backlog of 8 did
  not help either.
- A connection from another source port (WSL) is silent too during an outage:
  the whole port is dark.
- In the ZIP loop, every outage (11 of 11 in the logs) came right after a
  cycle whose result was "launched" instead of "declined". That is not a
  launch: `tools/hbc.py upload_outcome` calls 10 s of silence "launched", and
  HBC's uptime keeps counting through it (net_starts stays 1). That cycle is
  the outage's start as the PC sees it, not its cause.
- The PC also held 1-6 sockets in FIN_WAIT_2: the Wii acknowledged the PC's
  FIN but never sent its own. A connection IOS never finishes closing is a
  candidate leak.
- In the hammer, single connects often time out and recover at once every
  6-7 s before an outage starts.
- Polling slower (every 0.5 s, 5 s timeout; `hbc.py` since f90b522) made the
  ZIP loop's outages rare (1 in 49-59 cycles) but not gone, and the hammer
  shows a lower rate does not prevent them. Cause still open; next step is a
  packet capture (`pktmon`, elevated) during an outage.
- Script: [tests/wii/wii_port_outage.py](../../tests/wii/wii_port_outage.py).

## 1. The TCP stack stops answering for about 2.5 minutes

**Symptom.** HBC's listener on TCP port 4299 stops answering for 108-162 s
(once over 5 minutes). Then it answers again in the same HBC process, which
keeps drawing and running the whole time.

**What the PC sees during an outage.** A new connection stays in SYN_SENT: the
Wii never sends a SYN-ACK. An older connection stays in FIN_WAIT_1: the Wii
never acknowledges the PC's FIN. ICMP ping keeps working.

**What HBC sees.** Instrumented builds showed:
- HBC's network thread keeps waking about 60 times a second and calling
  `net_accept`, which returns `-EAGAIN`. No connection ever arrives.
- No single IOS network call was slow. The longest was 3.6 s (a write) before
  any change and 0.5 s after the change in "Tried", so the thread was never
  stuck inside IOS.
- HBCS reports these values as `net_longest` and `accepts`
  ([devnet.md](../devnet.md), 1.16.1).

So the TCP side of IOS stops processing SYNs and FINs, while ICMP, and IOS's
answers to HBC's own calls, continue.

**When it happens.**
- On both bench Wiis (devwii and lrwii), with the 1.16.0 release and with
  develop. It is not one console and not a recent HBC change.
- A loop reproduces it about once every 5-8 cycles, at roughly 12 s per cycle.
  Each cycle sends a ZIP without `--yes` and lets HBC's 10 s "Extract the
  received ZIP file?" question time out. Meanwhile `tools/hbc.py` polls HBCS,
  which opens a new TCP connection every 0.2 s with a 2 s timeout.
- Outages start at a random moment during the question, not at its end.
- They came after roughly 150-300 accepted connections.
- Once, it followed an app send, with no ZIP involved.

**Durations.** 108-162 s, with 162 s recurring. That is close to a TCP
retransmit or TIME_WAIT-scale timer (about 2xMSL, or a backed-off retransmit
series 1+2+4+...+64 s ≈ 190 s). This is unconfirmed.

**Leading hypothesis (untested).** IOS runs out of TCP control blocks (PCBs).
HBC's `tcp_close` (channel/channelapp/source/tcp.c) half-closes first with
`shutdown(WR)` so a queued reply is not lost to a reset (see 3 below). That
makes the Wii the active closer, so every short HBCS request leaves a
TIME_WAIT PCB on the Wii. Fast polling would then use up the pool. When the
pool is empty, new SYNs and FINs for any connection are dropped, and the
stack recovers as the TIME_WAIT entries expire.

**Tried.**
- `tcp_close` waits for the PC's FIN in 10 ms slices, and the listen backlog
  went from 3 to 8. Result: the outage still came, 5 times in 26 cycles.
  Requests do get answered faster.
- A bounded (non-blocking) reply send was not tried, because no network call
  was ever stuck.

**Open tests.**
- Slower PC polling (every 0.5 s, 5 s timeout) on the old HBC.
- Rapid HBCV requests (every 50 ms) with no popup, to see whether connection
  rate alone causes the outage.
- Letting the PC close first. HBC would wait for EOF instead of calling
  `shutdown`, and `hbc.py` would close as soon as it has the whole reply.
- A packet capture during an outage. `pktmon` needs an elevated prompt on the
  PC.

**What a networking cIOS could do.**
- Expose the PCB/socket pool size and its use, so a client can see exhaustion.
- Shorten TIME_WAIT, or recycle TIME_WAIT PCBs when the pool runs out.
- Support SO_LINGER {1,0}, or an equivalent, for an orderly abortive close.
- Keep the rest of the TCP stack serving established connections when the
  listener's pool is full.
- Check whether any of these exist in d2x or other cIOS network modules, and
  which IOS module (the IP stack in `/dev/net/ip/top`) holds the pool.

## 2. A non-blocking send can claim more than it queued

On a non-blocking socket whose send buffer fills (seen at the 4 KiB mark),
`net_write` can report a whole block as sent after IOS queued only part of it.
HBC therefore sends replies on a blocking socket (`devfile_send_all`,
channel/channelapp/source/devfile.c). Dolphin does not show this.

## 3. Closing a socket resets it and drops queued data

`net_close` on IOS sends a reset, which drops any reply still in the send
buffer. HBC half-closes first and drains for up to 1 s (`tcp_close`). That
choice makes the Wii the active closer; see 1.

## 4. `net_poll` for POLLOUT waits the whole timeout

On a real Wii, a poll for "writable" can wait out its entire timeout even when
the socket is writable (Dolphin returns at once). HBC polls in 10 ms slices
instead.

## 5. Small listen backlog, and accepts tied to the UI

HBC listened with a backlog of 3. Its network thread accepts only when the UI
loop signals it (`loader_signal_threads`, loader.c), once per frame:
- Popups such as `show_message_timed`, the options dialog, the receive
  progress and the Mii pickers do signal it.
- The progress loops in manage.c and update.c, and the fades, do not, so no
  connection is accepted while they run.

This is HBC's own design, not IOS's, but it decides how hard the stack is
pushed. It explains slow answers while a popup is up (about 1 s per request);
it does not explain the outage.

## Evidence

The bench job logs (2026-11-10) are in the session scratchpad and are not kept
in the repo. The main ones:
- the instrumented loop on devwii: 39 cycles, 5 outages;
- the build that recorded the network thread's activity during an outage;
- the first fix on devwii: 26 cycles, 5 outages;
- the old HBC on lrwii: 15 cycles, 4 outages of 162 s.

The loop script will be added to tests/wii once the fix settles.
