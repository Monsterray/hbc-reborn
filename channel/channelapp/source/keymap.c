// A USB keyboard's keys as a Wii Remote's buttons (keymap.h). No libogc here
// beyond the button values, so tests/test_keymap.py runs it on a PC.

#include <wiiuse/wpad.h>

#include "keymap.h"

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

u32 keymap_buttons (const u8 *keys) {
	u32 buttons = 0;
	unsigned i;

	for(i=0;i<sizeof(key_map)/sizeof(key_map[0]);i++)
		if(KEYMAP_GET (keys, key_map[i].usage))
			buttons |= key_map[i].button;
	return buttons;
}
