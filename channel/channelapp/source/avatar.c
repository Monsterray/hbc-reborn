// The user's Mii, standing beside the app list, as an avatar stands beside a
// console's dashboard: it breathes, looks around, blinks, now and then smiles
// and waves, and looks at the Wii Remote's pointer.
//
// This file doubles as the worked example of sdk/mii's drawing
// (sdk/mii/miidraw.h): load the face models, load a Mii, build it once, and
// each frame pose it and capture it into a texture that draws like any
// other picture.
//
//   1. mii_res_load: the face models, RFL_Res.dat. They are Nintendo's, so
//      HBC never ships them: the user saves the Wii's own copy to the SD card
//      with HOME > HBC > Save Mii faces (miires.c), and every app reads that.
//   2. mii_db_load and mii_db_get: the Mii, from the Mii Channel's database.
//   3. mii_model_make: shapes, face textures for each expression, a body.
//      About 0.1 s and 0.7 MiB, so a background thread does 1 to 3.
//   4. Each frame, before HBC draws anything: mii_capture poses it and draws
//      it into the EFB's corner, copies that out with alpha, and clears the
//      corner; gfx_restore puts HBC's GX state back. The texture then draws
//      as a sprite, under the app list and any dialog.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>

#include "../config.h"
#include "appentry.h"
#include "mii.h"
#include "miidraw.h"
#include "avatar.h"

// The sprite: drawn at twice this size and box-filtered down (miidraw.c),
// so 320 x 512 of the EFB while it draws.
#define AV_W 160
#define AV_H 256
#define AV_STACK (64 * 1024)
#define AV_PRIO 40

// Avatar states, set by the loader thread.
enum { AV_LOADING, AV_READY, AV_NONE };

static mii_res res;
static mii_model model;
static volatile int state = AV_NONE;
static volatile bool reload_asked;
static int want_slot;           // the Mii to show: this slot or the next one there
static int shown_slot = -1;
static char shown_name[MII_NAME_UTF8_MAX + 1];
static u32 frame_us, interval_us, frames;   // averaged over the last 64 frames
static u64 last_frame;
static lwp_t thread = LWP_THREAD_NULL;
static u8 stack[AV_STACK] ATTRIBUTE_ALIGN(32);
static u8 *pixels;
static gfx_entity entity;
static gfx_queue_entry entry;

// ---- Loading --------------------------------------------------------------

// The Mii to show: at first the first one starred as a favourite in the
// Mii Channel, else the first one there; after A on the avatar, the next
// one from want_slot on, round to the start.
static int pick_mii(mii *out) {
	static mii_db db;
	mii m;
	int i, first = -1, r;

	if ((r = mii_db_load(&db)) < 0)
		return r;
	for (i = 0; i < MII_DB_SLOTS; ++i) {
		int slot = want_slot >= 0 ? (want_slot + i) % MII_DB_SLOTS : i;

		if (mii_db_get(&db, slot, &m) < 0)
			continue;
		if (want_slot >= 0 || m.favorite) {
			*out = m;
			mii_db_free(&db);
			return slot;
		}
		if (first < 0) {
			first = slot;
			*out = m;
		}
	}
	mii_db_free(&db);
	return first >= 0 ? first : MII_E_EMPTY;
}

static void *loader(void *arg) {
	mii m;
	int r = MII_E_NO_FILE, tries, slot;
	char name[MII_NAME_UTF8_MAX + 1];

	(void) arg;
	// HBC mounts its devices as it starts: try for a minute.
	for (tries = 0; tries < 30; ++tries) {
		app_entry_mount_for(MII_RES_PATH);
		r = mii_res_load(&res, NULL);
		if (r != MII_E_NO_FILE && r != MII_E_IO)
			break;
		usleep(2000 * 1000);
	}
	if (r < 0) {
		hlog("Avatar: no Mii faces (%s): HOME > HBC > Save Mii faces\n", mii_strerror(r));
		state = AV_NONE;
		return NULL;
	}
	slot = pick_mii(&m);
	if (slot < 0) {
		hlog("Avatar: no Mii (%s)\n", mii_strerror(slot));
		mii_res_free(&res);
		state = AV_NONE;
		return NULL;
	}
	r = mii_model_make(&res, &m, 128, &model);
	mii_res_free(&res);   // the model keeps all it needs
	if (r < 0) {
		hlog("Avatar: the Mii in slot %d did not build (%s)\n", slot, mii_strerror(r));
		state = AV_NONE;
		return NULL;
	}
	mii_get_name(m.name, name, sizeof(name));
	hlog("Avatar: %s (slot %d)\n", name, slot);
	strcpy(shown_name, name);
	shown_slot = slot;
	state = AV_READY;
	return NULL;
}

