# usbguard

*An app that survives a stuck USB drive: guarded calls, a warning, the software unplug.*

| | |
| --- | --- |
| **Files** | [`usbguard.h`](usbguard.h) and [`usbguard.c`](usbguard.c) |
| **Needs** | libogc; libfat for `usbguard_mount`. Link `-lusbguard -lfat -logc`. The decision logic builds on a PC with `-DUSBGUARD_NO_OGC` |
| **Licence** | Public domain |
| **Changes on the Wii** | Nothing, except `usbguard_free`, which disables the stuck drive's USB port until it is replugged (see [below](#what-it-changes-on-the-wii)) |
| **Since** | HBC 1.15.20 |

**Contents:** [What it does](#what-it-does) · [Quick start](#quick-start) ·
[Add it to your app](#add-it-to-your-app) · [API](#api) ·
[What a guarded call does](#what-a-guarded-call-does) ·
[Rules for `on_wait`](#rules-for-on_wait) · [The worker threads](#the-worker-threads) ·
[What it changes on the Wii](#what-it-changes-on-the-wii) · [Limits](#limits) ·
[Design and evidence](#design-and-evidence) · [On a PC](#on-a-pc) · [Users](#users) ·
[Licence](#licence)

## What it does

A USB drive that a reset left mid-transfer can stay powered and NAK every
packet for ever. The Wii's EHCI controller cannot switch port power
(HCSPARAMS `0x2204`: no power switching), so IOS's USB driver waits on that
transfer for good. While it waits, **every other IOS request waits behind
it**: SD and NAND files, ES, the network, Bluetooth (so no Wii Remotes) and
`IOS_ReloadIOS`. The app sits on a black screen until someone pulls the drive.

Only two things free IOS: pulling the drive, or disabling its port in the
EHCI controller (clearing PE in PORTSC), which needs AHBPROT (`HW_AHBPROT`
reads `ffffffff`). d2x cIOS rc3 does not block: it gives up after about 7 s
with -101.

usbguard:

- runs the calls that can meet such a drive on a worker thread it owns, and
  keeps your thread free to tell the user what to do;
- warns after `warn_ms` and, when the drive is pulled, waits for it to be
  plugged back in and retries once;
- offers the software unplug (`usbguard_free`) when AHBPROT allows it, and
  never uses it on its own.

## Quick start

```c
#include "usbguard.h"

usbguard_opts o = USBGUARD_OPTS_INIT;
o.on_wait = usbguard_default_ui;          // a text console; or your own

s32 rc = usbguard_mount("usb", &__io_usbstorage, &o);
if (rc == 0)
    ...                                   // mounted
else if (rc == USBGUARD_NO_DRIVE)
    ...                                   // nothing plugged in
else if (rc == USBGUARD_FREED)
    ...                                   // it was stuck; now freed, but not mounted: go on without USB
else if (rc == USBGUARD_STUCK)
    ...                                   // gave up: see "Limits" before the next IOS call
```

Any call can be guarded:

```c
static s32 my_init(void *arg) { return USBStorage2_Init((u32) arg); }

s32 rc = usbguard_call(my_init, (void *) 0, &o);
```

## Add it to your app

Copy `usbguard.h` and `usbguard.c` into your project (the header has
`extern "C"`), or build the library with `make -C sdk/usbguard` and link
`-L<hbc-reborn>/sdk/usbguard -lusbguard -lfat -logc` with
`-I<hbc-reborn>/sdk/usbguard`.

## API

Results other than the codes below are the call's own result.

| Name | What it does |
| --- | --- |
| `usbguard_call(fn, arg, opts)` | Runs `fn(arg)` on a worker and returns its result, `USBGUARD_STUCK` or `USBGUARD_FREED`. `opts` may be NULL. |
| `usbguard_mount(name, iface, opts)` | Runs `iface->startup()`, `iface->isInserted()` and `fatMountSimple` on the worker. 0 when mounted, else `USBGUARD_NO_DRIVE`, `USBGUARD_NOT_MOUNTED`, `USBGUARD_STUCK` or `USBGUARD_FREED`. |
| `usbguard_reload_ios(ios, opts)` | Runs `IOS_ReloadIOS` on the worker. A reload waits behind a stuck request too (minutes). No retry. |
| `usbguard_check()` | Reads PORTSC. `USBGUARD_CUT_OFF`: a drive is connected (CCS=1) but its port is disabled (PE=0). `USBGUARD_OK` otherwise. `USBGUARD_NO_ACCESS`: no AHBPROT. |
| `usbguard_can_free()` | true when AHBPROT allows `usbguard_free` and the ports read. |
| `usbguard_free()` | Disables every EHCI port with a drive. The stuck transfer then fails as if the drive had been pulled. **Only when you call it.** |
| `usbguard_pulled()` | true once a port `usbguard_free` disabled shows the drive taken out. |
| `usbguard_blocked()` | true while a worker has been in its request past `warn_ms`, or was given up on, and has not come back. |
| `usbguard_drive_present()` | 1: a USB 2.0 device is on an EHCI port. 0: none. -1: no AHBPROT. |
| `usbguard_freed_ports(out, max)` | What the latest `usbguard_free` wrote: each port, PORTSC before and after. Each free starts the list again. |
| `usbguard_default_ui` | A ready-made `on_wait` for a text console. |
| `usbguard_opts` | `warn_ms` (5000), `poll_ms` (50), `replug_ms` (10000), `on_wait`, `ctx`, `no_retry`, `priority` (64). `USBGUARD_OPTS_INIT`; 0 means the default. |
| Codes | `USBGUARD_STUCK`, `_FREED`, `_NO_WORKER`, `_NO_DRIVE`, `_NOT_MOUNTED`, `USBGUARD_D2X_GAVE_UP` (-101), `USBGUARD_MSG_STUCK`, `USBGUARD_WORKERS` (4), `USBGUARD_STACK_SIZE` (32 KB). |

## What a guarded call does

1. `fn` runs on a worker. Your thread calls `opts->on_wait(ms, phase, ctx)`
   about every `poll_ms` until it comes back. A call that answers at once
   never calls `on_wait`. A healthy drive behaves as before.
2. The phase is `USBGUARD_PHASE_WAITING`, then `USBGUARD_PHASE_STUCK` from
   `warn_ms`. In STUCK, show `USBGUARD_MSG_STUCK`: "The USB drive is not
   responding. Unplug it and plug it back in."
3. `on_wait` returns true to keep waiting, or false to give up. Giving up
   returns `USBGUARD_STUCK`, and the request stays stuck in IOS.
4. When `fn` comes back with an error after STUCK, either the drive was
   pulled or `usbguard_free` cut it off:
   - cut off by `usbguard_free`: `USBGUARD_FREED`, whatever the request
     answered (a stuck `IOS_Open` can still return a handle after the free;
     that handle stays open);
   - pulled: phase `USBGUARD_PHASE_REPLUG` while the library waits for the
     drive to be plugged back in. With AHBPROT it watches the ports, then
     allows 1.5 s for IOS to see the drive; without, it waits `replug_ms`.
     Then it runs `fn` **once** more. If that fails or gets stuck too, the
     result is `USBGUARD_FREED`. `on_wait` returning false in REPLUG skips
     the retry (`USBGUARD_FREED`).
5. A late success is `fn`'s result. A stuck request can also answer
   *successfully* as the drive is pulled (`IOS_Open` of `/dev/usb/ven`). With
   AHBPROT, when a drive was in at the start and is gone at the end, the
   library waits in phase REPLUG for it to come back before it returns (no
   retry: the call succeeded), so that your next call finds the drive.

**Which device is the drive.** The EHCI registers do not say. A call notes
every port with a USB 2.0 device when it starts. A pull is one of those ports
losing its device, so a second device that stays on another port does not
hide it. The replug is all of them back, or a device on a port that was empty
at the start for each one still missing: a drive put into the other port
counts. A hub, or a drive behind one, is one device. `usbguard_free` cuts
off every port with a device, so a hub goes with everything on it, and any
other USB 2.0 device (a LAN adapter, a second drive, a keyboard) with it.

**d2x rc3's -101.** rc3 gives up on the drive after about 7 s and returns
-101 (`USBGUARD_D2X_GAVE_UP`). One that came in under 6.5 s is a plain answer
and is returned as it is.

- **With AHBPROT:** if a drive is still connected, the slower -101 is taken as
  stuck. The phase stays STUCK until the drive is pulled (or freed), then
  REPLUG and one retry. With no drive connected, the -101 is returned.
- **Without AHBPROT:** the slower -101 returns `USBGUARD_STUCK`.

`usbguard_mount` sees no -101: a `DISC_INTERFACE` startup returns only true
or false. It takes a startup that failed after `warn_ms` or more as one.

## Rules for `on_wait`

The waiting path makes no IOS call, and `on_wait` must not either. Every IOS
call waits behind the stuck request, and your app would hang in the UI that
was meant to save it.

- **No IOS call of any kind.** That means no files on SD or NAND (stdout
  redirected to a file counts), no network, no `WPAD_*`, no
  `IOS_ReloadIOS`, and no sound that streams from a file.
- **Use only assets that are already loaded:** fonts, textures and sounds in
  memory. These are fine: drawing with GX, writing the XFB, printing to a
  text console, and reading the GameCube controller (`PAD_*` runs over SI,
  not IOS).
- **Wii Remotes do not work** while USB is stuck, because Bluetooth is a USB
  device in IOS. Read the GameCube controller instead, or say that the drive
  has to be unplugged. (Wii models without GameCube ports have only the pull.)
- **Offer "continue without USB" only when `usbguard_can_free()`.** It calls
  `usbguard_free()`, and the call then returns `USBGUARD_FREED`.

`usbguard_default_ui` follows these rules. While the phase is WAITING it
prints nothing. In STUCK it prints the message, says that Wii Remotes do not
work, and names the GameCube controller buttons that do: A to continue
without USB (only with AHBPROT). In REPLUG: B to go on without the drive. It
starts afresh for each attempt of each call.

## The worker threads

- A call you gave up on keeps its worker thread and stack until the request
  comes back (a pull or `usbguard_free`). **They are never reused or freed
  while blocked**: IOS still holds the request and will write its answer. A
  finished call's slot is free again only after its thread is joined.
- Up to `USBGUARD_WORKERS` (4) calls can be blocked at once. A call with no
  free worker returns `USBGUARD_STUCK` without running.
- A call made while another is blocked starts in phase STUCK: it will almost
  certainly wait behind the same request.
- Each stack is `USBGUARD_STACK_SIZE` (32 KB) from `memalign`, allocated at
  first use and kept. The worker's priority is `opts->priority` (64 by
  default).
- Make the guarded calls from one thread at a time. The free and pull
  records and `usbguard_default_ui` keep plain globals.

## What it changes on the Wii

| Call | What changes | How long | Risk |
|---|---|---|---|
| `usbguard_call`, `_mount`, `_reload_ios` | Nothing beyond the call itself. | n/a | None: the call runs on another thread. |
| `usbguard_check`, `_pulled`, `_drive_present`, `_blocked` | Nothing: register reads. | n/a | None. |
| `usbguard_free` | Disables the EHCI ports that have a drive. It also clears their connect-change bit (write 1 to clear); the other write-1-to-clear bits are written 0. | Until the drive is replugged or IOS resets the controller (an IOS reload). | Any transfer in progress on that drive fails, as with a pull. A drive that was writing can be left with a half-written file. Call it only when the user chooses to go on without USB. |

## Limits

- **`USBGUARD_STUCK` does not always mean a request is still in IOS.** It
  comes back when `on_wait` gave up (the worker is blocked), when no worker
  was free, and also after the worker returned: the slow -101 case, when
  `on_wait` gave up or `replug_ms` ran out while waiting for the pull, or
  there is no AHBPROT. Then `usbguard_blocked()` is already false, but the
  drive is still in and probably not answering. Make no IOS call while
  `usbguard_blocked()` is true.
- **A reload you gave up on** leaves IOS half shut down: leave by a pull or
  `usbguard_free` only.
- **Without AHBPROT** the replug cannot be seen: the library waits
  `replug_ms` and retries, and `usbguard_free` does nothing.
- **Which device is the drive cannot be told**, so a pull of any USB 2.0
  device that was in at the start of the call counts, and `usbguard_free`
  cuts all of them off.
- **One retry** after a pull. A second failure is `USBGUARD_FREED`. Do not
  retry in a loop: each stuck attempt holds one of the 4 workers.
- **One calling thread at a time.**
- **A port reads cut off for a moment** while IOS enumerates a new drive
  (`usbguard_check`).

## Design and evidence

The worker owns the blocking IOS call, so the app thread can draw and read the
GameCube controller. The library never disables a port by itself: that stops
transfers on every USB 2.0 device, so the user chooses it.

Checked on the dev Wii (2026-11-08, HBC 1.15.20, IOS58) with d2x's `usb-spin`
and its watchdog reset leaving the drive stuck:

- the stuck state hung IOS58, d2x rc1 and upstream d2x 11.0; USB Loader GX and
  the d2x installer both stopped;
- HBC's start-up probe stuck on the worker, `usbguard_free` ran, the probe
  answered 3 ms after the disable, and Wii Remotes worked;
- the app's warning came at 5.0 s every time;
- a pull at 7.7 s gave phase REPLUG, the guard waited for the replug, and the
  mount then worked;
- A on a GameCube controller ran `usbguard_free`, and the call returned FREED;
- a stuck `IOS_Open` of `/dev/usb/ven` answered with a handle 1 ms after the
  free, and one answered with a handle at 16 s as the drive came out (hence
  point 5 of "What a guarded call does");
- a reload waited 217 s behind the stuck request, until the drive was pulled.

## On a PC

`-DUSBGUARD_NO_OGC` builds the decision logic with host threads, as the other
SDKs' `*_NO_OGC` switches do. The test program supplies the registers
(`usbguard_host_read32/write32`), `fatMountSimple` and stand-in `ogc/disc_io.h`
and `fat.h` headers in
[tests/host/usbguard_host/include](../../tests/host/usbguard_host/include);
`SETTLE` and d2x's give-up time can be overridden there with `-D`.
[tests/host/test_usbguard.py](../../tests/host/test_usbguard.py) uses
[tests/host/usbguard_host](../../tests/host/usbguard_host/main.c) to cover:

- the phases and their timing;
- a retry after a replug, including into another port;
- the free path, including the PORTSC value written;
- the no-AHBPROT path;
- d2x's -101;
- giving up while stuck, while waiting for a pull and while waiting for a
  replug, and running out of workers;
- a reload.

[tests/apps/usbguard_app](../../tests/apps/usbguard_app/main.c) runs each
wrapper on a Wii or in Dolphin, on a text console. `check` is for a healthy
USB; [tests/dolphin/dolphin_usbguard.py](../../tests/dolphin/dolphin_usbguard.py)
runs it in Dolphin. `stuck` is the attended run with a stuck drive;
[tests/wii/wii_usbguard.py](../../tests/wii/wii_usbguard.py) queues it on a Wii
(d2x's usb-spin and a watchdog reset).

## Users

- **HBC**: its start-up USB check, its software unplug, its replug check
  and the TV line (`channel/channelapp/source/bootlog.c`), and the IOS80
  fallback reload in `main.c`.
- **USB Loader GX**: next; see [USBLOADERGX.md](USBLOADERGX.md).

## Licence

Public domain (the Unlicense); the notice is at the end of `usbguard.c`.
