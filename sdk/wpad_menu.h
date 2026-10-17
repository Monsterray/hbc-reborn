#ifndef HBC_WPAD_MENU_H
#define HBC_WPAD_MENU_H

#include <math.h>
#include <stdbool.h>
#include <wiiuse/wpad.h>

#define HBC_WPAD_DPAD (WPAD_BUTTON_UP | WPAD_BUTTON_RIGHT | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT)

typedef struct {
	bool sideways;
	bool dpad_held;
} hbc_wpad_menu;

// Update once per scan, before mapping down/held/up. Keep a press and its
// release in the same orientation, even if the remote moves while held.
static inline void hbc_wpad_menu_update(hbc_wpad_menu *s, const WPADData *d) {
	if (!d || d->err != WPAD_ERR_NONE) {
		s->sideways = s->dpad_held = false;
		return;
	}
	if (!s->dpad_held && (d->data_present & WPAD_DATA_ACCEL)) {
		float x = -d->gforce.x, y = fabsf(d->gforce.y);
		float g2 = x * x + y * y + d->gforce.z * d->gforce.z;

		// D-pad at the left, buttons facing the player: gravity is -X.
		// Ignore shakes and keep the last orientation when lying face up,
		// where an accelerometer cannot distinguish upright from sideways.
		if (g2 > 0.64f && g2 < 1.44f) {
			if (x > 0.35f && x > y * 1.4f)
				s->sideways = true;
			else if (x < -0.35f || (y > 0.35f && y > x * 1.4f))
				s->sideways = false;
		}
	}
	s->dpad_held = ((d->btns_h | d->btns_d) & HBC_WPAD_DPAD) != 0;
}

static inline u32 hbc_wpad_menu_buttons(const hbc_wpad_menu *s, u32 buttons) {
	u32 out;
	if (!s->sideways)
		return buttons;
	out = buttons & ~HBC_WPAD_DPAD;
	// Match the physical directions with the D-pad end pointing left.
	if (buttons & WPAD_BUTTON_UP) out |= WPAD_BUTTON_LEFT;
	if (buttons & WPAD_BUTTON_RIGHT) out |= WPAD_BUTTON_UP;
	if (buttons & WPAD_BUTTON_DOWN) out |= WPAD_BUTTON_RIGHT;
	if (buttons & WPAD_BUTTON_LEFT) out |= WPAD_BUTTON_DOWN;
	return out;
}

#endif
