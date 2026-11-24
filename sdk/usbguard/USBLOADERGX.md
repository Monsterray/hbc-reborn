# usbguard in USB Loader GX

This is the plan for USB Loader GX to adopt [usbguard](README.md). Read the
README first: the phases, the results and the rules for `on_wait`. Line
numbers are from the USB Loader GX fork at commit `f3769067`.

## Why

A USB drive that a reset left mid-transfer NAKs for ever. Under it, USB
Loader GX stops on a black screen with IOS58, d2x rc1 and d2x 11.0. Two
things hang:

- the first `handle->startup()` (in `USBStorage2_Init`);
- every IOS call after it (the SD card, WPAD, `IOS_ReloadIOS`).

d2x rc3 returns -101 after about 7 s. `startup()` turns that into a plain
`false`, and USB Loader GX goes on spinning up for 20 s.

HBC 1.15.19+ already checks USB at start-up and clears PE on the stuck port.
It asks for a replug before a menu launch and refuses a network launch while
the drive is still in. So USB Loader GX can start in two states:

- **the port cut off**: `usbguard_check() == USBGUARD_CUT_OFF`. The drive is
  in, but disabled. It answers nothing until it is replugged.
- **the drive stuck**: no HBC, an older HBC, or a reset while USB Loader GX
  itself was writing.

## Adding the library

- Copy `usbguard.c` and `usbguard.h` to `source/libs/usbguard/`. They are
  public domain and need only libogc and libfat, which USB Loader GX already
  links. The header has `extern "C"`.
- USB Loader GX starts with `<ahb_access/>`, and `ReloadIosKeepingRights`
  keeps it after a reload, so `usbguard_can_free()` is normally true. Offer
  "continue without USB" then.

## The GUI while it waits

`on_wait` runs on the thread that made the call. **It must make no IOS
call.** In USB Loader GX that rules out three things it does all the time:

- **`UpdatePads()`**: it reads WPAD, and WPAD is Bluetooth, which is a USB
  device in IOS. Read the GameCube controller instead: `PAD_ScanPads()` and
  `PAD_ButtonsDown(chan)` run over SI. Wii Remotes do not work while USB is
  stuck, so say so on screen.
- **Sound** that streams from SD or USB (the background music). Music
  already in memory is fine.
- **Logging to a file** (`gprintf` to SD, if enabled). Log to memory, and
  write it after the call returns and `!usbguard_blocked()`.

Drawing is fine, as long as everything drawn is already loaded:
`messageTxt->SetTextf` and `Draw()` with fonts and images in memory. Check
that `StartUpProcess::Draw()` and the prompt windows load nothing on
demand.

Other threads that make IOS calls (the GUI input thread, the network
thread, the sound streamer) will block on their next IOS call while USB is
stuck. They wake by themselves after a pull or `usbguard_free`. Pause them
around a guarded call if their freezing matters; HBC's own threads simply
wait.

A minimal `on_wait` for the start-up screen:

```cpp
// ctx: the StartUpProcess. No IOS call in here.
static bool startup_wait(u32 ms, usbguard_phase phase, void *ctx)
{
	StartUpProcess *p = (StartUpProcess *) ctx;
	u32 down = 0;

	PAD_ScanPads();
	for (int i = 0; i < 4; ++i)
		down |= PAD_ButtonsDown(i);

	if (phase == USBGUARD_PHASE_WAITING) {
		p->SetTextf("Waiting for USB devices\n");
	} else if (phase == USBGUARD_PHASE_STUCK) {
		p->SetTextf("%s\nWii Remotes do not work until it is unplugged.\n%s",
			USBGUARD_MSG_STUCK,
			usbguard_can_free() ? "GameCube controller: A = continue without USB\n" : "");
		if ((down & PAD_BUTTON_A) && usbguard_can_free())
			usbguard_free();          // the call then returns USBGUARD_FREED
	} else {                              // REPLUG
		p->SetTextf("Plug the USB drive back in.\nGameCube controller: B = go on without it\n");
		if (down & PAD_BUTTON_B)
			return false;             // no retry: USBGUARD_FREED
	}
	p->Draw();
	return true;
}
```

`SetTextf` and `Draw` are `StartUpProcess` members, so make `startup_wait`
a static member function (or a friend). Check that `SetTextf` does not log
to a file.

## 1. Start-up spin-up: `StartUpProcess.cpp:243-273`

In `StartUpProcess::USBSpinUp` (`StartUpProcess.cpp:228`), each pass of the 20 s loop calls `handle0->startup() && handle0->isInserted()`.
When the drive is stuck, the first `startup()` never returns. Run each pass
guarded instead. Use `USBStorage2_Init` directly so that d2x rc3's -101
reaches usbguard (`startup()` hides it):

```cpp
struct SpinUp { u32 port; const DISC_INTERFACE *handle; };

static s32 spinup_fn(void *arg)
{
	SpinUp *s = (SpinUp *) arg;
	s32 r = USBStorage2_Init(s->port);    // what handle->startup() does
	if (r < 0)
		return r;                     // -101: usbguard decides
	return s->handle->isInserted() ? 1 : 0;
}
```

In the loop:

```cpp
usbguard_opts o = USBGUARD_OPTS_INIT;
o.on_wait = startup_wait;
o.ctx = this;

if (handle0) {
	SpinUp s = { 0, handle0 };
	s32 r = usbguard_call(spinup_fn, &s, &o);
	if (r == USBGUARD_STUCK || r == USBGUARD_FREED) {
		drawCancel = false;
		return false;         // Execute goes on without USB; see "After STUCK" below
	}
	started0 = r > 0;
}
```

