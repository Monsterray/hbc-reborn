# Changelog

One line per version, from the commit that set it. Findings, measurements and hardware
results for each version are in [docs/history/](docs/history/).

## 1.15 ([details](docs/history/1.15.md))

- **1.15.15** (2026-11-05): slotled audit fixes; its open questions left to contributors
- **1.15.14** (2026-11-05): slotled works without AHBPROT; Wii Settings agrees with its save
- **1.15.13** (2026-11-05): slotled: a flash no longer leaves the light stuck on
- **1.15.12** (2026-11-05): the keyboard's IOS report buffer has a cache line of its own
- **1.15.11** (2026-11-05): queued agent keys are dropped when HOME closes
- **1.15.10** (2026-11-05): a failed tree delete says how far it got
- **1.15.9** (2026-11-05): a listing name cannot leave the folder get -r writes into
- **1.15.8** (2026-11-05): badge widths on a tiny theme tile are clamped
- **1.15.7** (2026-11-05): a network upload waits for the scan before it is added
- **1.15.6** (2026-11-05): a scan marks every app seen whatever the badge setting
- **1.15.5** (2026-11-05): a badge record goes only after two whole scans miss it
- **1.15.4** (2026-11-05): a table that read as missing is not written over the file
- **1.15.3** (2026-11-05): an accept() network reset closes the listener
- **1.15.2** (2026-11-05): a Restart network keeps its watchdog
- **1.15.1** (2026-11-05): the crash-loop guard ends in the Wii Menu on a real Wii
- **1.15.0** (2026-11-05): release; the crash handler leaves by HBC's own stub and shows more

## 1.14 ([details](docs/history/1.14.md))

- **1.14.33** (2026-11-05): slotled check: a second STM handle opens on a real Wii
- **1.14.32** (2026-11-05): slotled saves Slot illumination on a real Wii
- **1.14.31** (2026-11-04): a network hang leaves HBC running; reloading is a developer setting
- **1.14.30** (2026-11-04): slotled's first check on a real Wii
- **1.14.29** (2026-11-03): SDK READMEs: what each changes on the Wii, licences; slotled's open questions
- **1.14.28** (2026-11-03): sdk/slotled, the disc-slot light; the WiiConnect24 SDK's plan
- **1.14.27** (2026-11-02): the keyboard in HOME and the crash screen's reset, on the Wii
- **1.14.26** (2026-11-02): app badge records are locked, read whole or not written
- **1.14.25** (2026-11-02): HBCU answers only a 16-byte query, never a reply from port 4299
- **1.14.24** (2026-11-02): no rm -r pattern in a device root or apps folder; log never guesses
- **1.14.23** (2026-11-02): the USB keyboard drives HOME; rollover and skipped devices
- **1.14.22** (2026-11-02): the crash screen from the first instruction, and once only
- **1.14.21** (2026-11-02): HBCU discovery answers on a real Wii
- **1.14.20** (2026-11-02): a slow IOS cleanup after a cancelled start is not a hang; no unreachable Reinit
- **1.14.19** (2026-11-02): merge PR 22, recover from network starts and cleanups that IOS never finishes
- **1.14.18** (2026-11-02): hbc.py finds a Wii by itself only for commands that read; --timeout=N
- **1.14.17** (2026-11-02): merge PR 21, find Wiis on the network with a UDP query (HBCU)
- **1.14.16** (2026-11-02): no wildcard rm -r at a device's top; the network thread gets 12 KB
- **1.14.15** (2026-11-02): merge PR 20, delete progress, and trees and patterns in one request
- **1.14.14** (2026-11-02): app badges keep their record in one file per device, as theme-coloured pills
- **1.14.13** (2026-11-02): merge PR 17, app badges for new and updated apps
- **1.14.12** (2026-11-02): a remote that goes away is no longer kept sideways
- **1.14.11** (2026-11-02): merge PR 23, a sideways remote stays sideways through brief upright readings
- **1.14.10** (2026-11-02): the docs say what PR 19 shows; TODO.md for later work
- **1.14.9** (2026-11-02): merge PR 19, the folder for every transfer
- **1.14.8** (2026-11-02): the USB keyboard types, and never holds up an exit
- **1.14.6** (2026-11-02): merge PR 15, a USB keyboard as a Wii Remote
- **1.14.5** (2026-11-02): the stick scrolls from every GameCube port too
- **1.14.4** (2026-11-02): merge PR 13, all four GameCube ports
- **1.14.3** (2026-11-02): the crash screen shows its text again
- **1.14.2** (2026-11-02): the Wii Remote queue wrap returns void, and never leaves a queue at 0
- **1.14.1** (2026-11-02): merge PR 14, a fourth Wii Remote or a Balance Board no longer crashes the channel
- **1.14.0** (2026-11-02): release; sideways remotes, transfers you can follow, packed DOLs again

