#ifndef _KEYBOARD_H_
#define _KEYBOARD_H_

#include <gctypes.h>

#include "keymap.h"

// A USB keyboard as a Wii Remote's buttons (keymap.h), read through libogc's
// USB calls. It can be plugged in at any time.

// Starts reading; a keyboard that is not there yet is picked up when it is
// plugged in. Without libogc's USB, the keyboard just reads as no keys.
void keyboard_init (void);
void keyboard_shutdown (void);
// After keyboard_shutdown: a read IOS never gave back, which would write
// into (and call back into) whatever is loaded there next. Only an IOS
// reload ends it.
bool keyboard_pending (void);

// Once a frame, before reading anything below: "down" and "up" are what
// changed since the previous call, and a key pressed and let go in between
// still counts as down.
void keyboard_scan (void);

// WPAD_BUTTON_* bits.
u32 keyboard_held (void);
u32 keyboard_down (void);
u32 keyboard_up (void);

// A key, by USB usage code, pressed since the last scan: for keys that have
// no Wii Remote button (KEYMAP_PRINT_SCREEN).
bool keyboard_key_down (u8 usage);

#endif
