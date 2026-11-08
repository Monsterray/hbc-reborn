# hbc_netlog

*Your app's `printf` on your PC, over the network.*

| | |
| --- | --- |
| **Files** | [`hbc_netlog.h`](hbc_netlog.h), header only |
| **Needs** | libogc (or libogc2), a Wii on the same network as the PC |
| **Licence** | Public domain |
| **Changes on the Wii** | Nothing lasting (see [below](#what-it-changes-on-the-wii)) |
| **Since** | HBC 1.2.0 |

**Contents:** [What it does](#what-it-does) · [Quick start](#quick-start) ·
[Add it to your app](#add-it-to-your-app) · [API](#api) ·
[What it changes on the Wii](#what-it-changes-on-the-wii) · [Limits](#limits) ·
[Licence](#licence)

## What it does

- Sends everything your app writes to `stdout` and `stderr` to
  `tools/hbc.py` on your PC, as it happens.
- Leaves an on-screen console you set up first working as before.
- Finds the PC on its own: `hbc.py` tells HBC its address, and HBC hands it
  to the next app it starts, also after that app returns to the installed
  channel.
- Gives up within about 5 seconds when there is no network or no PC, so a
  release build that keeps it does no harm.

## Quick start

In the app, once, after any video or console setup:

```c
#include "hbc_netlog.h"

int main(int argc, char **argv) {
    // ... VIDEO_Init, console setup, etc. ...
    hbc_netlog_init();                   // 0 when connected; harmless otherwise
    printf("hello from the Wii, argc=%d\n", argc);
    return 0;                            // the log closes at exit()
}
```

On the PC:

```sh
python3 tools/hbc.py run myapp.dol level1          # send, run, and print the output
python3 tools/hbc.py run myapp.dol -- --verbose    # "--" before arguments that start with "-"
python3 tools/hbc.py log                           # or: wait for apps started from the Wii's menu
```

[`tests/netlog_app`](../../tests/netlog_app) is a complete example.

## Add it to your app

Copy `hbc_netlog.h` into your project, or add `-I<hbc-reborn>/sdk/hbc_netlog`
to your compiler flags. There is nothing to link.

The PC must accept inbound TCP on the log port: 4405 by default, or choose one
with `hbc.py --log-port`. On Windows, allow it once in Windows Defender
Firewall.

## API

| Name | What it does |
| --- | --- |
| `s32 hbc_netlog_init(void)` | Connects to the PC HBC named and sends `stdout` and `stderr` there. Returns 0 when connected, a negative value otherwise. Waits for a network start-up another thread began (the agent's, for example). |
| `void hbc_netlog_close(void)` | Flushes and closes the log so the PC sees its end. `exit()` does this for you; call it before leaving any other way (a loader that jumps to a game). Safe to call twice. |
| `HBC_NETLOG_DEFAULT_PORT` | 4405, the PC's log port when `hbc.py` sets none. |
| `HBC_NETLOG_LAYOUT_ONLY` | Define before the include to get only the shared block layout (`hbc_netlog_block`), as HBC itself does. |

The block HBC leaves for the app is at `HBC_NETLOG_ADDR` (`0x80002f20`), with a
copy at `HBC_NETLOG_KEEP_ADDR` (`0x91800000`, MEM2). Its format is in
[docs/devnet.md](../../docs/devnet.md).

## What it changes on the Wii

| Part | What changes | How long it lasts |
| --- | --- | --- |
| The network | One TCP connection from the Wii to the PC. | While the app runs. |
| Low memory | Reads the 32-byte block HBC left at `0x80002f20`. | Nothing is written. |
| MEM2 | HBC keeps a copy of the block, and of the app's last output, at `0x91800000`. | Until the next app or a power-off. |

It installs nothing and writes no files.

## Limits

- The app must be started by this HBC (1.2.0 or later), from the PC or from
  the Wii's menu with `hbc.py log` running. Started any other way, it finds no
  PC and `hbc_netlog_init()` returns an error.
- Output written before `hbc_netlog_init()` stays on the Wii.
- On a slow network, a burst of output can hold up the thread that writes it.

## Licence

Public domain: copy, change and ship it in anything, open or closed, with no
credit needed.
