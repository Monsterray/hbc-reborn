#ifndef HBC_AGENT_SIDEWAYS_H
#define HBC_AGENT_SIDEWAYS_H

// A Wii Remote held sideways, D-pad at the left (PR 6, Matt Sephton): which
// way it is held, from its accelerometer, and its D-pad turned to match.
// Kept apart from overlay.c so tests/test_sideways_remote.py can run it.

#include <math.h>
#include <stdbool.h>
#include <wiiuse/wpad.h>

#define SIDEWAYS_DPAD (WPAD_BUTTON_UP | WPAD_BUTTON_RIGHT | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT)
// How far gravity must lie along the remote's length (in g) to count:
// 0.5 is a 30 degree tilt. Tune on hardware.
#define SIDEWAYS_TILT 0.5f

typedef struct {
	bool sideways;
	bool dpad_held;
} sideways_remote;

// Once per scan, before turning its down, held and up buttons. A press and
// its release keep the orientation they started in.
static inline void sideways_update(sideways_remote *s, const WPADData *d) {
	if (!d || d->err != WPAD_ERR_NONE) {
		s->sideways = s->dpad_held = false;
		return;
	}
	if (s->dpad_held)
		;
	// A remote that sees the sensor bar is pointing, so upright.
	else if (d->ir.valid)
		s->sideways = false;
	else if (d->data_present & WPAD_DATA_ACCEL) {
		// +X is the remote's left side: with the D-pad at the left and the
		// buttons tilted towards the player, gravity reads as -X.
		float x = -d->gforce.x, y = fabsf(d->gforce.y);
		float g2 = x * x + y * y + d->gforce.z * d->gforce.z;

		// Ignore shakes. Lying face up says nothing about sideways or
		// upright, so the last orientation stays.
		if (g2 > 0.64f && g2 < 1.44f) {
			if (x > SIDEWAYS_TILT && x > y * 1.4f)
				s->sideways = true;
			else if (x < -0.35f || (y > 0.35f && y > x * 1.4f))
				s->sideways = false;
		}
	}
	s->dpad_held = ((d->btns_h | d->btns_d) & SIDEWAYS_DPAD) != 0;
}

// The D-pad as the player sees it: Up is Left, Right is Up, and so on.
static inline u32 sideways_buttons(const sideways_remote *s, u32 buttons) {
	u32 out;

	if (!s->sideways)
		return buttons;
	out = buttons & ~SIDEWAYS_DPAD;
	if (buttons & WPAD_BUTTON_UP) out |= WPAD_BUTTON_LEFT;
	if (buttons & WPAD_BUTTON_RIGHT) out |= WPAD_BUTTON_UP;
	if (buttons & WPAD_BUTTON_DOWN) out |= WPAD_BUTTON_RIGHT;
	if (buttons & WPAD_BUTTON_LEFT) out |= WPAD_BUTTON_DOWN;
	return out;
}

#endif