Do the same for `handle1` with `port` 1.

Before the loop, handle HBC's cut-off port: while
`usbguard_check() == USBGUARD_CUT_OFF`, show `USBGUARD_MSG_STUCK`. The check
goes back to OK once the drive is pulled; then wait for it to come back
(`usbguard_drive_present() == 1`) and about 1.5 s more before the first
`startup()`. B on the GameCube controller goes on without USB. Without this,
the 20 s loop only counts down, because the disabled port answers nothing.

## 2. The mount after `IosLoader::LoadAppCios`

`LoadAppCios` (`IosLoader.cpp:130`) has no mount of its own in this
checkout. The mounts that follow a cIOS load are these:

- `StartUpProcess::Execute`, `StartUpProcess.cpp:448-460`: `LoadAppCios`,
  then `DeviceHandler::Instance()->MountAllUSB()`.
- `IosLoader::LoadGameCios`, `IosLoader.cpp:170-185`: `ReloadIosSafe`, then
  `MountAllUSB(true)`.

`MountAllUSB` (`DeviceHandler.cpp:227`) calls `startup()` and
`isInserted()` (`DeviceHandler.cpp:181-184`) and mounts the partitions.
Wrap the whole call:

```cpp
static s32 mount_all_fn(void *arg)
{
	return DeviceHandler::Instance()->MountAllUSB((bool) (uintptr_t) arg) ? 1 : 0;
}

s32 r = usbguard_call(mount_all_fn, (void *) (uintptr_t) true, &o);
```

The spin-up's on_wait works here too. `usbguard_mount` does not fit:
USB Loader GX mounts through its own partition handler, not
`fatMountSimple`.

## 3. The cIOS reload: `IosLoader::ReloadIosKeepingRights`, `IosLoader.cpp:231-240`

A reload waits behind a stuck request too: 217 s on the dev Wii, until the
drive was pulled. Keep the AHBPROT patch and CrashReset on the caller's
thread, and run only `IOS_ReloadIOS` on the worker:

```cpp
static s32 reload_fn(void *arg) { return IOS_ReloadIOS((int) (intptr_t) arg); }

IosPatch_AHBPROT(false);
CrashReset_Enter();
usbguard_opts o = USBGUARD_OPTS_INIT;
o.on_wait = reload_wait;   // draws "Reloading IOS"; STUCK: the message
o.no_retry = true;         // never run a reload twice
s32 ret = usbguard_call(reload_fn, (void *) (intptr_t) ios, &o);
CrashReset_Leave();
```

Check that `CrashReset_Enter` covers a reload made on another thread; it
may hook the calling thread only. `usbguard_reload_ios` is the same call
without the patch and CrashReset.

- **If on_wait gives up during a reload,** IOS is half shut down. Leave only
  by a pull or `usbguard_free()`, not by `Sys_BackToLoader`, which reloads
  IOS too.
- **The other reload,** `runtimeiospatch.c:214`, is upstream library code.
  It runs before USB is touched, so it can stay as it is unless a start
  after a stuck drive shows it hanging.

## 4. The remounts: `HardDriveSM.cpp:116-118`, `PromptWindows.cpp:1207-1208`

Both call `UnMountAllUSB()` and then `MountAllUSB()` from a settings menu or
a prompt.

- **`MountAllUSB()`:** wrap it as in section 2, with an on_wait that draws
  the current window. Those windows read WPAD through `UpdatePads()`, so the
  on_wait must not; read the GameCube controller as above.
- **`UnMountAllUSB()`:** it flushes the FAT caches, which writes to the
  drive. Wrap it too if a drive that stopped answering after start-up is in
  scope.

## After STUCK or FREED

- **`USBGUARD_FREED`:** IOS is free again, but the drive is not mounted.
  After `usbguard_free`, the port stays disabled until the drive is
  replugged. Go on in SD mode or with no games, and say why.
- **`USBGUARD_STUCK`:** you gave up, or no worker was free. If `on_wait` gave
  up, the request is still stuck: make no IOS call until
  `!usbguard_blocked()`. That includes the SD card, WPAD and exiting to the
  loader. Keep drawing the message from your own loop and wait for the pull.
  STUCK also comes back after the worker returned (d2x's slow -101 with no
  AHBPROT, or a wait for the pull that ended); then `usbguard_blocked()` is
  already false but the drive is still in and not answering, so go on without
  USB.
- **Retry:** usbguard retries once by itself after a pull. A second
  failure comes back as FREED. Do not retry again in a loop: each stuck
  attempt holds one of the 4 workers until the drive is pulled.

## Testing it

- **Healthy drive:** start-up, a game launch (`LoadGameCios`) and both
  remount menus behave as before. A guarded call that answers at once never
  calls on_wait.
- **Stuck drive (attended):** a reset during a USB transfer, or d2xdiag's
  `usb-spin` scenario with its watchdog reset. Then start USB Loader GX
  straight from a loader that does not free the port. Expect the warning
  within 5 s, then:
  - a pull frees it, and a replug mounts on the retry;
  - A on a GameCube controller frees it (`USBGUARD_FREED`, SD mode).
- **HBC's cut-off port:** start from HBC after it freed a stuck drive. USB
  Loader GX shows the replug message straight away, not a 20 s countdown.
