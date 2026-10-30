// The controllers WPAD and PAD do not read (wiiinput.h): each driver's state
// gathered once a frame. The keyboard and mouse parts are libwiiinput's
// (github.com/quatric/libwiiinput), rewritten for this tree.

#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/usb.h>
#include <ogc/usbmouse.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/usbkeyboard.h>

#include "wiiinput.h"

static bool inited;
static u32 flags;
static const char *source = "";

// ---- Wii U Pro Controllers and the GameCube adapter ----

static u32 classic_held, classic_down, classic_up;
static u16 gc_held, gc_down, gc_up;
static s32 stick_x, stick_y;

// The stick pushed furthest wins, in GameCube stick units.
static void stick_feed(s32 x, s32 y) {
	if (x * x + y * y > stick_x * stick_x + stick_y * stick_y) {
		stick_x = x;
		stick_y = y;
	}
}

static void scan_pads(void) {
	u32 held = 0;
	u16 gheld = 0;
	int i;

	stick_x = stick_y = 0;

	for (i = 0; i < 4; ++i) {
		wiiinput_pro_pad p;

		if (!wiiinput_wupc_pad(i, &p))
			continue;
		held |= p.buttons;
		// About +-1024 against a GameCube stick's +-100.
		stick_feed(p.lx / 10, p.ly / 10);
	}

	wiiinput_gcadapter_update();
	for (i = 0; i < 4; ++i) {
		wiiinput_gc_pad p;

		if (!wiiinput_gcadapter_pad(i, &p))
			continue;
		gheld |= p.buttons;
		stick_feed(p.stick_x, p.stick_y);
	}

	classic_down = held & ~classic_held;
	classic_up = classic_held & ~held;
	classic_held = held;
	if (classic_down)
		source = "Wii U Pro";

	gc_down = gheld & ~gc_held;
	gc_up = gc_held & ~gheld;
	gc_held = gheld;
	if (gc_down)
		source = "GameCube adapter";
}

u32 wiiinput_classic_held(void) { return classic_held; }
u32 wiiinput_classic_down(void) { return classic_down; }
u32 wiiinput_classic_up(void) { return classic_up; }
u16 wiiinput_gc_held(void) { return gc_held; }
u16 wiiinput_gc_down(void) { return gc_down; }
u16 wiiinput_gc_up(void) { return gc_up; }
s32 wiiinput_stick_x(void) { return stick_x; }
s32 wiiinput_stick_y(void) { return stick_y; }

void wiiinput_rumble(bool on) {
	int i;

	for (i = 0; i < 4; ++i) {
		wiiinput_wupc_rumble(i, on);
		wiiinput_gcadapter_rumble(i, on);
	}
}

// ---- The keyboard ----
//
// A thread keeps it open (it can be plugged in at any time) and scans it:
// USBKeyboard_Scan waits in IOS for the next report, so it cannot run in
// the frame loop. Key states are bits by USB HID usage code.

#define KEY_BYTES (256 / 8)
#define KEY_GET(a, k) ((a)[(k) >> 3] & (1 << ((k) & 7)))
#define KEY_SET(a, k) ((a)[(k) >> 3] |= (1 << ((k) & 7)))
#define KEY_CLR(a, k) ((a)[(k) >> 3] &= ~(1 << ((k) & 7)))

static u8 keys_live[KEY_BYTES], keys_pressed[KEY_BYTES];
static u8 keys_now[KEY_BYTES], keys_new[KEY_BYTES];
static u32 key_held, key_down, key_up;
static mutex_t kbd_lock = LWP_MUTEX_NULL;
static lwp_t kbd_thread = LWP_THREAD_NULL;
static volatile bool kbd_run;
static volatile bool kbd_lost;
static bool kbd_inited;

