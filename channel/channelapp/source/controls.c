#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>

#include "../config.h"

#include "controls.h"
#include "gfx.h"
#include "wiiinput.h"

static s32 pointer_owner = -1;
WPADData *wpads[WPAD_MAX_WIIMOTES];

static int rumbling = 0;

#define QUEUED_MAX 64
static u32 queued[QUEUED_MAX];
static volatile u32 queued_head, queued_tail;
static u32 queued_up;

// The USB mouse has the pointer while it is the last one moved; a Wii
// Remote pointing at the screen takes it back.
static bool mouse_pointer;

void controls_init (void) {
	int i;

	// The Wii U Pro Controller's hook into wiiuse is set before WPAD_Init.
	wiiinput_init (0);
	wiiinput_mouse_area (view_width, view_height);

	i = WPAD_Init ();

	if(i < 0) {
		gprintf("WPAD_Init failed: %d\n",i);
		return;
	}

	for(i=0;i<WPAD_MAX_WIIMOTES;i++) {
		WPAD_SetDataFormat (i, WPAD_FMT_BTNS_ACC_IR);
		WPAD_SetVRes (i, view_width + 128, view_height + 128);
	}
	WPAD_SetIdleTimeout (300);	// 5 minutes, as libogc and the Wii Menu
}

void controls_deinit (void) {
	wiiinput_shutdown ();
	WPAD_Shutdown ();
}

// A Classic Controller's buttons, or a Wii U Pro Controller's (the same
// bits), added to btns as a Wii Remote's.
static u32 classic_button_transform(u32 btns) {
	if(btns & WPAD_CLASSIC_BUTTON_LEFT)
		btns |= WPAD_BUTTON_LEFT;
	if(btns & WPAD_CLASSIC_BUTTON_RIGHT)
		btns |= WPAD_BUTTON_RIGHT;
	if(btns & WPAD_CLASSIC_BUTTON_UP)
		btns |= WPAD_BUTTON_UP;
	if(btns & WPAD_CLASSIC_BUTTON_DOWN)
		btns |= WPAD_BUTTON_DOWN;
	if(btns & WPAD_CLASSIC_BUTTON_A)
		btns |= WPAD_BUTTON_A;
	if(btns & WPAD_CLASSIC_BUTTON_B)
		btns |= WPAD_BUTTON_B;
	if(btns & WPAD_CLASSIC_BUTTON_X)
		btns |= WPAD_BUTTON_1;
	if(btns & WPAD_CLASSIC_BUTTON_Y)
		btns |= WPAD_BUTTON_2;
	if((btns & WPAD_CLASSIC_BUTTON_FULL_L) || (btns & WPAD_CLASSIC_BUTTON_MINUS))
		btns |= WPAD_BUTTON_MINUS;
	if((btns & WPAD_CLASSIC_BUTTON_FULL_R) || (btns & WPAD_CLASSIC_BUTTON_PLUS))
		btns |= WPAD_BUTTON_PLUS;
	if(btns & WPAD_CLASSIC_BUTTON_HOME)
		btns |= WPAD_BUTTON_HOME;
	if(btns & WPAD_CLASSIC_BUTTON_ZR)
		btns |= PADW_BUTTON_NET_INIT;
	if(btns & WPAD_CLASSIC_BUTTON_ZL)
		btns |= PADW_BUTTON_SCREENSHOT;
	return btns;
}

static u32 wpad_button_transform(WPADData *wd, u32 btns) {
	btns &= ~PADW_BUTTON_NET_INIT;
	btns &= ~PADW_BUTTON_SCREENSHOT;

	switch(wd->exp.type) {
		case WPAD_EXP_NUNCHUK:
			if(btns & WPAD_NUNCHUK_BUTTON_Z)
				btns |= PADW_BUTTON_NET_INIT;
			if(btns & WPAD_NUNCHUK_BUTTON_C)
				btns |= PADW_BUTTON_SCREENSHOT;
			break;
		case WPAD_EXP_CLASSIC:
			btns = classic_button_transform(btns);
			break;
		case WPAD_EXP_GUITARHERO3:
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_STRUM_UP)
				btns |= WPAD_BUTTON_UP;
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_STRUM_DOWN)
				btns |= WPAD_BUTTON_DOWN;
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_GREEN)
				btns |= WPAD_BUTTON_A;
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_RED)
				btns |= WPAD_BUTTON_B;
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_PLUS)
				btns |= WPAD_BUTTON_PLUS;
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_MINUS)
				btns |= WPAD_BUTTON_MINUS;
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_YELLOW)
				btns |= WPAD_BUTTON_LEFT;
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_BLUE)
				btns |= WPAD_BUTTON_RIGHT;
			if(btns & WPAD_GUITAR_HERO_3_BUTTON_ORANGE)
				btns |= WPAD_BUTTON_1;
			break;
	}
	return (btns&0xffff) << 16;
}

