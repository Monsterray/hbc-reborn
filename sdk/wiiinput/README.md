# wiiinput

The controllers libogc's WPAD and PAD do not read, for HBC's
`channel/channelapp/source/controls.c`. It is a rewrite of
[libwiiinput](https://github.com/quatric/libwiiinput) for this tree: the
parts HBC needs, built straight into the channel like `sdk/mii`, with no
submodule or prebuilt library.

| Controller | How HBC reads it | File |
| --- | --- | --- |
| Wii U Pro Controller | as a Classic Controller (D-pad, A, B, X = 1, Y = 2, L/- = Minus, R/+ = Plus, HOME, ZR starts the network, ZL takes a screenshot) | `wupc.c` |
| GameCube controller adapter for Wii U (WUP-028 and the adapters that copy it) | its four ports as GameCube controllers, like the console's own ports (Z and START together open HOME) | `gcad.c`, `gcadapter.c` |
| USB keyboard | arrows = D-pad, Enter or Space = A, Backspace = B, X or 1 = 1, Y or 2 = 2, - or Delete = Minus, + or Insert = Plus, Esc or Home = HOME, Print Screen takes a screenshot | `wiiinput.c` |
| USB mouse | the pointer, left button = A, right = B, middle = HOME, the wheel scrolls; a Wii Remote pointing at the screen takes the pointer back, and the mouse carries on from there | `wiiinput.c` |
| A fourth Wii Remote, or a Balance Board left on | no longer crashes libogc 3.1 as it connects | `wpadqueue.c` |

GameCube controllers in ports 2 to 4 of the console are read by
`controls.c` itself (HBC used to read port 1 only). The HOME menu reads all
of these too, through the agent's `buttons_down` hook (`sdk/hbc_agent.h`).

## Using it

```c
#include "wiiinput.h"

wiiinput_init(0);               // before WPAD_Init()
wiiinput_mouse_area(640, 480);
...
wiiinput_scan();                // once a frame
u32 classic = wiiinput_classic_down();  // WPAD_CLASSIC_BUTTON_*
u16 gc = wiiinput_gc_down();            // PAD_BUTTON_*
u32 keys = wiiinput_keys_down();        // WPAD_BUTTON_*
...
wiiinput_shutdown();            // before WPAD_Shutdown()
```

Link with `-lwiikeyboard` and
`-Wl,--wrap=wiiuse_accept,--wrap=wiiuse_init_cmd_queue`, as
`channel/channelapp/Makefile` does.

- **Wii U Pro Controller:** pair it from the Wii's SYNC button like a Wii
  Remote. Up to four, in the order they connect; their LEDs show the order.
  `wupc.c` follows FIX94's [libwupc](https://github.com/FIX94/libwupc),
  brought to libogc 3.1's `wiiuse_accept` and `bte_listenasync`.
- **GameCube adapter:** looked for every two seconds, so it can be plugged in
  with HBC running. Under IOS 58 libogc's USB already holds `/dev/usb/hid`,
  so the driver goes through libogc's device list; under a d2x cIOS it opens
  `/dev/usb/hid` itself. A USB module that does not answer within 1.5 s turns
  the adapter off instead of hanging. The driver comes from
  [RiftWii](https://github.com/KakarottoCake/Riftwii) by way of libwiiinput.
- **Keyboard:** a thread keeps it open and scans it, so it can be plugged in
  at any time.
- **Mouse:** libogc's driver finds it when it is plugged in.

Not here: the DualShock 3 (libsicksaxis has no licence to copy it under) and
the Wii U GamePad (only reachable from vWii injects, never from HBC).

## License

`gcad.c`, `gcad.h`, `gcadapter.c` and `wpadqueue.c` (from RiftWii) and
`wupc.c` (from libwupc) are GPL-3.0-or-later, as their headers say;
`wiiinput.c` and `wiiinput.h` are, like the rest of HBC, GPL-2.0-or-later.
The channel built with them is therefore distributed under GPL-3.0.