static const struct {
	u8 usage;
	u32 button;
} key_map[] = {
	{ 0x52, WPAD_BUTTON_UP }, { 0x51, WPAD_BUTTON_DOWN },
	{ 0x50, WPAD_BUTTON_LEFT }, { 0x4f, WPAD_BUTTON_RIGHT },
	{ 0x28, WPAD_BUTTON_A }, { 0x58, WPAD_BUTTON_A },		// Enter, keypad Enter
	{ 0x2c, WPAD_BUTTON_A },								// Space
	{ 0x2a, WPAD_BUTTON_B },								// Backspace
	{ 0x1b, WPAD_BUTTON_1 }, { 0x1c, WPAD_BUTTON_2 },		// X, Y
	{ 0x1e, WPAD_BUTTON_1 }, { 0x1f, WPAD_BUTTON_2 },		// 1, 2
	{ 0x2d, WPAD_BUTTON_MINUS }, { 0x56, WPAD_BUTTON_MINUS },	// -, keypad -
	{ 0x4c, WPAD_BUTTON_MINUS },							// Delete
	{ 0x2e, WPAD_BUTTON_PLUS }, { 0x57, WPAD_BUTTON_PLUS },	// =/+, keypad +
	{ 0x49, WPAD_BUTTON_PLUS },								// Insert
	{ 0x29, WPAD_BUTTON_HOME }, { 0x4a, WPAD_BUTTON_HOME },	// Esc, Home
};

// The scan thread's callback. A disconnect comes from an IPC callback,
// where the mutex cannot be taken: the thread lets go of the keys then.
static void kbd_event(USBKeyboard_event ev) {
	if (ev.type == USBKEYBOARD_DISCONNECTED) {
		kbd_lost = true;
		return;
	}
	LWP_MutexLock(kbd_lock);
	if (ev.type == USBKEYBOARD_PRESSED) {
		if (!KEY_GET(keys_live, ev.keyCode))
			KEY_SET(keys_pressed, ev.keyCode);
		KEY_SET(keys_live, ev.keyCode);
	} else if (ev.type == USBKEYBOARD_RELEASED) {
		KEY_CLR(keys_live, ev.keyCode);
	}
	LWP_MutexUnlock(kbd_lock);
}

static void *kbd_func(void *arg) {
	while (kbd_run) {
		if (kbd_lost) {
			kbd_lost = false;
			LWP_MutexLock(kbd_lock);
			memset(keys_live, 0, sizeof(keys_live));
			LWP_MutexUnlock(kbd_lock);
		}
		if (!USBKeyboard_IsConnected()) {
			// Looking walks IOS's USB device list: a few times a second.
			if (USBKeyboard_Open(kbd_event) <= 0) {
				usleep(250 * 1000);
				continue;
			}
		}
		if (USBKeyboard_Scan() < 0)
			usleep(10 * 1000);
		else
			usleep(400);
	}
	return NULL;
}

static void kbd_init(void) {
	if (USBKeyboard_Initialize() < 0)
		return;
	if (LWP_MutexInit(&kbd_lock, false) < 0) {
		USBKeyboard_Deinitialize();
		return;
	}
	memset(keys_live, 0, sizeof(keys_live));
	memset(keys_pressed, 0, sizeof(keys_pressed));
	kbd_run = true;
	if (LWP_CreateThread(&kbd_thread, kbd_func, NULL, NULL, 16 * 1024, 64) < 0) {
		kbd_run = false;
		kbd_thread = LWP_THREAD_NULL;
		LWP_MutexDestroy(kbd_lock);
		kbd_lock = LWP_MUTEX_NULL;
		USBKeyboard_Deinitialize();
		return;
	}
	kbd_inited = true;
}

static void kbd_deinit(void) {
	if (!kbd_inited)
		return;
	kbd_run = false;
	LWP_JoinThread(kbd_thread, NULL);
	kbd_thread = LWP_THREAD_NULL;
	USBKeyboard_Close();
	USBKeyboard_Deinitialize();
	LWP_MutexDestroy(kbd_lock);
	kbd_lock = LWP_MUTEX_NULL;
	kbd_inited = false;
}

static void scan_keyboard(void) {
	u32 held = 0, down = 0;
	size_t i;

	if (!kbd_inited)
		return;

	LWP_MutexLock(kbd_lock);
	// A key pressed and let go between two scans still counts as down.
	memcpy(keys_now, keys_live, sizeof(keys_now));
	memcpy(keys_new, keys_pressed, sizeof(keys_new));
	memset(keys_pressed, 0, sizeof(keys_pressed));
	LWP_MutexUnlock(kbd_lock);

	for (i = 0; i < sizeof(key_map) / sizeof(key_map[0]); ++i) {
		if (KEY_GET(keys_now, key_map[i].usage))
			held |= key_map[i].button;
		if (KEY_GET(keys_new, key_map[i].usage))
			down |= key_map[i].button;
	}
	key_up = key_held & ~held;
	key_held = held;
	key_down = down;
	for (i = 0; i < KEY_BYTES; ++i)
		if (keys_new[i]) {
			source = "Keyboard";
			break;
		}
}

