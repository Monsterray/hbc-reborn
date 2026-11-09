#ifndef _USBSTART_H_
#define _USBSTART_H_

#include <gctypes.h>

// HBC's start-up policy for a stuck USB drive, on top of sdk/usbguard.
//
// A USB drive that a reset left mid-transfer can wedge IOS58's USB module
// until the drive is unplugged. While USB requests wait in it, every other
// IOS request waits too (ES, NAND, the network, Bluetooth, IOS_ReloadIOS).
// main_pre therefore asks USB one question first, and acts on the answer:
//  1. usbstart_check: does USB answer within `ms`? If so, nothing else here
//     matters.
//  2. usbstart_unplug (needs AHBPROT and a drive on an EHCI port): the ports
//     are disabled in software and the stuck request fails as when the drive
//     is pulled.
//  3. Not possible, or still stuck: main_pre reloads IOS80, which has no
//     EHCI driver, and tells us with usbstart_ios_reloaded.
// The rest of HBC asks the predicates below before it touches USB.

// Whether IOS's USB module answers the requests WPAD_Init starts with
// (/dev/usb/ven, its version, the Bluetooth device), all within `ms`. Nothing
// else should ask USB anything when this says false: the request that got no
// answer stays pending in IOS.
bool usbstart_check(u32 ms);
// usbstart_check said no: HBC starts without USB drives and keyboards.
bool usbstart_stuck(void);

// After usbstart_check said no: disables every EHCI port with a drive and
// waits up to `ms` for the stuck request to answer. Disabling a port frees
// IOS, as pulling the drive does; a reload or handing the port to the
// companion controller does not. The registers are written directly
// (AHBPROT), so this does not queue behind IOS. True: the stuck request
// answered. The drive is not seen at USB 2.0 again until it is replugged or
// IOS resets the controller.
bool usbstart_unplug(u32 ms);
// usbstart_unplug freed USB: Wii Remotes work, USB drives stay off.
bool usbstart_unplugged(void);

// HBC left the stuck IOS for one whose USB answers (main_pre).
void usbstart_ios_reloaded(void);
// USB is stuck on the IOS HBC runs on: no Wii Remotes either (Bluetooth is
// /dev/usb/oh1, the stuck module); the GameCube controller still works.
bool usbstart_remotes_off(void);

// USB was stuck at start-up and the drive has not been unplugged since: an
// app that reloads IOS would hang on it (IOS58, d2x rc1 and older cIOSes;
// d2x rc3 gives up after about 7 s). Only a pull or a replug clears the
// drive, not a reset. Reads the ports usbstart_unplug disabled on each call;
// once it sees the drive out it stays false. HBC asks before a menu launch
// and refuses a network one while this is true.
bool usbstart_replug_needed(void);

// ,"ehci":"..." for HBCS: the EHCI registers at each unplug step (none: 0
// bytes).
int usbstart_ehci_json(char *buf, int size);

#endif
