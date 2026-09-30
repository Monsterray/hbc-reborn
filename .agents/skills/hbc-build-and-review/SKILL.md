---
name: hbc-build-and-review
description: Build, test, debug, or review this Homebrew Channel project across its Wii runtime, WAD packaging, and host tools. Use for project changes and subsystem audits; use README for one-time contributor setup.
---

# HBC build and review

Read `README.md` for dependencies and `REVIEW.md` for the current subsystem plan. Trace the affected path from input to output before changing code. Fix the smallest shared cause that explains a confirmed failure. Keep future work separate from verified defects.

## Build checks

From the repository root, use the installed devkitPro toolchain:

```sh
make -C wiipax
make -C channel/channelapp channel
make -C channel PYTHON="$(pwd)/.venv/bin/python"
make -C channel/title PYTHON="$(pwd)/.venv/bin/python" check
tests/host_bounds.sh
```

On Windows, run these from the devkitPro MSYS2 shell (`C:/devkitPro/msys2/usr/bin/bash.exe -lc '...'`) and use `.venv/Scripts/python`. Git Bash sets `DEVKITPRO=/opt/devkitpro`, which only exists inside that MSYS2. The tree has no symlinks; keep it that way, because Windows checkouts store them as text files.

The full build needs `pycryptodomex` in `.venv`, SoX, `msgfmt`, host libpng, and a private 16-byte `~/.wii/common-key`. Use `make -C channel clean` when testing a clean build. The retail WAD is `channel/title/channel_retail.wad`. The optional DPKI target needs separate private signing keys.

## Runtime checks

`python3 tests/dolphin_smoke.py [image] [seconds]` boots the DOL in a throwaway Dolphin profile and passes when the guest answers `HBCV` with this release's version. Dolphin binds the guest socket to the host's LAN address, not loopback. The script passes every setting with `-C`, kills only its own process, and greps the log for faults. Build `tests/netlog_app` first and add `--devnet` to also run status, SD file requests, bad-path rejection, and the network-log round trip against an emulated SD card; see `docs/devnet.md`. Use `tools/hbc.py` (status, run, put, get, ls, rm, mkdir, log) rather than ad hoc sockets for real-Wii work. Pass the retail WAD instead of the DOL to install it into the throwaway NAND; the script then enables cheats (disabling Dolphin's `HBReload`) and requires the installed channel to come back after `netlog_app` exits. On the bench Wii, queue `python tests/wii_devnet.py --log-port 4300` (TCP 4300 is the port the firewall allows inbound), `python tests/wii_netbench.py`, and for `sdk/hbc_agent` `python tests/wii_agent.py --log-port 4300` (it sends this tree's DOL whenever an app returns to an older installed channel). `--agent` runs the same agent checks in Dolphin with MMU emulation on, so the test app's store to `0x10` faults. It boots the retail WAD and refuses a DOL: in a DOL boot HBC keeps Dolphin's `HBReload` hook, so an app's exit never comes back. Drive the agent's HOME overlay with `hbc.py key` and check it with `hbc.py screen` (the agent suite does both); `python3 tests/overlay_preview/preview.py` renders every overlay page on the PC from the same `ov_ui.c`/`ov_draw.c`. IOS clears low memory on every title launch; anything that must outlive an app's return to the installed channel goes in MEM2 (`0x91800000`, see `docs/devnet.md`). Dolphin's host sockets hide IOS network quirks (reset on close, misreported non-blocking sends, per-call latency), so treat Dolphin network results as functional only and measure speed on hardware.

Test a new DOL in an isolated Dolphin profile. The sibling Wii64 project's `.dev/dolphin_test.sh` shows the local Intel Mac profile and log setup; adapt its steps to this DOL rather than reusing a Wii64 ROM workflow. Read only the latest boot segment in Dolphin's append-only log. Match guest fault addresses to the ELF from the same build. A link result alone does not show that the menu or loader works.

On macOS, the Wii64 launch pattern is:

```sh
profile="$(mktemp -d /tmp/hbc-dolphin.XXXXXX)"
open -n -a /Applications/Dolphin.app --args -b \
  -e "$(pwd)/channel/channelapp/channelapp-channel.dol" -u "$profile" \
  -C Dolphin.Core.CPUThread=True -C Dolphin.Core.MMU=True \
  -C Dolphin.Core.DSPHLE=True \
  -C Dolphin.Analytics.PermissionAsked=True \
  -C Dolphin.Analytics.Enabled=False \
  -C Dolphin.Interface.UsePanicHandlers=False \
  -C Logger.Options.WriteToFile=True -C Logger.Options.Verbosity=4 \
  -C Logger.Logs.MASTER=True -C Logger.Logs.BOOT=True
```

The append-only log is `$profile/Logs/dolphin.log`; inspect only its latest `Starting core = Wii mode` segment. Launch Dolphin through `open`; direct executable launch caused a Qt/Cocoa startup abort on this Mac. Logs can end at `Setup Wii Memory` even when the guest menu is visible, so confirm the display too. The 1.1.7 retail WAD installed and showed the HBC menu in an isolated Dolphin NAND; app return from that installed title still needs a separate test.

For a WAD boot, use a new profile and pass `channel/title/channel_retail.wad` to `-e` in the same `open` command. Dolphin imports the ticket and title into that profile's `Wii/` NAND. Check the menu visually and test app return separately. For an interactive return test, omit `-b`, add `-C Dolphin.Core.EnableCheats=True` with no cheat codes, and add `-C Dolphin.Interface.ConfirmStop=False`. Dolphin otherwise replaces the HBC stub at `0x80001800` with its own stop-emulation hook. Check the actual stub bytes or PC in the debugger before interpreting an exit result. See `WII_DEVELOPMENT.md`.

The bench Wii on this workstation is shared. Queue hardware jobs through `python tools/wii-bench/wiibench.py add --name NAME --cwd DIR -- CMD` and `wait ID` (git-controlled here; `C:/tools/wii-bench/wiibench.py` is a shim to it and holds the shared queue state); the job gets `WII_BENCH_IP` and must leave the Wii in HBC. Other workstations (Windows, Linux, macOS) share it through the lease server on the homeserver; set one up with `wiibench.py setup --server URL` (see `tools/wii-bench/README.md`). Probe HBC only with a request that sends data (`HBCV` or `PING` + 12 bytes); bare connect-and-close probes fill its listen backlog.

For a real Wii check, send the DOL through the existing Homebrew Channel with Wiiload, then launch a known DOL such as Wii64 through the new menu. Keep an installable WAD off the real Wii until installation is an explicit test objective.

To confirm the running DOL version before launching another app, use the local Wii IP from the Wii64 project's ignored `.dev/hardware.env`:

```sh
wii_ip="$(sed -n 's/^WII64_WII_IP=//p' ../Wii64/.dev/hardware.env)"
WIILOAD="tcp:$wii_ip" /opt/devkitpro/tools/bin/wiiload channel/channelapp/channelapp-channel.dol
python3 tools/hbc.py --wii "$wii_ip" wait
```

The second command polls port 4299 with a read-only `HBCV` query until HBC answers and prints its version; compare it with `CHANNEL_VERSION_STR`.

## Packaging and release checks

For WAD changes, parse the ticket and TMD and verify both content sizes and decrypted SHA-1 hashes. Do not print, copy into the repo, or stage keys. Before a commit, inspect staged names and bump both version fields named in `AGENTS.md`. State whether validation covers a DOL boot, a WAD build, or an installed channel.
