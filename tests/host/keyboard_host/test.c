// Host test for channel/channelapp/source/keyboard.c against a fake USB:
// prints one line per check, "ok NAME" or "FAIL NAME: why".

#include <stdio.h>
#include <string.h>

#include "../../../channel/channelapp/source/keymap.c"
#include "../../../channel/channelapp/source/keyboard.c"

// ---- The fake USB: devices 1 to 7, by ID ----------------------------------

static struct dev {
	bool present, keyboard;
	int open_fails;		// opens that fail before one works
	int opens;
	bool cancel_answers;	// USB_ClearHalt ends the pending read
	usbcallback cb;		// the pending read
	void *arg;
	u8 *buf;
} devs[8];

void usleep (unsigned us) { (void) us; }
s32 USB_Initialize (void) { return 0; }

s32 USB_OpenDevice (s32 id, u16 vid, u16 pid, s32 *fd) {
	(void) vid; (void) pid;
	devs[id].opens++;
	if (!devs[id].present || devs[id].open_fails-- > 0)
		return -1;
	*fd = id;
	return 0;
}

s32 USB_CloseDevice (s32 *fd) { (void) fd; return 0; }

s32 USB_GetDescriptors (s32 fd, usb_devdesc *d) {
	usb_interfacedesc *u = &d->configurations[0].interfaces[0];

	memset (d, 0, sizeof(*d));
	d->bNumConfigurations = 1;
	d->configurations[0].bNumInterfaces = 1;
	u->bInterfaceClass = USB_CLASS_HID;
	u->bInterfaceSubClass = USB_SUBCLASS_BOOT;
	u->bInterfaceProtocol = devs[fd].keyboard ? USB_PROTOCOL_KEYBOARD : 2;	// 2: a mouse
	u->bNumEndpoints = 1;
	u->endpoints[0].bmAttributes = USB_ENDPOINT_INTERRUPT;
	u->endpoints[0].bEndpointAddress = USB_ENDPOINT_IN | 1;
	return 0;
}

void USB_FreeDescriptors (usb_devdesc *d) { (void) d; }

s32 USB_WriteCtrlMsg (s32 fd, u8 t, u8 r, u16 v, u16 i, u16 l, void *p) {
	(void) fd; (void) t; (void) r; (void) v; (void) i; (void) l; (void) p;
	return 0;
}

s32 USB_ReadIntrMsgAsync (s32 fd, u8 ep, u16 len, void *buf, usbcallback cb, void *arg) {
	(void) ep; (void) len;
	devs[fd].cb = cb;
	devs[fd].arg = arg;
	devs[fd].buf = buf;
	return 0;
}

s32 USB_GetDeviceList (usb_device_entry *list, u8 max, u8 cls, u8 *count) {
	s32 id;

	(void) cls;
	*count = 0;
	for (id = 1; id < 8 && *count < max; id++)
		if (devs[id].present) {
			list[*count].device_id = id;
			list[*count].vid = list[*count].pid = 1;
			++*count;
		}
	return 0;
}

s32 USB_ClearHalt (s32 fd, u8 ep) {
	usbcallback cb = devs[fd].cb;

	(void) ep;
	if (devs[fd].cancel_answers && cb) {
		devs[fd].cb = NULL;
		cb (-1, devs[fd].arg);
	}
	return 0;
}

// IOS answers the read on device ID with this report.
static void report (s32 id, u8 k0, u8 k1, u8 k2) {
	usbcallback cb = devs[id].cb;
	u8 r[REPORT] = { 0, 0, k0, k1, k2 };

	devs[id].cb = NULL;
	memcpy (devs[id].buf, r, REPORT);
	cb (0, devs[id].arg);
}

// keyboard_scan until it has looked for devices again.
static void next_look (void) {
	u32 i;

	for (i = 0; i < LOOK_FRAMES; i++)
		keyboard_scan ();
}

static int failed;

static void check (bool ok, const char *name, const char *why) {
	if (ok)
		printf ("ok %s\n", name);
	else
		printf ("FAIL %s: %s\n", name, why);
	failed |= !ok;
}

int main (void) {
	devs[1].present = devs[1].keyboard = devs[1].cancel_answers = true;
	keyboard_init ();
	keyboard_scan ();	// looks: opens the keyboard
	check (devs[1].cb != NULL, "open", "the keyboard is not being read");

	// Rollover: Enter held, then a rollover report, then Enter alone.
	report (1, 0x28, 0, 0);
	keyboard_scan ();
	check (keyboard_down () == WPAD_BUTTON_A, "press", "Enter is not A down");
	report (1, 1, 1, 1);
	keyboard_scan ();
	check (keyboard_held () == WPAD_BUTTON_A && !keyboard_up (), "rollover_keeps_keys",
		   "a rollover report let go of Enter");
	check (devs[1].cb != NULL, "rollover_rearms", "no read after a rollover report");
	report (1, 0x28, 0, 0);
	keyboard_scan ();
	check (!keyboard_down (), "rollover_no_replay", "Enter came down again after the rollover");

	// A keyboard whose first open fails is tried again.
	devs[2].present = devs[2].keyboard = true;
	devs[2].open_fails = 1;
	next_look ();
	next_look ();
	check (devs[2].cb != NULL, "retry_failed_open", "a failed open was never tried again");

	// A device that is not a keyboard is opened once, however often HBC looks.
	devs[3].present = true;
	next_look ();
	next_look ();
	next_look ();
	check (devs[3].opens == 1, "non_keyboard_once", "a mouse was opened more than once");

	// Unplugged, its ID is forgotten; a keyboard plugged in under that ID
	// later is opened.
	devs[3].present = false;
	next_look ();
	devs[3].present = devs[3].keyboard = true;
	next_look ();
	check (devs[3].cb != NULL, "stale_id_dropped", "a new keyboard under an old ID was not opened");

	// Shutdown: device 2's read never answers the cancel.
	devs[3].cancel_answers = true;
	keyboard_shutdown ();
	check (keyboard_pending (), "pending_after_shutdown", "a read IOS kept was not reported");
	devs[2].cb = NULL;
	kbds[1].state = KBD_FREE;
	check (!keyboard_pending (), "none_pending", "pending with no read left");

	return failed;
}