static void start(void) {
	if (!pixels) {
		pixels = memalign(32, AV_W * AV_H * 4);
		if (!pixels)
			return;
		// The GPU writes it (mii_capture's copy); the CPU never touches it.
		DCInvalidateRange(pixels, AV_W * AV_H * 4);
		entity.type = GFXE_TEXTURE;
		entity.w = AV_W;
		entity.h = AV_H;
		entity.texture.pixels = pixels;
		entity.texture.type = GFXT_RGBA8;
		GX_InitTexObj(&entity.texture.obj, pixels, AV_W, AV_H, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP,
					  GX_FALSE);
	}
	state = AV_LOADING;
	if (LWP_CreateThread(&thread, loader, NULL, stack, AV_STACK, AV_PRIO) < 0) {
		thread = LWP_THREAD_NULL;
		state = AV_NONE;
	}
}

void avatar_init(void) {
	want_slot = -1;
	start();
}

void avatar_deinit(void) {
	if (thread != LWP_THREAD_NULL) {
		LWP_JoinThread(thread, NULL);
		thread = LWP_THREAD_NULL;
	}
	if (state == AV_READY)
		mii_model_free(&model);
	state = AV_NONE;
}

void avatar_reload(void) {
	reload_asked = true;
}

// ---- Animation ------------------------------------------------------------

static u32 seed = 0x4d494921;

static float rnd(void) {   // 0 to 1
	seed = seed * 1103515245u + 12345u;
	return (seed >> 8) / 16777216.0f;
}

static float approach(float v, float target, float rate, float dt) {
	return v + (target - v) * (1 - expf(-rate * dt));
}

// What the Mii is doing; times in seconds since the avatar first stood.
// (The time base itself can be large enough that a float of its seconds
// does not move frame to frame.)
static u64 t0;
static struct {
	float t, last;
	float look_yaw, look_pitch, want_yaw, want_pitch, next_look;
	float blink_at, smile_until, next_smile;
	float wave_from, next_wave;
	float raise;
} a;