void controls_scan (u32 *down, u32 *held, u32 *up) {
	u32 bd, bh, bu;
	int i;
	s32 last_owner;

	PAD_ScanPads ();

	// Every GameCube port, and the GameCube adapter for Wii U's.
	bd = bh = bu = 0;
	for(i=0;i<PAD_CHANMAX;i++) {
		bd |= PAD_ButtonsDown (i);
		bh |= PAD_ButtonsHeld (i);
		bu |= PAD_ButtonsUp (i);
	}

	wiiinput_scan ();
	bd |= wiiinput_gc_down ();
	bh |= wiiinput_gc_held ();
	bu |= wiiinput_gc_up ();

	WPAD_ScanPads ();

	for(i=0;i<WPAD_MAX_WIIMOTES;i++)
		if(WPAD_Probe (i, NULL) == WPAD_ERR_NONE) {
			wpads[i] = WPAD_Data(i);
		} else {
			wpads[i] = NULL;
		}

	last_owner = pointer_owner;

	// kill pointer owner if it stops pointing
	if((pointer_owner >= 0) && (!wpads[pointer_owner] || !wpads[pointer_owner]->ir.valid)) {
		pointer_owner = -1;
	}

	// find a new pointer owner if necessary
	if(pointer_owner < 0) {
		for(i=0;i<WPAD_MAX_WIIMOTES;i++) {
			if(wpads[i] && wpads[i]->ir.valid) {
				pointer_owner = i;
				break;
			}
		}
	}

	// pointer owner owns buttons
	if(pointer_owner >= 0) {
		bd |= wpad_button_transform(wpads[pointer_owner], wpads[pointer_owner]->btns_d);
		bh |= wpad_button_transform(wpads[pointer_owner], wpads[pointer_owner]->btns_h);
		bu |= wpad_button_transform(wpads[pointer_owner], wpads[pointer_owner]->btns_u);
	} else {
	// otherwise just mix all buttons together
		for(i=0;i<WPAD_MAX_WIIMOTES;i++) {
			if(wpads[i]) {
				bd |= wpad_button_transform(wpads[i], wpads[i]->btns_d);
				bh |= wpad_button_transform(wpads[i], wpads[i]->btns_h);
				bu |= wpad_button_transform(wpads[i], wpads[i]->btns_u);
			}
		}
	}

	// Wii U Pro Controllers, as Classic Controllers.
	bd |= (classic_button_transform(wiiinput_classic_down()) & 0xffff) << 16;
	bh |= (classic_button_transform(wiiinput_classic_held()) & 0xffff) << 16;
	bu |= (classic_button_transform(wiiinput_classic_up()) & 0xffff) << 16;

	// The USB keyboard, as a Wii Remote; Print Screen takes a screenshot.
	bd |= (wiiinput_keys_down() & 0xffff) << 16;
	bh |= (wiiinput_keys_held() & 0xffff) << 16;
	bu |= (wiiinput_keys_up() & 0xffff) << 16;
	if (wiiinput_key_down(0x46))
		bd |= PADS_SCREENSHOT;

	// The USB mouse: left is A, right B, middle HOME, and the wheel
	// presses up or down.
	if (wiiinput_mouse_moved())
		mouse_pointer = true;
	{
		static const u32 mouse_btns[3] = { PADS_A, PADS_B, PADS_HOME };
		u8 md = wiiinput_mouse_down(), mh = wiiinput_mouse_held(), mu = wiiinput_mouse_up();
		s32 wheel = wiiinput_mouse_wheel();

		for(i=0;i<3;i++) {
			if(md & (1 << i))
				bd |= mouse_btns[i] & ~0xffff;
			if(mh & (1 << i))
				bh |= mouse_btns[i] & ~0xffff;
			if(mu & (1 << i))
				bu |= mouse_btns[i] & ~0xffff;
		}
		if (wheel > 0)
			controls_queue_press(PADS_DOWN & ~0xffff);
		else if (wheel < 0)
			controls_queue_press(PADS_UP & ~0xffff);
	}

	// A press from the PC: down this frame, up the next.
	if (queued_up) {
		bu |= queued_up;
		queued_up = 0;
	} else if (queued_tail != queued_head) {
		queued_up = queued[queued_tail % QUEUED_MAX];
		bd |= queued_up;
		bh |= queued_up;
		++queued_tail;
	}

	if(last_owner >= 0 && last_owner != pointer_owner && rumbling) {
		WPAD_Rumble (last_owner, 0);
		if(pointer_owner >= 0)
			WPAD_Rumble(pointer_owner, 1);
	}

	if (down)
		*down = bd;

	if (held)
		*held = bh;

	if (up)
		*up = bu;
}