u32 wiiinput_keys_held(void) { return key_held; }
u32 wiiinput_keys_down(void) { return key_down; }
u32 wiiinput_keys_up(void) { return key_up; }

bool wiiinput_key_down(u8 usage) {
	return kbd_inited && KEY_GET(keys_new, usage);
}

// ---- The mouse ----
//
// libogc's mouse driver finds the mouse by itself when it is plugged in.

static bool mouse_inited;
static int mouse_w = 640, mouse_h = 480;
static s32 mouse_x = 320, mouse_y = 240, mouse_wheel;
static u8 mouse_btn_held, mouse_btn_down, mouse_btn_up;
static bool mouse_move;

static s32 clamp(s32 v, s32 lo, s32 hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

void wiiinput_mouse_area(int width, int height) {
	if (width < 1 || height < 1)
		return;
	mouse_w = width;
	mouse_h = height;
	mouse_x = clamp(mouse_x, 0, mouse_w - 1);
	mouse_y = clamp(mouse_y, 0, mouse_h - 1);
}

static void scan_mouse(void) {
	mouse_event ev;
	s32 dx = 0, dy = 0;
	u8 prev = mouse_btn_held;
	int n;

	mouse_wheel = 0;
	mouse_move = false;
	if (!mouse_inited)
		return;
	if (!MOUSE_IsConnected()) {
		mouse_btn_held = 0;
	} else {
		for (n = 0; n < 64 && MOUSE_GetEvent(&ev) > 0; ++n) {
			dx += ev.rx;
			dy += ev.ry;
			mouse_wheel += ev.rz;
			mouse_btn_held = ev.button & 7;
		}
	}
	mouse_x = clamp(mouse_x + dx, 0, mouse_w - 1);
	mouse_y = clamp(mouse_y + dy, 0, mouse_h - 1);
	mouse_btn_down = mouse_btn_held & ~prev;
	mouse_btn_up = prev & ~mouse_btn_held;
	mouse_move = dx || dy || mouse_btn_down;
	if (mouse_move)
		source = "Mouse";
}

bool wiiinput_mouse(s32 *x, s32 *y) {
	if (!mouse_inited || !MOUSE_IsConnected())
		return false;
	if (x)
		*x = mouse_x;
	if (y)
		*y = mouse_y;
	return true;
}

bool wiiinput_mouse_moved(void) { return mouse_move; }

void wiiinput_mouse_warp(s32 x, s32 y) {
	mouse_x = clamp(x, 0, mouse_w - 1);
	mouse_y = clamp(y, 0, mouse_h - 1);
}

u8 wiiinput_mouse_held(void) { return mouse_btn_held; }
u8 wiiinput_mouse_down(void) { return mouse_btn_down; }
u8 wiiinput_mouse_up(void) { return mouse_btn_up; }
s32 wiiinput_mouse_wheel(void) { return mouse_wheel; }

// ---- All of it ----

void wiiinput_init(u32 f) {
	if (inited)
		return;
	flags = f;
	USB_Initialize();
	if (!(flags & WIIINPUT_NO_WUPC))
		wiiinput_wupc_init();
	wiiinput_gcadapter_init(!(flags & WIIINPUT_NO_GCADAPTER));
	if (!(flags & WIIINPUT_NO_KEYBOARD))
		kbd_init();
	if (!(flags & WIIINPUT_NO_MOUSE))
		mouse_inited = MOUSE_Init() >= 0;
	inited = true;
}

void wiiinput_shutdown(void) {
	if (!inited)
		return;
	wiiinput_rumble(false);
	wiiinput_gcadapter_shutdown();
	kbd_deinit();
	if (mouse_inited)
		MOUSE_Deinit();
	mouse_inited = false;
	wiiinput_wupc_shutdown();
	classic_held = classic_down = classic_up = 0;
	gc_held = gc_down = gc_up = 0;
	key_held = key_down = key_up = 0;
	inited = false;
}

void wiiinput_scan(void) {
	if (!inited)
		return;
	scan_pads();
	scan_keyboard();
	scan_mouse();
}

const char *wiiinput_source(void) {
	return source;
}