## 1.13 ([details](docs/history/1.13.md))

- **1.13.16** (2026-11-02): packed DOLs start again: the "sc" handler is back (fixes #12)
- **1.13.15** (2026-11-02): the network tries again by itself after a failed start
- **1.13.14** (2026-11-02): the turn icon sits at the top right, half the size
- **1.13.13** (2026-11-02): an icon shows which way a turning remote is read
- **1.13.12** (2026-11-02): Sideways remote in HOME's Controller settings
- **1.13.11** (2026-11-01): Wii Remote icon drafts, kept for later
- **1.13.10** (2026-11-01): D-pad repeat and Grid edges: Turn page in the app list
- **1.13.9** (2026-11-01): sdk/mboard puts MEM2_PROT back after the play log
- **1.13.8** (2026-11-01): the Wiiload receive dialog names the file
- **1.13.7** (2026-11-01): hbc.py put and sync read each upload back
- **1.13.6** (2026-11-01): a develop branch; pushes run CI on master only
- **1.13.5** (2026-11-01): AGENTS.md: compute numbers with a tool, re-check before acting
- **1.13.4** (2026-11-01): HBCS reports Options, for the progress test on the Wii
- **1.13.3** (2026-11-01): a progress circle for transfers, in Options; a USB keyboard as a Wii Remote
- **1.13.2** (2026-11-01): transfer labels show the app folder; Wii groups, and the manager pins, unpins and regroups; a freeze marker no longer outlives its dispatcher; a fourth Wii Remote or a Balance Board no longer crashes the channel
- **1.13.1** (2026-11-01): Dolphin test runs delete their profiles; read all four GameCube ports
- **1.13.0** (2026-11-01): release; the Mii picker, and HBC built on libogc 3.1.0 again

## 1.12 ([details](docs/history/1.12.md))

- **1.12.9** (2026-10-29): CI builds with libogc 3.1.0, as the bench PC does
- **1.12.8** (2026-10-29): a WAD installer for the bench, and the exit to the System Menu in Dolphin
- **1.12.7** (2026-10-26): Dolphin tests refuse to start while another Dolphin holds port 4299
- **1.12.6** (2026-10-29): the shadow on the floor for every height and build
- **1.12.5** (2026-10-29): Z writes on at the copy, and the effects API's gaps
- **1.12.4** (2026-10-29): a party of Miis, to find how many the Wii can draw
- **1.12.3** (2026-10-29): the audit after the sliver
- **1.12.2** (2026-10-29): the Mii picker and playground, effects, and the whole Mii
- **1.12.1** (2026-10-28): the levels where they look right, and the hair whole
- **1.12.0** (2026-10-27): release; Miis by the hundred, and the SDK's audit

## 1.11 ([details](docs/history/1.11.md))

- **1.11.10** (2026-10-27): levels by likeness, a fourth level, and the hardware doing the drawing
- **1.11.9** (2026-10-27): coarser heads, faces drawn by the GPU, the skin in one colour
- **1.11.8** (2026-10-27): levels of detail from the user's own body, and a crowd walking in and out
- **1.11.7** (2026-10-27): packed parts, shared parts, a pose worked out once
- **1.11.6** (2026-10-27): a Mii keeps only what it draws
- **1.11.5** (2026-10-27): what a Mii costs: crowd tests on the bench Wii, and a Mii made through the SDK
- **1.11.4** (2026-10-27): the avatar out of its wave without a jump
- **1.11.3** (2026-10-27): sdk/mii 2.0 step 1: the Mii Channel body skinned on the GPU, and cross-fades
- **1.11.2** (2026-10-27): the plan for sdk/mii 2.0 (docs/mii-sdk-2.md)
- **1.11.1** (2026-10-27): the Mii's face whole on a real Wii; bump a turn that waits for HBC
- **1.11.0** (2026-10-26): release; the Mii avatar and SDK, the bounded way out, the GX FIFO in MEM1

## 1.10 ([details](docs/history/1.10.md))

- **1.10.26** (2026-10-25): the Mii Channel's own bodies, and HBC's GX FIFO back in MEM1
- **1.10.24** (2026-10-25): the way out never hands a wedged IOS to the stub, and shows itself
- **1.10.22** (2026-10-25): the agent links without f_chmod and refuses a stub it can't run
- **1.10.21** (2026-10-24): Miis drawn (sdk/mii/miidraw) and HBC's avatar
- **1.10.20** (2026-10-24): HOME > HBC > Save Mii faces
- **1.10.19** (2026-10-24): way-out hardware results; Mii rendering research and probe
- **1.10.18** (2026-10-24): the way out, reviewed and fixed
- **1.10.16** (2026-10-24): audit of rename, move and attributes
- **1.10.15** (2026-10-24): every FAT attribute, read and set, as a normal FS shows them
- **1.10.14** (2026-10-24): release; the agent leaves a wedged IOS without a power cycle; rename and move (HBCF op R, hbc.py mv); the way out proven on the bench Wii; STM and refused-launch resets on hardware, BootMii note; wii_way_out.py's TV recorder no longer holds the job's log
- **1.10.13** (2026-10-24): release; sdk/mii audit, robust and easy to use; the monitor shows each Wii as a line, a card and a lane; "used outside the queue" needs two missed probes; Wii cards wrap the job's name and keep their notes at the bottom; a Wii can lack a part; dev lacks a disc drive
- **1.10.12** (2026-10-23): release; sdk/mii proven on a real Wii Remote
- **1.10.11** (2026-10-23): release; sdk/mii, Miis from homebrew
- **1.10.10** (2026-10-23): release; HOME's Test and Calibrate switch a MotionPlus on
- **1.10.9** (2026-10-22): sdk/mboard, posts to the Wii Message Board from homebrew; the Message Board library on the bench Wii, docs/mboard.md; docs/mboard.md: the WiiStation message seen on the bench board; docs/messageboard-messages.md: board record auth, icon, info; sdk/mboard's full message: a Mii, a photo, board data; mboard tests: the full message on the bench, and the Wii number; docs/mboard.md: the full message on the bench Wii's board; sdk/mboard sends to another Wii: mboard_send_msg; nandget and nandput pause WiiConnect24 around /shared2/wc24; HBC's play log is sdk/mboard; the library's docs everywhere; tests/dnscheck: what a Wii's DNS answers, from a fresh start; tests/eulacheck: why User Agreements shows a black screen; mboard_send_msg's text in base64, as the Wii Menu sends it; docs: a WiiLink letter's stationery and board icon, as received; Message Board stationery, five themes, and the address book; stationery themes: a better forest, a spacy night, arcade inside its border; stationery themes: a dark banner for the sender's name, quieter music; sdk/mboard docs audit: one structure, current facts, clearer API; the overlay switches a MotionPlus on for Test and Calibrate
- **1.10.8** (2026-10-18): release; Z+START opens HOME, Reconnect in player order; the bench queue's week of logs, read and acted on; the bench queue drives several Wiis; one firewall range for every bench Wii's network log; README: the older firewall rule's name, when widening it; the three Wiis on the bench queue, two of them pinned-only; devnet.md and netblock_run.py: a queue job's own Wii and log port
- **1.10.7** (2026-10-14): release; the agent leaves the app's MEM2 alone; CI green again
- **1.10.6** (2026-10-13): release; file times and flags, a sync that need not read
- **1.10.5** (2026-10-13): release; sync with big files, the agent's stack figure
- **1.10.4** (2026-10-13): release; Restart network, file errors that say what happened, long names on screen
- **1.10.3** (2026-10-11): release; transfers on screen, volatile interrupt flags; the bench's manage page, named jobs, and data after a freeze
- **1.10.2** (2026-10-09): release; a fault inside the agent's crash hook froze the Wii; bench jobs keep their data, and run in the order received
- **1.10.1** (2026-10-08): release; hbc_agent_stop() and hbc_agent_listen()
- **1.10.0** (2026-10-04): release; a Custom app order, no page cycling, Load by default; the bench queue's first five days of logs, read and acted on; every workstation's queue on the lease server; a queue that looked stuck, waiting for a Wii in the old HBC

## 1.9 ([details](docs/history/1.9.md))

- **1.9.9** (2026-10-04): release; hbc.py answered while Options is open
- **1.9.8** (2026-10-04): release; two USB drives, GPT disks, cached frame reads
- **1.9.7** (2026-10-04): release; the HOME overlay at 60 fps
- **1.9.6** (2026-10-04): release; agent safety tools, DEV > Info's hardware pages, hbc.py hw
- **1.9.5** (2026-10-04): release; popups from the network close themselves
- **1.9.4** (2026-10-01): release; B backs out of every menu; a live monitor for the bench queue; the bench monitor, 4 s a frame down to 0.1 s; the bench monitor checked end to end, a dispatcher crash fixed
- **1.9.3** (2026-10-01): release; the play log test in CI's container
- **1.9.2** (2026-10-01): release; bench jobs in a minimized window
- **1.9.1** (2026-10-01): release; Wiiload uploads named for the play log by their sender; the bench queue's fair turns, no contact during runs, a history
- **1.9.0** (2026-10-01): release; fatal reports, a hang watchdog and a kept log for every agent app

## 1.8 ([details](docs/history/1.8.md))

- **1.8.9** (2026-10-01): release; the Message Board play log, startup timings, the play record off the critical path
- **1.8.8** (2026-09-30): release; the bench queue's dead time between jobs
- **1.8.7** (2026-09-30): release; the bench queue no longer takes an agent app for HBC
- **1.8.6** (2026-09-30): release; one bench Wii for several workstations, on any OS
- **1.8.5** (2026-09-30): release; the libogc2 crash hook on the bench Wii, agent stack scan
- **1.8.4** (2026-09-30): release; the line-ending test in CI's container
- **1.8.3** (2026-09-30): release; crash reports on libogc2 and libogc 1.x, LF line endings
- **1.8.2** (2026-09-30): release; CI off Node.js 20
- **1.8.1** (2026-09-30): release; DEV > Sync clock, Save and Log that say something
- **1.8.0** (2026-09-29): release; wiispk, a WAV player, the pointer after a reset, a LULZ build

## 1.7 ([details](docs/history/1.7.md))

- **1.7.6** (2026-09-29): release; a Wii Remote speaker driver after Nintendo's SDK
- **1.7.5** (2026-09-29): release; pace the speaker at its real rate, a PCM/ADPCM test
- **1.7.4** (2026-09-29): release; the remote's speaker, Find's light chase, a real On glow
- **1.7.3** (2026-09-29): release; keep the Wii Remotes' command queues moving
- **1.7.2** (2026-09-29): release; working Find, a blue bloom, HBC's pointer on 16:9
- **1.7.1** (2026-09-29): release; the Wii Remote pointer and a visible highlight
- **1.7.0** (2026-09-29): release; the agent's overlay as HBC's HOME menu

## 1.6 ([details](docs/history/1.6.md))

- **1.6.2** (2026-09-29): release; fix the crash on a session's first small download
- **1.6.1** (2026-09-29): release; the overlay preview building under GCC
- **1.6.0** (2026-09-29): release; the agent's HOME overlay

## 1.5 ([details](docs/history/1.5.md))

- **1.5.0** (2026-09-29): release; the in-app agent and crash reports

## 1.4 ([details](docs/history/1.4.md))

- **1.4.1** (2026-09-29): release; MEM1 zlib state and the bench queue in git
- **1.4.0** (2026-09-29): release; the review's fixes and optimizations

## 1.3 ([details](docs/history/1.3.md))

- **1.3.6** (2026-09-29): release; the repository cleaned up
- **1.3.5** (2026-09-29): release; retried CI package installs
- **1.3.4** (2026-09-29): release; a rewritten README
- **1.3.3** (2026-09-29): release; the README's hardware status corrected
- **1.3.2** (2026-09-29): release; after the installed WAD passes on the dev Wii
- **1.3.1** (2026-09-29): release; checked banner tool I/O and a title launcher
- **1.3.0** (2026-09-29): release; fast, checksummed developer transfers

## 1.2 ([details](docs/history/1.2.md))

- **1.2.2** (2026-09-29): release; hardware-verified developer networking
- **1.2.0** (2026-09-28): release; developer network requests

## 1.1 ([details](docs/history/1.1.md))

- **1.1.9** (2026-09-28): release; a warning-free build, Python 3 tools and CI
- **1.1.8** (2026-09-28): release; bounds fixes and a portable build
- **1.1.6** (2026-09-27): release; Wii version readback
- **1.1.4** (2026-09-26): retail WAD with current devkitPro and Python 3
