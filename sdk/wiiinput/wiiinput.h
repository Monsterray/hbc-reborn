// Controllers the Wii's own WPAD and PAD libraries do not read, for HBC's
// controls.c (and any app that wants them): a rewrite of libwiiinput
// (github.com/quatric/libwiiinput) for this tree. See README.md.
//
//  - the Wii U Pro Controller, over Bluetooth like a Wii Remote; it reads
//    as a Classic Controller (WPAD_CLASSIC_BUTTON_* bits)
//  - the GameCube controller adapter for Wii U (WUP-028, and the adapters
//    that copy it), its four ports read as GameCube pads (PAD_* bits)
//  - a USB keyboard, as a Wii Remote (WPAD_BUTTON_* bits)
//  - a USB mouse, as a pointer and A/B
//  - a fourth Wii Remote or a Balance Board left on, which crash libogc 3.1
//    as they connect (wpadqueue.c, linked with --wrap=wiiuse_init_cmd_queue)
//
// Link with -Wl,--wrap=wiiuse_accept,--wrap=wiiuse_init_cmd_queue and
// -lwiikeyboard. Call wiiinput_init() before WPAD_Init() and
// wiiinput_shutdown() before WPAD_Shutdown().

#ifndef _WIIINPUT_H_
#define _WIIINPUT_H_

#include <gctypes.h>

// wiiinput_init() flags
#define WIIINPUT_NO_WUPC		(1u << 0)
#define WIIINPUT_NO_GCADAPTER	(1u << 1)
#define WIIINPUT_NO_KEYBOARD	(1u << 2)
#define WIIINPUT_NO_MOUSE		(1u << 3)

// Starts libogc's USB and every driver not turned off by a flag. Devices
// can be plugged in, and Pro Controllers turned on, at any time after.
void wiiinput_init(u32 flags);
void wiiinput_shutdown(void);

// Once a frame, before reading anything below: "down" and "up" are what
// changed since the previous call.
void wiiinput_scan(void);

// Wii U Pro Controllers, all of them together: WPAD_CLASSIC_BUTTON_* bits.
u32 wiiinput_classic_held(void);
u32 wiiinput_classic_down(void);
u32 wiiinput_classic_up(void);

// The GameCube adapter's four ports together: PAD_BUTTON_* / PAD_TRIGGER_*.
u16 wiiinput_gc_held(void);
u16 wiiinput_gc_down(void);
u16 wiiinput_gc_up(void);

// The keyboard: arrows are the D-pad, Enter A, Backspace B, X 1, Y 2,
// - and + (or Delete and Insert) Minus and Plus, Esc and Home HOME.
// WPAD_BUTTON_* bits. wiiinput_key_down() is a USB HID usage code pressed
// since the last scan, for keys with no Wii Remote button.
u32 wiiinput_keys_held(void);
u32 wiiinput_keys_down(void);
u32 wiiinput_keys_up(void);
bool wiiinput_key_down(u8 usage);

// The left stick that is pushed furthest, Pro Controller or adapter, in a
// GameCube stick's units (about -100..100, y up). 0 when none is pushed.
s32 wiiinput_stick_x(void);
s32 wiiinput_stick_y(void);

// The USB mouse: its position, kept within the screen size given here
// (it starts in the middle), and its buttons, bit 0 left, 1 right, 2
// middle. wiiinput_mouse_moved() says whether it moved or clicked in the
// last scan, so a pointer can hand over between it and a Wii Remote;
// wiiinput_mouse_warp() puts it where the other pointer was.
void wiiinput_mouse_area(int width, int height);
bool wiiinput_mouse(s32 *x, s32 *y);
bool wiiinput_mouse_moved(void);
void wiiinput_mouse_warp(s32 x, s32 y);
u8 wiiinput_mouse_held(void);
u8 wiiinput_mouse_down(void);
u8 wiiinput_mouse_up(void);
s32 wiiinput_mouse_wheel(void);

#define WIIINPUT_MOUSE_LEFT		(1 << 0)
#define WIIINPUT_MOUSE_RIGHT	(1 << 1)
#define WIIINPUT_MOUSE_MIDDLE	(1 << 2)

// Rumble on every Pro Controller and adapter port in use.
void wiiinput_rumble(bool on);

// The device that gave the last press: "Wii U Pro", "GameCube adapter",
// "Keyboard", "Mouse", or "" before any.
const char *wiiinput_source(void);

// ---- The parts, for the files that make them up ----

// The GameCube adapter's ports (gcadapter.c, from RiftWii).
typedef struct {
	u16 buttons;				// PAD_BUTTON_* / PAD_TRIGGER_*
	s8 stick_x, stick_y;		// from the centre, y up, about -100..100
	s8 substick_x, substick_y;
	u8 trigger_l, trigger_r;	// 0..255
	bool wireless;				// a WaveBird receiver: no rumble
} wiiinput_gc_pad;

void wiiinput_gcadapter_init(bool enabled);
void wiiinput_gcadapter_shutdown(void);
void wiiinput_gcadapter_update(void);
bool wiiinput_gcadapter_connected(void);
bool wiiinput_gcadapter_pad(int port, wiiinput_gc_pad *out);
void wiiinput_gcadapter_rumble(int port, bool on);
void wiiinput_gcadapter_set_log(int (*print)(const char *fmt, ...));

// Wii U Pro Controllers (wupc.c, from FIX94's libwupc): up to four, in the
// order they connect.
typedef struct {
	u32 buttons;				// WPAD_CLASSIC_BUTTON_*
	s16 lx, ly, rx, ry;			// from the centre, about -1024..1024, y up
	u8 battery;					// 0 (empty) to 4
} wiiinput_pro_pad;

void wiiinput_wupc_init(void);
void wiiinput_wupc_shutdown(void);
bool wiiinput_wupc_pad(int chan, wiiinput_pro_pad *out);
void wiiinput_wupc_rumble(int chan, bool on);

#endif
