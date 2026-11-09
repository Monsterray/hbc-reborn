// Host test for channel/channelapp/source/keyboard.c against a fake USB:
// prints one line per check, "ok NAME" or "FAIL NAME: why". The USB worker
// thread (kbd_worker) runs on a real host thread; the test waits for its looks
// with a bounded wait.

#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "../../../channel/channelapp/source/keymap.c"
#include "../../../channel/channelapp/source/keyboard.c"

// ---- What HBC's other files give keyboard.c, and its worker thread ----------

void bootlog_thread (const char *name) { (void) name; }
int bootlog_in (const char *call) { (void) call; return 0; }
void bootlog_out (int slot, s32 rc) { (void) slot; (void) rc; }
bool usbstart_stuck (void) { return false; }

typedef struct { void *(*entry) (void *); void *arg; } thread_start;
static thread_start start;
#ifdef _WIN32
static HANDLE thread;
static DWORD WINAPI thread_main (void *p) {
	(void) p;
	start.entry (start.arg);
	return 0;
}
#else
static pthread_t thread;
static void *thread_main (void *p) {
	(void) p;
	return start.entry (start.arg);
}
#endif

s32 LWP_CreateThread (lwp_t *h, void *(*entry) (void *), void *arg, void *stack, u32 size, u8 prio) {
	(void) stack; (void) size; (void) prio;
	start.entry = entry;
	start.arg = arg;
#ifdef _WIN32
	thread = CreateThread (NULL, 0, thread_main, NULL, 0, NULL);
	if (!thread)
		return -1;
#else
	if (pthread_create (&thread, NULL, thread_main, NULL))
		return -1;
#endif
	*h = 1;
	return 0;
}

s32 LWP_JoinThread (lwp_t h, void **value) {
	(void) h; (void) value;
#ifdef _WIN32
	WaitForSingleObject (thread, INFINITE);
	CloseHandle (thread);
#else
	pthread_join (thread, NULL);
#endif
	return 0;
}

// ---- The fake USB: devices 1 to 7, by ID ----------------------------------

static struct dev {
	bool present, keyboard;
	int open_fails;		// opens that fail before one works
	int opens;
	bool cancel_answers;	// USB_ClearHalt ends the pending read
	// The pending read: set by the worker's open, answered by the test's thread.
	usbcallback volatile cb;
	void *volatile arg;
	u8 *volatile buf;
	int stop_at;		// shut down (as keyboard_shutdown does) inside this USB step
	int handles;		// opened and not closed
} devs[8];

static volatile int descs;	// descriptor sets not given back

enum { STOP_NONE, STOP_OPEN, STOP_DESC, STOP_PROTOCOL };

static void stop_in (s32 id, int step) {
	if (devs[id].stop_at == step)
		stopping = true;
}

static volatile u32 looks;	// device lists asked for

// The worker sleeps between looks (a frame's time per call): shortened, so a
// look takes a few ms. keyboard_shutdown's 1 ms waits stay real.
void usleep (unsigned us) {
#ifdef _WIN32
	Sleep (us >= 10000 ? 0 : (us + 999) / 1000);
#else
	struct timespec t = { 0, us >= 10000 ? 100000 : us * 1000L };
	nanosleep (&t, NULL);
#endif
}

s32 USB_Initialize (void) { return 0; }

s32 USB_OpenDevice (s32 id, u16 vid, u16 pid, s32 *fd) {
	(void) vid; (void) pid;
	devs[id].opens++;
	if (!devs[id].present || devs[id].open_fails-- > 0)
		return -1;
	*fd = id;
	devs[id].handles++;
	stop_in (id, STOP_OPEN);
	return 0;
}

s32 USB_CloseDevice (s32 *fd) {
	devs[*fd].handles--;
	return 0;
}

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
	descs++;
	stop_in (fd, STOP_DESC);
	return 0;
}

void USB_FreeDescriptors (usb_devdesc *d) { (void) d; descs--; }

s32 USB_WriteCtrlMsg (s32 fd, u8 t, u8 r, u16 v, u16 i, u16 l, void *p) {
	(void) t; (void) r; (void) v; (void) i; (void) l; (void) p;
	stop_in (fd, STOP_PROTOCOL);
	return 0;
}

s32 USB_ReadIntrMsgAsync (s32 fd, u8 ep, u16 len, void *buf, usbcallback cb, void *arg) {
	(void) ep; (void) len;
	devs[fd].arg = arg;
	devs[fd].buf = buf;
	devs[fd].cb = cb;
	return 0;
}

s32 USB_GetDeviceList (usb_device_entry *list, u8 max, u8 cls, u8 *count) {
	s32 id;

	(void) cls;
	looks++;
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

// Waits (5 s at most) for the worker to look for devices, and to finish what
// that look opened: a look that began after this call has ended once the one
// after it begins, so three lists is enough.
static void next_look (void) {
	u32 start = looks;

	for (int t = 0; t < 5000 && looks < start + 3; t++)
		usleep (1000);
	keyboard_scan ();
}

// Waits (5 s at most) for the fake USB to hold a read for device id.
static bool wait_read (s32 id) {
	for (int t = 0; t < 5000 && !devs[id].cb; t++)
		usleep (1000);
	return devs[id].cb != NULL;
}

static int failed;

static void check (bool ok, const char *name, const char *why) {
	if (ok)
		printf ("ok %s\n", name);
	else
		printf ("FAIL %s: %s\n", name, why);
	failed |= !ok;
}

// Shutting down while kbd_open is between its USB steps (as keyboard_shutdown
// does when it gives up on the worker): the device is closed, its descriptors
// are freed and no slot is filled.
static void stop_during_open (int step, const char *name) {
	char why[64];
	u32 i;
	bool slot = false;

	memset (devs, 0, sizeof(devs));
	descs = 0;
	devs[5].present = devs[5].keyboard = true;
	devs[5].stop_at = step;
	keyboard_init ();
	for (int t = 0; t < 5000 && !worker_done; t++)
		usleep (1000);
	for (i = 0; i < KBD_MAX; i++)
		slot |= kbds[i].state != KBD_FREE;
	snprintf (why, sizeof why, "slot %d, handles %d, descriptors %d, read %d",
			  slot, devs[5].handles, descs, devs[5].cb != NULL);
	check (worker_done && !slot && !devs[5].handles && !descs && !devs[5].cb, name, why);
	keyboard_shutdown ();
}

int main (void) {
	devs[1].present = devs[1].keyboard = devs[1].cancel_answers = true;
	keyboard_init ();
	keyboard_scan ();
	check (wait_read (1), "open", "the keyboard is not being read");

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
	check (wait_read (2), "retry_failed_open", "a failed open was never tried again");

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
	check (wait_read (3), "stale_id_dropped", "a new keyboard under an old ID was not opened");

	// Shutdown: device 2's read never answers the cancel.
	devs[3].cancel_answers = true;
	keyboard_shutdown ();
	check (worker_done, "worker_ends", "keyboard_shutdown left the worker running");
	check (keyboard_pending (), "pending_after_shutdown", "a read IOS kept was not reported");
	devs[2].cb = NULL;
	kbds[1].state = KBD_FREE;
	check (!keyboard_pending (), "none_pending", "pending with no read left");

	stop_during_open (STOP_OPEN, "shutdown_after_open");
	stop_during_open (STOP_DESC, "shutdown_after_descriptors");
	stop_during_open (STOP_PROTOCOL, "shutdown_after_setprotocol");

	return failed;
}
