// A USB keyboard (keyboard.h): libwiikeyboard's driver kept open and scanned
// by a thread, and the keys it reports gathered once a frame.

#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <wiikeyboard/usbkeyboard.h>

#include "keyboard.h"

// USBKeyboard_Scan waits in IOS for the next report, so it cannot run in the
// frame loop. "live" is what is down now, "pressed" what went down since the
// last scan; the other two are the frame's copies.
static u8 keys_live[KEYMAP_BYTES], keys_pressed[KEYMAP_BYTES];
static u8 keys_now[KEYMAP_BYTES], keys_new[KEYMAP_BYTES];
static u32 key_held, key_down, key_up;
static mutex_t kbd_lock = LWP_MUTEX_NULL;
static lwp_t kbd_thread = LWP_THREAD_NULL;
static volatile bool kbd_run;
static volatile bool kbd_lost;
static bool kbd_inited;

// The scan thread's callback. A disconnect comes from an IPC callback, where
// the mutex cannot be taken: the thread lets go of the keys then.
static void kbd_event (USBKeyboard_event ev) {
	if(ev.type == USBKEYBOARD_DISCONNECTED) {
		kbd_lost = true;
		return;
	}
	LWP_MutexLock (kbd_lock);
	if(ev.type == USBKEYBOARD_PRESSED) {
		if(!KEYMAP_GET (keys_live, ev.keyCode))
			KEYMAP_SET (keys_pressed, ev.keyCode);
		KEYMAP_SET (keys_live, ev.keyCode);
	} else if(ev.type == USBKEYBOARD_RELEASED) {
		KEYMAP_CLR (keys_live, ev.keyCode);
	}
	LWP_MutexUnlock (kbd_lock);
}

static void *kbd_func (void *arg) {
	(void) arg;
	while(kbd_run) {
		if(kbd_lost) {
			kbd_lost = false;
			LWP_MutexLock (kbd_lock);
			memset (keys_live, 0, sizeof(keys_live));
			LWP_MutexUnlock (kbd_lock);
		}
		if(!USBKeyboard_IsConnected ()) {
			// Looking walks IOS's USB device list: a few times a second.
			if(USBKeyboard_Open (kbd_event) <= 0) {
				usleep (250 * 1000);
				continue;
			}
		}
		if(USBKeyboard_Scan () < 0)
			usleep (10 * 1000);
		else
			usleep (400);
	}
	return NULL;
}

void keyboard_init (void) {
	if(kbd_inited)
		return;
	if(USBKeyboard_Initialize () < 0)
		return;
	if(LWP_MutexInit (&kbd_lock, false) < 0) {
		USBKeyboard_Deinitialize ();
		return;
	}
	memset (keys_live, 0, sizeof(keys_live));
	memset (keys_pressed, 0, sizeof(keys_pressed));
	key_held = key_down = key_up = 0;
	kbd_run = true;
	if(LWP_CreateThread (&kbd_thread, kbd_func, NULL, NULL, 16 * 1024, 64) < 0) {
		kbd_run = false;
		kbd_thread = LWP_THREAD_NULL;
		LWP_MutexDestroy (kbd_lock);
		kbd_lock = LWP_MUTEX_NULL;
		USBKeyboard_Deinitialize ();
		return;
	}
	kbd_inited = true;
}

void keyboard_shutdown (void) {
	if(!kbd_inited)
		return;
	kbd_run = false;
	LWP_JoinThread (kbd_thread, NULL);
	kbd_thread = LWP_THREAD_NULL;
	USBKeyboard_Close ();
	USBKeyboard_Deinitialize ();
	LWP_MutexDestroy (kbd_lock);
	kbd_lock = LWP_MUTEX_NULL;
	kbd_inited = false;
}

void keyboard_scan (void) {
	u32 held, down;

	if(!kbd_inited)
		return;

	LWP_MutexLock (kbd_lock);
	memcpy (keys_now, keys_live, sizeof(keys_now));
	memcpy (keys_new, keys_pressed, sizeof(keys_new));
	memset (keys_pressed, 0, sizeof(keys_pressed));
	LWP_MutexUnlock (kbd_lock);

	held = keymap_buttons (keys_now);
	down = keymap_buttons (keys_new);
	key_up = key_held & ~held;
	key_held = held;
	key_down = down;
}

u32 keyboard_held (void) { return key_held; }
u32 keyboard_down (void) { return key_down; }
u32 keyboard_up (void) { return key_up; }

bool keyboard_key_down (u8 usage) {
	return kbd_inited && KEYMAP_GET (keys_new, usage);
}
