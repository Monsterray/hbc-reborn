// A USB keyboard (keyboard.h): every boot-protocol keyboard interface read
// with interrupt requests that IOS answers as keys change, and the keys they
// report gathered once a frame.
//
// Not libwiikeyboard: it opens only the first boot keyboard interface it
// finds and waits in IOS for its reports. A keyboard with two (a Chicony
// 04f2:0841 lists its second, silent one first) then never types, and the
// wait holds up HBC's exit until a key is pressed.

#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/usb.h>
#include <ogc/machine/processor.h>

#include "keyboard.h"

#define KBD_MAX 4		// keyboard interfaces read at once
#define REPORT 8		// a boot keyboard report: modifiers, 0, six keys
#define LOOK_FRAMES 60	// how often keyboard_scan looks for new ones

enum { KBD_FREE, KBD_READING, KBD_ENDED };

static struct kbd {
	s32 device_id, fd;
	u8 ep;
	volatile u8 state;
	u8 keys[REPORT];	// the last report, all zero once it ends
	// IOS writes here by DMA, and libogc invalidates the cache line(s) of
	// the buffer. It has a whole 32-byte line to itself (nothing else in
	// the line: an invalidate would drop the CPU's writes to it).
	u8 report[32] ATTRIBUTE_ALIGN (32);
} kbds[KBD_MAX];

_Static_assert(offsetof (struct kbd, report) % 32 == 0 && sizeof (struct kbd) % 32 == 0 &&
		sizeof ((struct kbd *) 0)->report == 32 && REPORT <= 32,
		"kbd.report must own its cache line");

// Devices proven not to be keyboards (their descriptors have no boot
// keyboard interface), so they are opened once. One that could not be
// opened or read is tried again the next time.
static s32 seen[16];
static u32 seen_count;

// Keys that went down since the last scan, from the callback; and the
// frame's copies.
static u8 keys_pressed[KEYMAP_BYTES];
static u8 keys_now[KEYMAP_BYTES], keys_new[KEYMAP_BYTES];
static u32 key_held, key_down, key_up;
static volatile bool stopping;
static bool inited;
static u32 frames;

// IOS's answer to a read, in the IPC interrupt: keep the report and ask for
// the next one. A failure is an unplugged keyboard or keyboard_shutdown.
static s32 kbd_read_cb (s32 res, void *arg) {
	struct kbd *k = arg;
	u32 i, j;

	if(res < 0 || stopping) {
		memset (k->keys, 0, REPORT);
		k->state = KBD_ENDED;
		return 0;
	}

	// Too many keys at once: every key slot says ErrorRollOver (1). The
	// keys are unknown, not up: keep the last ones, or each would go up and
	// then come down again as a new press when the rollover ends.
	if(k->report[2] == 1)
		goto rearm;

	for(i = 2; i < REPORT; i++) {
		if(k->report[i] < 4)	// none, or a rollover error
			continue;
		for(j = 2; j < REPORT && k->keys[j] != k->report[i]; j++)
			;
		if(j == REPORT)
			KEYMAP_SET (keys_pressed, k->report[i]);
	}
	memcpy (k->keys, k->report, REPORT);

rearm:
	if(USB_ReadIntrMsgAsync (k->fd, k->ep, REPORT, k->report, kbd_read_cb, k) < 0) {
		memset (k->keys, 0, REPORT);
		k->state = KBD_ENDED;
	}
	return 0;
}

static bool kbd_seen (s32 device_id) {
	u32 i;

	for(i = 0; i < KBD_MAX; i++)
		if(kbds[i].state != KBD_FREE && kbds[i].device_id == device_id)
			return true;
	for(i = 0; i < seen_count; i++)
		if(seen[i] == device_id)
			return true;
	return false;
}

