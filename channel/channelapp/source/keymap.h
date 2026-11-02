#ifndef _KEYMAP_H_
#define _KEYMAP_H_

#include <gctypes.h>

// A USB keyboard's keys as a Wii Remote's buttons. Keys are bits by USB HID
// usage code (keyboard page), so a set of keys is KEYMAP_BYTES bytes.
//
//   arrows        D-pad
//   Enter, Space  A            Backspace  B
//   X, 1          1            Y, 2       2
//   -, Delete     Minus        =/+, Insert  Plus
//   Esc, Home     HOME
// The keypad's Enter, - and + work as their main-block twins.

#define KEYMAP_BYTES (256 / 8)
#define KEYMAP_GET(a, k) (((a)[(k) >> 3] & (1 << ((k) & 7))) != 0)
#define KEYMAP_SET(a, k) ((a)[(k) >> 3] |= (1 << ((k) & 7)))
#define KEYMAP_CLR(a, k) ((a)[(k) >> 3] &= ~(1 << ((k) & 7)))

// The USB usage code of Print Screen, which controls.c takes as a screenshot.
#define KEYMAP_PRINT_SCREEN 0x46

// WPAD_BUTTON_* bits for the keys set in keys[KEYMAP_BYTES].
u32 keymap_buttons(const u8 *keys);

#endif
