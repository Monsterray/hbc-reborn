# hbc.py: the developer network from your PC

`tools/hbc.py` talks to HBC (and to apps with the in-app agent) over the
network: status, files, sending and running apps, their output. It needs
Python 3.10+ and nothing else. Start with the
[quick start](../README.md#quick-start-develop-on-a-real-wii); the wire
format is in [devnet.md](devnet.md).

`python3 tools/hbc.py` on its own prints an overview of every command, and
`hbc.py help COMMAND` explains one with examples. The table below is the
same list.

```
python3 tools/hbc.py [--wii ADDR] [--json] [--log-port PORT] [--timeout S] COMMAND ...
```

| Command | What it does |
| --- | --- |
| `version` | Print the running HBC version (`HBCV`). |
| `status` | Print JSON status: version, protocol, IOS and revision, AHBPROT, free memory, loader stack use, startup and app-scan time, IP, app count, mounted device, log target, and timing of the last transfer. |
| `hw` | The Wii's hardware and settings, as DEV > Info shows them (`HBCH`): console, serial, console ID, Wii Menu, IOS, boot2, CPU; video mode, cable, TV settings; SD, USB, NAND and memory cards; USB devices; IP, gateway, DNS, MAC, the connection in use. |
| `wait [SECONDS]` | Wait until HBC answers (default 90 s). |
| `send FILE [ARG ...]` | Send a DOL, ELF, or ZIP over Wiiload. A ZIP is installed to the SD card after you confirm on the Wii, or at once with `--yes`. Fails with HBC's reason when the Wii cannot load it. `--no-wait` returns as soon as HBC shows its verdict or stops answering (at most 8 s), for a test app that replaces HBC and is followed by `hbc.py wait`. |
| `run FILE [ARG ...]` | Register for logs, send `FILE`, and print its output until it exits (`--timeout`, default 300 s). Fails at once with HBC's reason when the Wii cannot load it. |
| `log` | Register for logs and print app output until Ctrl+C. Use it when you launch apps from the Wii itself. |
| `ls REMOTE` | List a directory, e.g. `sd:/apps`. Lines are `d DATE TIME name` or `f size DATE TIME name` (when it last changed, by the Wii's clock), then the FAT attributes set: `r` read-only, `h` hidden, `s` system, `a` archive. |
| `stat REMOTE` | One file or directory: size, when it last changed, its attributes. |
| `attrib [--recursive] [+r\|-r] [+h\|-h] [+s\|-s] [+a\|-a] REMOTE...` | Show or change FAT attributes, as Windows' `attrib` does. A read-only file cannot be written over or deleted, nor a read-only folder deleted, until `-r`. HBC 1.10.15 and its agent. |
| `get [-r] REMOTE [LOCAL]` | Download a file, or with `-r` a directory tree. |
| `put [-r] [--no-verify] LOCAL REMOTE` | Upload a file or, with `-r`, a directory tree; parent directories are created. A `REMOTE` ending in `/` gets the file's name appended. Each file is then read back on the Wii and its size and CRC-32 checked, uploading it again (twice at most) on a mismatch; `--no-verify` skips that. |
| `sync [--delete] [--checksum] [--no-verify] LOCALDIR REMOTEDIR` | Make `REMOTEDIR` match `LOCALDIR`, uploading only what changed: a same-size file the Wii wrote after the local one last changed is skipped without being read; otherwise the CRC-32s decide (`--checksum` uses them for every same-size file). Uploads are read back and checked as `put` does (`--no-verify` skips it). `--delete` also removes remote extras; a file that fails is reported and the rest go on. |
| `rm [-r] [--force] REMOTE` | Delete a file or an empty directory, or with `-r` a tree. A `*` or `?` in the last name deletes every match (`rm 'sd:/logs/*.log'`; folders only with `-r`). The Wii walks the tree or pattern itself in one request. Device roots and `<device>:/apps` itself are refused, by the Wii too, and so is a read-only entry unless `--force`, which clears read-only first (as `del /f` does). |
| `mkdir REMOTE` | Create a directory and its parents. |
| `mv [--replace] FROM TO` | Rename or move a file or folder (also `rename`). A `TO` ending in `/`, or a folder that is there, means "into that folder". On one device the Wii renames in place, at once; between devices (`sd:` to `usb:`) each file is copied, checked by CRC-32, and only then deleted (files up to 512 MiB; a read-only source is refused first; the copies get new times). An existing `TO` is refused unless `--replace` (a file only). HBC 1.10.14 and its agent. |
| `mii-faces` | The same as HOME > HBC > Save Mii faces (`HBCM`): HBC copies the Wii Menu's Mii face models (`RFL_Res.dat`) and the Mii Channel's bodies (`maleBody.brres`, `femaleBody.brres`) and effects (`effect_nigaoe.breff`, `.breft`) to `sd:/mii`, for apps that draw Miis. The files are Nintendo's: keep them on your own SD card, never ship them. HBC 1.10.20 (bodies 1.10.26, effects 1.12.2). |
| `net-hang [wait\|reload]` | What HBC does when a network cleanup hangs inside IOS: carry on without the network until it next starts (the default), or reload itself at once (for a developer's Wii); with no mode, print the current one. |
| `transfer-style [scroll\|bounce\|truncate]` | Force how HBC shows a file name too long for its on-screen transfer line, until it restarts (for testing); with no name, print the current one. |
| `order [--clear] APPS [FOLDER ...]` | HBC's Custom app order (see [Your own app order](using-hbc.md#your-own-app-order)): with only `APPS` (e.g. `sd:/apps`), print the apps in that order with their `<sort_id>`; with folders, give them 10, 20, 30... in the order named, editing or making each `meta.xml`; `--clear` removes every `<sort_id>`. |
| `exit` | Ask the running [agent](../sdk/hbc_agent/README.md) app to exit to HBC, and wait for HBC. |
| `key KEYS` | Send controller presses to HBC's HOME menu or the running agent app: `h` (HOME, opens or closes its overlay), `u` `d` `l` `r` (D-pad), `a`, `b`. For scripted tests and for driving the overlay from the PC. |
| `screen FILE.png` | Save what the TV shows, from HBC or a running agent app, overlay included. |
| `crash [--elf FILE] [--clear]` | Print the crash an agent app reported: exception, registers, backtrace, and with `--elf` function names and source lines (needs devkitPPC's `addr2line`). `--clear` forgets it. |

`--json` makes `version`, `status`, `ls`, and `crash` print JSON. Options can follow
the command; for `send` and `run`, everything after the file goes to the app,
and `--` ends option parsing. Large transfers show progress on a terminal.

The Wii address comes from `--wii`, then `$HBC_WII`, `$WII_BENCH_IP`, or
`$WIILOAD` (`tcp:ADDR`, the variable the devkitPro `wiiload` uses). With
none of those set, `hbc.py` asks the network (`hbc.py find`, a UDP broadcast)
and uses the one Wii that answers. `hbc.py find` lists every Wii running HBC
or an agent app, with its address and name.

Remote paths are `<device>:/<path>` with device `sd`, `usb`, `usb2` (a
second USB drive), `carda`, or `cardb`; paths containing `..`, `//`, or backslashes are refused. An app you
`put` or `sync` under `sd:/apps/` appears in the menu right away, and one you
remove disappears. The menu reloads once the writes stop (a second of
quiet), on the page it shows, so a whole folder's upload is one quiet reload.
While a file moves, HBC shows it at the bottom right of the screen
("Receiving ridgerac/boot.dol 42%", "Sending save.dat"). Files under a
device's `apps/` directory show the app folder and any subfolders; other
files show their parent folder and name (a file at a device's root, its name). Options (1) > Progress: **Circle** replaces the
percentage with a progress circle in the text colour on a faded track (the
choice is kept in `settings.xml`). The circle fills clockwise from the top,
and turns while the transfer's size is unknown. A name too long for the
line scrolls round, bounces from end to end, or ends in "..." (each HBC
start picks one at random; `hbc.py transfer-style bounce` forces one for
testing). A moving name rests a second at its start, and a bouncing one at
each end; "Receiving" and the percentage (or circle) stay put. A delete
shows "Deleting" with the name sent (a folder or pattern, not each entry
inside it); the circle turns while HBC counts the entries, then fills as
they go.

When receiving a Wiiload upload from `hbc.py`, the popup heading shows the
file name, including its app folder for `boot.dol` or `boot.elf`. The sender's
IP address appears below the progress bar at the lower left, separate from
the percentage on the right. A sender that supplies no name keeps the
"Receiving from <address>" heading.

Uploads and downloads use 64 KiB frames, each with a CRC-32 checked on both
ends, compressed with zlib when that makes them smaller. A corrupted frame
fails the transfer and leaves no partial file. On an 802.11g Wii a typical
ELF moves at about 1.5 MB/s up and 0.9 MB/s down, and compressible data at
4 to 5 MB/s; incompressible data is limited by the Wii's network stack to
about 1.35 MB/s in and 0.6 MB/s out. [docs/devnet.md](../docs/devnet.md) has the
measurements.

## Seeing your app's output

Add [`sdk/hbc_netlog`](../sdk/hbc_netlog/README.md) to your app (one public-domain
header) and call `hbc_netlog_init()` once; `hbc.py run myapp.dol` then prints
everything the app writes. Its README has the example, the firewall note and
what to do for apps started from the Wii's own menu.

## A typical loop

```sh
make && python3 tools/hbc.py run myapp.dol          # build, run, watch the log
python3 tools/hbc.py sync dist/myapp sd:/apps/myapp # install: only changed files move
python3 tools/hbc.py put data/level1.bin sd:/apps/myapp/level1.bin
python3 tools/hbc.py get sd:/apps/myapp/save.dat    # pull a file the app wrote
python3 tools/hbc.py mv sd:/apps/myapp sd:/apps/myapp-old   # rename, at once
python3 tools/hbc.py status                         # IOS, AHBPROT, memory after a run
```

To install an app permanently, `sync` or `put -r` its folder (with
`boot.dol` and `meta.xml`) to `sd:/apps/<name>/`, or `send` a ZIP.

`hbc_netlog_init()` gives up within about 5 s when the PC is unreachable, and
`run` and `log` clear HBC's log target when they exit, so a stale target
never stalls later apps.
