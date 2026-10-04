#ifndef _USBMSD_H_
#define _USBMSD_H_

#include <gctypes.h>
#include <ogc/disc_io.h>

// Two USB mass storage drives at once (usbmsd.c): slot 0 is "usb", slot 1
// "usb2". The first two bulk-only drives that mount, in IOS's order; a drive
// keeps its slot while it stays plugged in.
#define USBMSD_DRIVES 2

extern DISC_INTERFACE usbmsd_disc[USBMSD_DRIVES];

// The drive in a slot, while mounted: vendor and product ID, and its size in
// 512-byte sectors (when sectors is not NULL).
bool usbmsd_info(int slot, u16 *vid, u16 *pid, u32 *sectors);
// HBCS: ,"usb_drives":[...]: each slot's drive, or why it has none.
int usbmsd_json(char *buf, int size);

#endif