u32 controls_other_down(void) {
	static const struct {
		u16 pad;
		u32 wpad;
	} gc_map[] = {
		{ PAD_BUTTON_A, WPAD_BUTTON_A }, { PAD_BUTTON_B, WPAD_BUTTON_B },
		{ PAD_BUTTON_X, WPAD_BUTTON_1 }, { PAD_BUTTON_Y, WPAD_BUTTON_2 },
		{ PAD_BUTTON_START, WPAD_BUTTON_HOME },
		{ PAD_BUTTON_UP, WPAD_BUTTON_UP }, { PAD_BUTTON_DOWN, WPAD_BUTTON_DOWN },
		{ PAD_BUTTON_LEFT, WPAD_BUTTON_LEFT }, { PAD_BUTTON_RIGHT, WPAD_BUTTON_RIGHT },
	};
	u32 b;
	u16 gc;
	u8 m;
	size_t i;

	wiiinput_scan ();
	b = classic_button_transform(wiiinput_classic_down()) & 0xffff;
	b |= wiiinput_keys_down() & 0xffff;
	gc = wiiinput_gc_down();
	for (i = 0; i < sizeof(gc_map) / sizeof(gc_map[0]); i++)
		if (gc & gc_map[i].pad)
			b |= gc_map[i].wpad;
	m = wiiinput_mouse_down();
	if (m & WIIINPUT_MOUSE_LEFT)
		b |= WPAD_BUTTON_A;
	if (m & WIIINPUT_MOUSE_RIGHT)
		b |= WPAD_BUTTON_B;
	if (m & WIIINPUT_MOUSE_MIDDLE)
		b |= WPAD_BUTTON_HOME;
	return b;
}

bool controls_queue_press(u32 buttons) {
	if (queued_head - queued_tail >= QUEUED_MAX)
		return false;
	queued[queued_head % QUEUED_MAX] = buttons;
	++queued_head;
	return true;
}

bool controls_ir (s32 *x, s32 *y, f32 *roll) {
	if (pointer_owner >= 0 && wpads[pointer_owner] && wpads[pointer_owner]->ir.valid) {
		*x = wpads[pointer_owner]->ir.x - 64;
		*y = wpads[pointer_owner]->ir.y - 64;

		if(roll)
			*roll = -wpads[pointer_owner]->ir.angle / 180.0 * M_PI;

		// The mouse carries on from where the Wii Remote pointed.
		mouse_pointer = false;
		wiiinput_mouse_warp(*x, *y);
		return true;
	} else if (mouse_pointer && wiiinput_mouse(x, y)) {
		if(roll)
			*roll = 0;
		return true;
	} else {
		return false;
	}
}

void controls_rumble(int rumble) {
	if(pointer_owner >= 0)
		WPAD_Rumble(pointer_owner, rumble);
	rumbling = rumble;
}

static s32 wpad_sticky(int chan) {
	s32 sy = 0;
	if(wpads[chan]) {
		switch(wpads[chan]->exp.type) {
			case WPAD_EXP_NUNCHUK:
				sy = wpads[chan]->exp.nunchuk.js.pos.y - wpads[chan]->exp.nunchuk.js.center.y;
				break;
			case WPAD_EXP_CLASSIC:
				sy = (wpads[chan]->exp.classic.ljs.pos.y - wpads[chan]->exp.classic.ljs.center.y) * 4;
				break;
			case WPAD_EXP_GUITARHERO3:
				sy = (wpads[chan]->exp.gh3.js.pos.y - wpads[chan]->exp.gh3.js.center.y) * 4;
				break;
		}
	}
	return sy;
}

#define DEADZONE 10

s32 deadzone(s32 v) {
	if(v > DEADZONE)
		return v - DEADZONE;
	if(v < -DEADZONE)
		return v + DEADZONE;
	return 0;
}

s32 controls_sticky(void) {
	s32 sy = 0;
	int i;
	for(i=0;i<PAD_CHANMAX;i++)
		sy += deadzone(PAD_StickY(i));
	sy += deadzone(wiiinput_stick_y());
	if(pointer_owner >= 0) {
		sy += deadzone(wpad_sticky(pointer_owner));
	} else {
		for(i=0;i<WPAD_MAX_WIIMOTES;i++) {
			sy += deadzone(wpad_sticky(i));
		}
	}
	return sy;
}

void controls_set_ir_threshold(int on) {
	int i;
	for(i=0;i<WPAD_MAX_WIIMOTES;i++) {
		if(on)
			WPAD_SetIdleThresholds(i, WPAD_THRESH_DEFAULT_BUTTONS, 10, WPAD_THRESH_DEFAULT_ACCEL, WPAD_THRESH_DEFAULT_JOYSTICK, WPAD_THRESH_DEFAULT_BALANCEBOARD, WPAD_THRESH_DEFAULT_MOTION_PLUS);
		else
			WPAD_SetIdleThresholds(i, WPAD_THRESH_DEFAULT_BUTTONS, WPAD_THRESH_DEFAULT_IR, WPAD_THRESH_DEFAULT_ACCEL, WPAD_THRESH_DEFAULT_JOYSTICK, WPAD_THRESH_DEFAULT_BALANCEBOARD, WPAD_THRESH_DEFAULT_MOTION_PLUS);
	}
}
