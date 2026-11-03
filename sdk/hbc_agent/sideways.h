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
// Scans (about 60 a second) the other orientation must be seen in a row
// before the remote counts as turned: one report of a twist, a tilt or the
// sensor bar flipped a sideways remote back to upright mid-use. Leaving
// sideways takes longer than entering it.
#define SIDEWAYS_ENTER_SCANS 10
#define SIDEWAYS_LEAVE_SCANS 30

typedef struct {
	bool sideways;
	bool dpad_held;
	unsigned char streak;   // scans in a row that pointed the other way
} sideways_remote;

// One scan's evidence: 1 sideways, 0 upright, -1 nothing either way.
static inline void sideways_vote(sideways_remote *s, int v) {
	if (v < 0 || v == (int) s->sideways) {
		if (v >= 0)
			s->streak = 0;
		return;
	}
	if (++s->streak >= (s->sideways ? SIDEWAYS_LEAVE_SCANS : SIDEWAYS_ENTER_SCANS)) {
		s->sideways = v;
		s->streak = 0;
	}
}

// Once per scan, before turning its down, held and up buttons. A press and
// its release keep the orientation they started in.
static inline void sideways_update(sideways_remote *s, const WPADData *d) {
	// No report this scan (a remote that is quiet, between reports or
	// briefly not answering): keep the orientation. Resetting to upright here
	// flipped a sideways remote back for good, since a quiet remote sends no
	// reading to turn it sideways again (real Wii, 2026-11-03: most scans of
	// a remote held still carried no accelerometer data).
	if (!d || d->err != WPAD_ERR_NONE) {
		s->dpad_held = false;
		s->streak = 0;
		return;
	}
	if (s->dpad_held)
		;
	// A remote that sees the sensor bar is pointing, so upright.
	else if (d->ir.valid)
		sideways_vote(s, 0);
	else if (d->data_present & WPAD_DATA_ACCEL) {
		// +X is the remote's left side: with the D-pad at the left and the
		// buttons tilted towards the player, gravity reads as -X.
		float x = -d->gforce.x, y = fabsf(d->gforce.y);
		float g2 = x * x + y * y + d->gforce.z * d->gforce.z;

		// Ignore shakes. Lying face up says nothing about sideways or
		// upright, so the last orientation stays.
		if (g2 > 0.64f && g2 < 1.44f) {
			if (x > SIDEWAYS_TILT && x > y * 1.4f)
				sideways_vote(s, 1);
			else if (x < -0.35f || (y > 0.35f && y > x * 1.4f))
				sideways_vote(s, 0);
			else
				sideways_vote(s, -1);
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