// Opens one device's boot keyboard interface, if it has one, into a free
// slot. False only when the device is proven not to be a keyboard.
static bool kbd_open (const usb_device_entry *e) {
	usb_devdesc d;
	usb_interfacedesc *u;
	struct kbd *k = NULL;
	s32 fd;
	u32 c, f, p, i;

	for(i = 0; i < KBD_MAX && !k; i++)
		if(kbds[i].state == KBD_FREE)
			k = &kbds[i];
	if(!k || USB_OpenDevice (e->device_id, e->vid, e->pid, &fd) < 0)
		return true;
	if(USB_GetDescriptors (fd, &d) < 0) {
		USB_CloseDevice (&fd);
		return true;
	}

	for(c = 0; c < d.bNumConfigurations; c++)
		for(f = 0; f < d.configurations[c].bNumInterfaces; f++) {
			u = &d.configurations[c].interfaces[f];
			if(u->bInterfaceClass != USB_CLASS_HID ||
					u->bInterfaceSubClass != USB_SUBCLASS_BOOT ||
					u->bInterfaceProtocol != USB_PROTOCOL_KEYBOARD)
				continue;
			for(p = 0; p < u->bNumEndpoints; p++) {
				if(u->endpoints[p].bmAttributes != USB_ENDPOINT_INTERRUPT ||
						!(u->endpoints[p].bEndpointAddress & USB_ENDPOINT_IN))
					continue;
				// Boot reports, whatever the keyboard would send otherwise.
				USB_WriteCtrlMsg (fd, USB_REQTYPE_INTERFACE_SET,
								  USB_REQ_SETPROTOCOL, 0, u->bInterfaceNumber, 0, NULL);
				k->device_id = e->device_id;
				k->fd = fd;
				k->ep = u->endpoints[p].bEndpointAddress;
				memset (k->keys, 0, REPORT);
				k->state = KBD_READING;
				if(USB_ReadIntrMsgAsync (fd, k->ep, REPORT, k->report, kbd_read_cb, k) < 0)
					k->state = KBD_ENDED;
				USB_FreeDescriptors (&d);
				return true;
			}
		}

	USB_FreeDescriptors (&d);
	USB_CloseDevice (&fd);
	return false;
}

// New keyboards, and slots of ended ones back to free.
static void kbd_look (void) {
	static usb_device_entry list[16] ATTRIBUTE_ALIGN (32);
	u8 count = 0;
	u32 i;

	u32 j, kept;

	for(i = 0; i < KBD_MAX; i++)
		if(kbds[i].state == KBD_ENDED) {
			USB_CloseDevice (&kbds[i].fd);
			kbds[i].state = KBD_FREE;
		}

	if(USB_GetDeviceList (list, 16, USB_CLASS_HID, &count) < 0)
		return;
	// Forget devices that are gone: their IDs can come back on another one.
	for(i = kept = 0; i < seen_count; i++) {
		for(j = 0; j < count && list[j].device_id != seen[i]; j++)
			;
		if(j < count)
			seen[kept++] = seen[i];
	}
	seen_count = kept;
	for(i = 0; i < count; i++)
		if(list[i].vid && list[i].pid && !kbd_seen (list[i].device_id) &&
				!kbd_open (&list[i]) && seen_count < sizeof(seen) / sizeof(*seen))
			seen[seen_count++] = list[i].device_id;
}

void keyboard_init (void) {
	if(inited)
		return;
	// HBC starts USB only when it looks for a drive (usbmsd.c); a second
	// call does nothing.
	if(USB_Initialize () < 0)
		return;
	memset (kbds, 0, sizeof(kbds));
	memset (keys_pressed, 0, sizeof(keys_pressed));
	key_held = key_down = key_up = 0;
	seen_count = 0;
	frames = 0;
	stopping = false;
	inited = true;
}

void keyboard_shutdown (void) {
	u32 i, t;

	if(!inited)
		return;
	stopping = true;
	for(i = 0; i < KBD_MAX; i++) {
		if(kbds[i].state == KBD_FREE)
			continue;
		// Cancels the read IOS holds (USBV5_CancelEndpoint on this IOS).
		if(kbds[i].state == KBD_READING)
			USB_ClearHalt (kbds[i].fd, kbds[i].ep);
		for(t = 0; t < 100 && kbds[i].state == KBD_READING; t++)
			usleep (1000);
		if(kbds[i].state == KBD_READING)
			continue;	// never answered: leave it open rather than wait
		USB_CloseDevice (&kbds[i].fd);
		kbds[i].state = KBD_FREE;
	}
	inited = false;
}

void keyboard_scan (void) {
	u32 held, down, level, i, j;

	if(!inited)
		return;
	if(frames++ % LOOK_FRAMES == 0)
		kbd_look ();

	memset (keys_now, 0, sizeof(keys_now));
	_CPU_ISR_Disable (level);
	for(i = 0; i < KBD_MAX; i++)
		for(j = 2; j < REPORT; j++)
			if(kbds[i].keys[j] >= 4)
				KEYMAP_SET (keys_now, kbds[i].keys[j]);
	memcpy (keys_new, keys_pressed, sizeof(keys_new));
	memset (keys_pressed, 0, sizeof(keys_pressed));
	_CPU_ISR_Restore (level);

	held = keymap_buttons (keys_now);
	down = keymap_buttons (keys_new);
	key_up = key_held & ~held;
	key_held = held;
	key_down = down;
}

bool keyboard_pending (void) {
	u32 i;

	for(i = 0; i < KBD_MAX; i++)
		if(kbds[i].state == KBD_READING)
			return true;
	return false;
}

u32 keyboard_held (void) { return key_held; }
u32 keyboard_down (void) { return key_down; }
u32 keyboard_up (void) { return key_up; }

bool keyboard_key_down (u8 usage) {
	return inited && KEYMAP_GET (keys_new, usage);
}
