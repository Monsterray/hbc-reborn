# hbc_agent: HBC's developer tools inside your app

Link the agent into a development build and `tools/hbc.py` keeps working
while your app runs:
- `status`, the file commands and `log` reach your app instead of HBC.
- `run` and `send` ask the app to exit to HBC before sending the next build.
- A crash comes back to the PC as a report, not a frozen screen.

The API and every option are in [../hbc_agent.h](../hbc_agent.h).
[tests/agent_app](../../tests/agent_app) is a complete example, and HBC's own
HOME menu (`channel/channelapp/source/home.c`) is a commented one.

## Use

```sh
make -C sdk/hbc_agent            # sdk/hbc_agent/libhbcagent.a
```

```c
#include <fat.h>
#include "hbc_netlog.h"          // optional: stdout to the PC
#include "hbc_agent.h"

int main(int argc, char **argv) {
    // ... video setup ...
    fatInitDefault();            // mount first: the agent serves what the app mounted
    hbc_netlog_init();
    hbc_agent_init(NULL);        // one low-priority thread, about 20 KiB while idle
    while (!hbc_agent_exit_requested()) {
        // ... one frame ...
    }
    return 0;                    // back to HBC
}
```

Link with `-I<hbc-reborn>/sdk -L<hbc-reborn>/sdk/hbc_agent -lhbcagent -lfat -lz -logc`.

- **Other libogcs:** for libogc2 or libogc 1.x, build with
  `make -C sdk/hbc_agent OGC=libogc2` (or `OGC=libogc-1.8.23`) and link the
  copy made under `sdk/hbc_agent/<OGC>/`.
- **The network:** if your app calls `net_init()` itself, call it before
  `hbc_agent_init()`. libogc hangs if two threads start the network at once.
- **The HOME overlay:** `hbc_agent_home()` adds one in HBC's style, with
  DEV, Exit, Shot and Wii Remote menus.
- **Leaving another way:** an app that leaves without `exit()` (a loader
  jumping to a game) calls `hbc_agent_stop()` first.

## What it does to your Wii

| Part | What changes | How long it lasts |
|---|---|---|
| The agent thread | One thread below your main thread, and about 20 KiB at idle, taken from the top of the MEM2 arena. A file transfer borrows up to about 560 KiB and gives it back. | While the app runs. |
| The network | The Wii listens on TCP port 4299. **Any PC on your network** can then list, read, write and delete files on the devices your app mounted, and stop the app, just as with HBC itself. | While the app runs. Keep it to development builds on a home network. |
| Files | Only what the PC asks for, on the devices the app mounted: SD, USB, or the NAND only if your app mounts it. The agent writes nothing by itself. | For good, like any file write. |
| Crash reports | A small record and the app's last output, in MEM2 at `0x91800000`, written only as the app stops. HBC reads them back. | Until HBC reads or clears them, or the Wii powers off. |
| Safety tools | They watch the stack, `assert`/`abort`, Reset and Power, and write back the SD and USB caches at exit. Nothing is saved. | While the app runs. |

It installs nothing, and changes no settings and no NAND files of its own.

## Licence

The agent is built from HBC's own sources, so it is **GPL v2 or later**,
like HBC. An app that ships with it linked in must be GPL-compatible.
Otherwise keep the agent in development builds only, for example behind an
`#ifdef`. `hbc_netlog.h` is public domain and can go into any app.