static void pose_for(mii_pose *p, bool pointer, s32 px, s32 py, float sx, float sy, float sw, float sh) {
	float t = a.t, dt = t - a.last;
	float wave = -1;

	if (dt < 0 || dt > 0.5f)
		dt = 1 / 60.0f;
	a.last = t;
	memset(p, 0, sizeof(*p));

	// Look: at the pointer when it is on the screen, else wherever.
	if (pointer) {
		float cx = sx + sw / 2, cy = sy + sh * 0.25f;

		a.want_yaw = (px - cx) / 400.0f;
		a.want_pitch = -(py - cy) / 500.0f;
		if (a.want_yaw > 0.9f) a.want_yaw = 0.9f;
		if (a.want_yaw < -0.6f) a.want_yaw = -0.6f;
		if (a.want_pitch > 0.35f) a.want_pitch = 0.35f;
		if (a.want_pitch < -0.35f) a.want_pitch = -0.35f;
		// Pointed at: a smile and a wave.
		if (px >= sx && px < sx + sw && py >= sy && py < sy + sh) {
			a.smile_until = t + 1.5f;
			if (a.wave_from < t - 3)
				a.wave_from = t;
		}
	} else if (t >= a.next_look) {
		a.want_yaw = 0.25f + (rnd() - 0.5f) * 0.9f;
		a.want_pitch = (rnd() - 0.5f) * 0.3f;
		a.next_look = t + 1.5f + rnd() * 3.5f;
	}
	a.look_yaw = approach(a.look_yaw, a.want_yaw, 5, dt);
	a.look_pitch = approach(a.look_pitch, a.want_pitch, 5, dt);

	// The body turns a little towards the list, and sways.
	p->yaw = 0.18f + 0.06f * sinf(t * 0.45f) + a.look_yaw * 0.25f;
	p->head_yaw = a.look_yaw * 0.75f;
	p->head_pitch = a.look_pitch;
	p->head_roll = 0.04f * sinf(t * 0.7f);
	p->breathe = 0.5f + 0.5f * sinf(t * 2 * (float) M_PI / 3.4f);
	p->bob = 0.4f * p->breathe;

	// A wave now and then: the right arm up, the hand going side to side.
	if (t >= a.next_wave) {
		a.wave_from = t;
		a.next_wave = t + 18 + rnd() * 20;
		a.smile_until = t + 2.6f;
	}
	if (t - a.wave_from < 2.4f)
		wave = t - a.wave_from;
	a.raise = approach(a.raise, wave >= 0 ? 2.35f : 0.1f, 7, dt);
	p->arm_raise[0] = a.raise + (wave >= 0 ? 0.28f * sinf(wave * 2 * (float) M_PI * 2.2f) : 0);
	p->arm_raise[1] = 0.1f;
	p->arm_swing[0] = wave >= 0 ? 0.35f : 0.05f * sinf(t * 0.9f);
	p->arm_swing[1] = -0.05f * sinf(t * 0.9f);

	// The face: smiles now and then; blinks every few seconds.
	if (t >= a.next_smile) {
		a.smile_until = t + 2 + rnd() * 2;
		a.next_smile = t + 10 + rnd() * 14;
	}
	p->expression = t < a.smile_until ? MII_EXPR_SMILE : MII_EXPR_NORMAL;
	if (t >= a.blink_at) {
		if (t < a.blink_at + 0.13f)
			p->expression = MII_EXPR_BLINK;
		else
			a.blink_at = t + 2 + rnd() * 4;
	}
}

// ---- Each frame -----------------------------------------------------------

const gfx_queue_entry *avatar_frame(bool pointer, s32 px, s32 py, bool a_down) {
	u64 start_ticks = gettime();
	float stretch = widescreen ? WIDESCREEN_RATIO : 1.0f;
	float w = AV_W * stretch, h = AV_H, x, y;
	mii_pose pose;

	if (reload_asked && state != AV_LOADING) {
		reload_asked = false;
		avatar_deinit();
		start();
	}
	if (state != AV_READY)
		return NULL;
	if (!t0) {
		t0 = gettime();
		memset(&a, 0, sizeof(a));
		a.next_look = 1;
		a.blink_at = 2;
		a.next_smile = 6;
		a.next_wave = 4;
		a.wave_from = -100;
	}
	a.t = diff_msec(t0, gettime()) / 1000.0f;   // u32 ms: 49 days

	// In the space left of the app list (320 wide, centred), feet just above
	// the logo.
	x = (view_width / 2 - 160 - w) / 2;
	if (x < 8)
		x = 8;
	y = 404 - h;

	// A on the avatar: the next Mii.
	if (a_down && pointer && px >= x && px < x + w && py >= y && py < y + h) {
		want_slot = shown_slot + 1;
		reload_asked = true;
	}
	pose_for(&pose, pointer, px, py, x, y, w, h);
	if (mii_capture(&model, &pose, AV_W, AV_H, 0.04f, stretch, (GXColor) { 0, 0, 0, 0xff },
					pixels) < 0)
		return NULL;
	gfx_restore();

	entity.w = (u16) (w + 0.5f);   // the texture spans this many view units
	if (last_frame)
		interval_us = interval_us - interval_us / 64 + diff_usec(last_frame, start_ticks) / 64;
	last_frame = start_ticks;
	frame_us = frame_us - frame_us / 64 + diff_usec(start_ticks, gettime()) / 64;
	++frames;
	gfx_qe_entity(&entry, &entity, x, y, -2, COL_DEFAULT);
	return &entry;
}

void avatar_get_info(avatar_info *info) {
	info->state = state == AV_READY ? "ready" : state == AV_LOADING ? "loading" : "none";
	strcpy(info->name, state == AV_READY ? shown_name : "");
	info->slot = state == AV_READY ? shown_slot : -1;
	info->frame_us = frame_us;
	info->interval_us = interval_us;
	info->frames = frames;
}
