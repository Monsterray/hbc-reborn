// The user's Mii, standing beside the app list, as an avatar stands beside a
// console's dashboard: it breathes, looks around, blinks, now and then smiles
// and waves, and looks at the Wii Remote's pointer. A on it opens the Mii
// picker (miipick.c), and the Mii chosen there stands in its place.
//
// This file doubles as the worked example of sdk/mii's drawing
// (sdk/mii/miidraw.h): load the face models, load a Mii, build it once, and
// each frame pose it and draw it in its place in the frame.
//
//   1. mii_res_load: the face models, RFL_Res.dat. They are Nintendo's, so
//      HBC never ships them: the user saves the Wii's own copy to the SD card
//      with HOME > HBC > Save Mii faces (miires.c), and every app reads that.
//   2. mii_db_load and mii_db_get: the Mii, from the Mii Channel's database.
//   3. mii_model_build: shapes, face textures for all seven expressions
//      (the playground shows each), a body; then mii_body_load and
//      mii_model_set_body: the Mii Channel's own body for the Mii's sex
//      (saved beside the faces), with its animations. About 0.1 s, so a
//      background thread does 1 to 3.
//   4. Each frame avatar_frame poses it and queues a GFXE_DRAW entity where
//      a sprite would go. When HBC's frame reaches it, avatar_draw sets the
//      viewport to that rectangle and a thin band of depth (gfx.c's
//      plot_draw), mii_view frames the Mii, and mii_draw draws it; HBC's GX
//      state is then put back. So it stands under the app list and any
//      dialog, with no copy and no pixel-format change (1.10.21 captured it
//      into a texture each frame with mii_capture instead).

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "poison.h"
#include "fpurelease.h"
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "../config.h"
#include "appentry.h"
#include "controls.h"
#include "mii.h"
#include "miidraw.h"
#include "avatar.h"

// The avatar's rectangle in view units (its width times WIDESCREEN_RATIO on
// a 16:9 screen).
#define AV_W 128
#define AV_H 208
#define AV_STACK (64 * 1024)
#define AV_PRIO 40

// Avatar states, set by the loader thread.
enum { AV_LOADING, AV_READY, AV_NONE };

static mii_res res;
static mii_model model;
static mii_body body;                 // the Mii Channel's body, when saved
static int anim_wait, anim_greet;     // 1 + its animations (0: none)
static volatile int state = AV_NONE;
static volatile bool reload_asked;
static volatile bool stopping;   // avatar_deinit: the loader ends at once
static char want_id[2 * MII_ID_SIZE + 1];   // the Mii chosen (creation ID, hex), or ""
static char shown_id[2 * MII_ID_SIZE + 1];
static bool hit;                // A on it this frame
static volatile bool asked;     // the picker, asked for by the PC (avatar_ask_picker)
static char play_name[sizeof(((mii_anim *) 0)->name)];   // avatar_play's, from the PC
static volatile u32 play_asked;   // avatar_play calls; pose_for() takes each once
static int shown_slot = -1;
static char shown_name[MII_NAME_UTF8_MAX + 1];
static u32 frame_us, draw_us, interval_us, frames;   // averaged over the last 64 frames
static u64 last_frame;
static lwp_t thread = LWP_THREAD_NULL;
static u8 stack[AV_STACK] ATTRIBUTE_ALIGN(32);
static gfx_entity entity;
static mii_pose pose;   // this frame's, from avatar_frame() for avatar_draw()
static gfx_queue_entry entry;
// The Mii Channel's effects (sdk/mii's player), when Save Mii faces saved
// them: loaded with the first body, kept for every Mii after.
static mii_fx fx;
static mii_fx_state fxs;
static bool fx_ok;
static u32 fx_played;   // effects started, for HBCS (one lasts under a second)
static volatile bool poof_asked;   // a new Mii: a large poof of dust where it stands

// ---- Loading --------------------------------------------------------------

static void id_hex(const u8 *id, char *out) {
	int i;

	for (i = 0; i < MII_ID_SIZE; ++i)
		sprintf(out + 2 * i, "%02x", id[i]);
}

// The Mii to show: the one chosen in the picker (want_id) while it is
// there; else the first one starred as a favourite in the Mii Channel,
// else the first one there.
static int pick_mii(mii *out) {
	static mii_db db;
	mii m, fav_m, first_m;
	int i, first = -1, fav = -1, r;
	char id[2 * MII_ID_SIZE + 1];

	if ((r = mii_db_load(&db)) < 0)
		return r;
	for (i = 0; i < MII_DB_SLOTS; ++i) {
		if (mii_db_get(&db, i, &m) < 0)
			continue;
		id_hex(m.id, id);
		if (want_id[0] && !strcmp(id, want_id)) {
			*out = m;
			mii_db_free(&db);
			return i;
		}
		if (fav < 0 && m.favorite) {
			fav = i;
			fav_m = m;
		}
		if (first < 0) {
			first = i;
			first_m = m;
		}
	}
	mii_db_free(&db);
	if (fav >= 0) {
		*out = fav_m;
		return fav;
	}
	if (first >= 0)
		*out = first_m;
	return first >= 0 ? first : MII_E_EMPTY;
}

static void *loader_body(void *arg);
static void *loader(void *arg) {
	void *r = loader_body(arg);

	fpu_release();
	return r;
}

static void *loader_body(void *arg) {
	mii m;
	int r = MII_E_NO_FILE, tries, slot;
	char name[MII_NAME_UTF8_MAX + 1];

	(void) arg;
	// HBC mounts its devices as it starts: while the card is not mounted yet,
	// try for a minute. A mounted card without the file (no Save Mii faces
	// yet) is an answer, not a wait. Every sleep checks for avatar_deinit,
	// which launching an app or exiting calls right away.
	for (tries = 0; tries < 30 && !stopping; ++tries) {
		struct stat st;
		int k;

		app_entry_mount_for(MII_RES_PATH);
		r = mii_res_load(&res, NULL);
		if ((r != MII_E_NO_FILE && r != MII_E_IO) || stat("sd:/", &st) == 0)
			break;
		for (k = 0; k < 20 && !stopping; ++k)
			usleep(100 * 1000);
	}
	if (stopping) {
		if (r >= 0)
			mii_res_free(&res);
		state = AV_NONE;
		return NULL;
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
	{
		mii_model_opts o = { 128, 0, NULL, 0, 0 };   // every expression

		r = mii_model_build(&res, &m, &o, &model);
	}
	mii_res_free(&res);   // the model keeps all it needs
	if (r < 0) {
		hlog("Avatar: the Mii in slot %d did not build (%s)\n", slot, mii_strerror(r));
		state = AV_NONE;
		return NULL;
	}
	// The Wii's own body for this Mii, if Save Mii faces saved them; else
	// the library's.
	anim_wait = anim_greet = 0;
	if (!fx_ok && mii_fx_load(&fx, NULL, NULL) >= 0) {
		mii_fx_init(&fxs, &fx, 1);
		fx_ok = true;
	}
	r = mii_body_load(&body, m.sex == MII_FEMALE ? MII_BODY_FEMALE_PATH : MII_BODY_MALE_PATH);
	if (r >= 0 && (r = mii_model_set_body(&model, &body)) >= 0) {
		anim_wait = 1 + mii_body_anim(&body, "bodyWait");
		anim_greet = 1 + mii_body_anim(&body, "bodyGreeting");
	} else {
		hlog("Avatar: no Mii Channel body (%s): the built one\n", mii_strerror(r));
		mii_body_free(&body);
	}
	mii_get_name(m.name, name, sizeof(name));
	hlog("Avatar: %s (slot %d)\n", name, slot);
	strcpy(shown_name, name);
	{   // another Mii than the one shown (not the first, at start): it poofs in
		char id[sizeof(shown_id)];

		id_hex(m.id, id);
		poof_asked = shown_id[0] && strcmp(id, shown_id);
		strcpy(shown_id, id);
	}
	shown_slot = slot;
	state = AV_READY;
	return NULL;
}

static void start(void) {
	state = AV_LOADING;
	poison_fill(stack, AV_STACK);
	if (LWP_CreateThread(&thread, loader, NULL, stack, AV_STACK, AV_PRIO) < 0) {
		thread = LWP_THREAD_NULL;
		state = AV_NONE;
	}
}

void avatar_init(const char *mii_id) {
	snprintf(want_id, sizeof(want_id), "%s", mii_id ? mii_id : "");
	start();
}

void avatar_deinit(void) {
	if (thread != LWP_THREAD_NULL) {
		stopping = true;
		LWP_JoinThread(thread, NULL);
		thread = LWP_THREAD_NULL;
		stopping = false;
	}
	if (state == AV_READY) {
		mii_model_free(&model);
		mii_body_free(&body);
	}
	if (fx_ok) {
		mii_fx_free(&fx);
		fx_ok = false;
	}
	state = AV_NONE;
}

void avatar_reload(void) {
	reload_asked = true;
}

void avatar_choose(const char *mii_id) {
	snprintf(want_id, sizeof(want_id), "%s", mii_id);
	if (strcmp(want_id, shown_id) || state != AV_READY)
		reload_asked = true;
}

const char *avatar_mii_id(void) {
	return state == AV_READY ? shown_id : NULL;
}

void avatar_ask_picker(void) {
	asked = true;
}

void avatar_play(const char *name) {
	snprintf(play_name, sizeof(play_name), "%s", name);
	++play_asked;
}

bool avatar_clicked(void) {
	bool r = hit || asked;

	hit = asked = false;
	return r;
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
	// An animation the PC asked for (avatar_play): 1 + its index, or 0, and
	// when it started.
	u32 play_seen;
	int play;
	float play_from;
	// The animation last shown and its frame; on a change, a cross-fade
	// from it (still playing) for FADE_S.
	int last_anim;
	float last_frame;
	int from_anim;
	float from_frame, from_t;
} a;

#define FADE_S 0.4f   // how long a change of animation blends

// The playground (miiplay.c): the Mii's feet at (x, y) in view units on the
// whole screen, on the floor or falling to it, or held by the pointer.
#define PG_H 260          // the Mii's box at size 1, as AV_H beside the list
#define PG_SIZE_MIN 4     // its size in tenths: + and - in miiplay.c
#define PG_SIZE_MAX 14
#define PG_GRAVITY 2400   // view units a second, a second
#define PG_TOP 48         // let go with its head this near the top of the screen: it lands hard
static struct {
	bool on, held, falling;
	float x, y, vy, from_y;
	float floor;              // where its feet stand: over miiplay.c's buttons
	int size;                 // tenths of PG_H
	float grab_dx, grab_dy;   // the pointer from the feet, as it took hold
	int expr;                 // MII_EXPR_*, or -1: its own
	int pose;                 // AV_POSE_*
	int freeze_anim;
	float freeze_frame;
	const char *landed;       // the animation it landed with, for HBCS
} pg;

// The effects with the animations: which, at which of its frames. Nothing
// in the files says (the Mii Channel's program does): HBC's choice, from
// where the body is at those frames (the head lunging, a foot down, the
// landing). On the head (zzz, talk, the sneeze: following it) or on the
// ground (smoke), as sdk/mii's mii_fx_place puts them.
static const struct {
	const char *anim;
	float frame;
	const char *fx;
	bool head;
} fx_cues[] = {
	{ "bodysnart_01", 14, "Nc_sneeze01", true },
	{ "bodyTalk", 0, "Nc_talk01", true },
	{ "bodyTalk_01", 0, "Nc_talk01", true },
	{ "bodyTalk_01", 90, "Nc_talk01", true },
	{ "bodyRun", 13, "Nc_run_smoke01", false },
	{ "bodyRun", 33, "Nc_run_smoke01", false },
	{ "bodySlip_00", 26, "Nc_bodyslip01", false },
	{ "bodySlip_01", 31, "Nc_bodyslip01", false },
};

// A Mii coming down from the air (the slaps throw it; bodyGetDown drops it
// in): dust where it lands, as it lands. Its lowest bone (not a root, not
// the shadow's) above AIR_UP is in the air; back under AIR_DOWN, landed.
#define AIR_UP 80
#define AIR_DOWN 45
static void landing(const mii_pose *p) {
	static bool up;
	static int puff;
	float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3], low = 1e9f, at[3][4];
	int i, e;

	mii_body_pose(&model, p, rot, pos);
	for (i = 0; i < body.nbones; ++i)
		if (!strstr(body.bone[i].name, "_root") && i != body.shadow && pos[i][1] < low)
			low = pos[i][1];
	if (low > AIR_UP) {
		up = true;
	} else if (up && low < AIR_DOWN) {
		up = false;
		mii_fx_place(&model, p, 0, at);
		if ((e = mii_fx_find(&fx, puff++ & 1 ? "Nc_slapsmoke02" : "Nc_slapsmoke01")) >= 0)
			fx_played += mii_fx_play(&fxs, e, at) > 0;
	}
}

// A Mii just chosen appears in a puff of the Mii Channel's dust: its smokes
// together, twice the size.
static void poof(const mii_pose *p) {
	static const char *smokes[] = { "Nc_slapsmoke01", "Nc_slapsmoke02", "Nc_bodyslip01" };
	float at[3][4];
	int i, e, r, c;

	mii_fx_place(&model, p, 0, at);
	for (r = 0; r < 3; ++r)
		for (c = 0; c < 3; ++c)
			at[r][c] *= 2;
	at[1][3] += 20;   // around the Mii, not under it
	for (i = 0; i < 3; ++i)
		if ((e = mii_fx_find(&fx, smokes[i])) >= 0)
			fx_played += mii_fx_play(&fxs, e, at) > 0;
}

// The frames since the last: starts each effect whose frame was passed
// (round the end, for a looping animation); zzz while napping. dt in
// seconds.
static void effects(const mii_pose *p, float dt) {
	static int last_anim, zzz, talk;
	static float last_f;
	float at[3][4], head[3][4], f;
	const char *name;
	int i, frames;

	if (!fx_ok)
		return;
	mii_fx_step(&fxs, dt * 60);
	if (poof_asked) {
		poof_asked = false;
		poof(p);
	}
	mii_fx_place(&model, p, 0, at);
	mii_fx_place(&model, p, 1, head);
	if (zzz)
		mii_fx_move(&fxs, zzz, head);
	if (talk)
		mii_fx_move(&fxs, talk, head);
	if (!anim_wait || p->anim < 1 || p->anim > body.nanims) {
		last_anim = 0;
		return;
	}
	if (!p->head_only)
		landing(p);
	name = body.anim[p->anim - 1].name;
	frames = body.anim[p->anim - 1].frames;
	f = fmodf(p->frame, (float) frames);
	if (p->anim != last_anim) {
		if (zzz) {
			mii_fx_stop(&fxs, zzz);
			zzz = 0;
		}
		if (!strcmp(name, "bodyCatnap_00") && (i = mii_fx_find(&fx, "Nc_zzz01")) >= 0)
			zzz = (zzz = mii_fx_play(&fxs, i, head)) > 0 ? zzz : 0;
		fx_played += zzz > 0;
		last_f = -1;   // a cue at frame 0 starts at once
	}
	if (f != last_f || p->anim != last_anim)
		for (i = 0; i < (int) (sizeof(fx_cues) / sizeof(fx_cues[0])); ++i) {
			float q = fx_cues[i].frame;
			bool cue = f >= last_f ? q > last_f && q <= f : q > last_f || q <= f;
			int e;

			if (cue && !strcmp(name, fx_cues[i].anim) && (e = mii_fx_find(&fx, fx_cues[i].fx)) >= 0 &&
					(fx_cues[i].head || !p->head_only)) {   // no ground under an icon
				int h = mii_fx_play(&fxs, e, fx_cues[i].head ? head : at);

				fx_played += h > 0;
				if (!strcmp(fx_cues[i].fx, "Nc_talk01") && h > 0)
					talk = h;
			}
		}
	last_anim = p->anim;
	last_f = f;
}

static void pose_for(mii_pose *p, bool pointer, s32 px, s32 py, float sx, float sy, float sw, float sh) {
	float t = a.t, dt = t - a.last;
	float wave = -1, play_frame = 0;

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
		// Pointed at: a smile and a wave (in the playground, only the smile:
		// the pointer is there to play).
		if (px >= sx && px < sx + sw && py >= sy && py < sy + sh) {
			a.smile_until = t + 1.5f;
			if (a.wave_from < t - 3 && !pg.on)
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
	if (pg.on) {
		// The playground's Mii faces the front, or the way it was told to.
		static const float yaw[AV_POSE_COUNT] = { 0, -1.1f, 1.1f, (float) M_PI };

		p->yaw = (pg.pose < 4 ? yaw[pg.pose] : 0) + 0.04f * sinf(t * 0.45f);
		if (pg.pose == AV_POSE_AROUND)
			p->head_yaw = p->head_pitch = 0;
		if (pg.pose == AV_POSE_UP || pg.pose == AV_POSE_DOWN) {
			p->head_yaw = 0;
			p->head_pitch = pg.pose == AV_POSE_UP ? 0.45f : -0.45f;
		}
		if (pg.pose == AV_POSE_TILT)
			p->head_roll = 0.4f;
		if (pg.pose == AV_POSE_ICON)
			p->head_only = 1;   // its icon, live: the face changing, zzz and talk on it
	}

	// An animation the PC asked for: a one-shot once through, a looping one
	// until the next ask (an unknown name stops it); then the Mii's own go
	// on.
	if (a.play_seen != play_asked) {
		a.play_seen = play_asked;
		a.play = anim_wait ? 1 + mii_body_anim(&body, play_name) : 0;
		a.play_from = t;
	}
	if (a.play) {
		int done;

		play_frame = mii_anim_frame(&body, a.play, t - a.play_from, &done);
		if (done)
			a.play = 0;
	}

	// A wave now and then: the right arm up, the hand going side to side.
	if (t >= a.next_wave && !a.play && !pg.on) {
		a.wave_from = t;
		a.next_wave = t + 18 + rnd() * 20;
		a.smile_until = t + 2.6f;
	}
	if (t - a.wave_from < (anim_greet ? body.anim[anim_greet - 1].frames / 60.0f : 2.4f) && !a.play)
		wave = t - a.wave_from;
	a.raise = approach(a.raise, wave >= 0 ? 2.35f : 0.1f, 7, dt);
	p->arm_raise[0] = a.raise + (wave >= 0 ? 0.28f * sinf(wave * 2 * (float) M_PI * 2.2f) : 0);
	p->arm_raise[1] = 0.1f;
	p->arm_swing[0] = wave >= 0 ? 0.35f : 0.05f * sinf(t * 0.9f);
	p->arm_swing[1] = -0.05f * sinf(t * 0.9f);

	// A Wii body plays its own animations, 60 frames a second: Wait, and
	// Greeting for the wave.
	if (anim_wait) {
		p->anim = wave >= 0 && anim_greet ? anim_greet : anim_wait;
		p->frame = (wave >= 0 && anim_greet ? wave : t) * 60;
		if (a.play) {
			p->anim = a.play;
			p->frame = play_frame;
		}
		if (pg.on && pg.pose == AV_POSE_FREEZE && pg.freeze_anim) {
			p->anim = pg.freeze_anim;
			p->frame = pg.freeze_frame;
		}
		p->bob = 0;
		p->shadow = 1;   // the Mii Channel's, under its feet
		if (pg.on && pg.floor > pg.y) {   // up in the air: it stays on the floor
			float top, half;

			mii_model_bounds(&model, 0, &top, &half);
			p->shadow_drop = (pg.floor - pg.y) * top / (sh * 0.92f);
		}
		// Into the wave and back out: a cross-fade, not a snap.
		if (a.last_anim && p->anim != a.last_anim) {
			a.from_anim = a.last_anim;
			a.from_frame = a.last_frame;
			a.from_t = t;
		}
		a.last_anim = p->anim;
		a.last_frame = p->frame;
		p->from_weight = a.from_anim ? mii_fade_weight(t - a.from_t, FADE_S) : 0;
		if (p->from_weight > 0) {
			p->from_anim = a.from_anim;
			// Wait loops, so it plays on as it fades. The wave (or another
			// one-shot) is done: it holds its last pose (frames wrap, so
			// playing on would fade toward it starting over, the arm going
			// up again).
			p->from_frame = a.from_anim != anim_wait ? a.from_frame : a.from_frame + (t - a.from_t) * 60;
		}
	}

	// The face: smiles now and then; blinks every few seconds.
	if (t >= a.next_smile) {
		a.smile_until = t + 2 + rnd() * 2;
		a.next_smile = t + 10 + rnd() * 14;
	}
	p->expression = t < a.smile_until ? MII_EXPR_SMILE : MII_EXPR_NORMAL;
	if (anim_wait)   // the animation's own: asleep, flung, talking
		p->expression = mii_anim_expression(&body, p->anim, p->frame, p->expression);
	if (pg.on && pg.held)
		p->expression = MII_EXPR_SURPRISE;   // up in the air
	if (t >= a.blink_at) {
		if (t < a.blink_at + 0.13f)
			p->expression = MII_EXPR_BLINK;
		else
			a.blink_at = t + 2 + rnd() * 4;
	}
	if (pg.on && pg.expr >= 0)
		p->expression = pg.expr;   // the one chosen, without blinking
	effects(p, dt);
}

// ---- Each frame -----------------------------------------------------------

static u32 bad;
static char bad_at[96];


#ifdef HBC_DIAG_GPU
// The posed body as the GPU was just sent it: every value finite, every
// position within the body's reach, every normal about unit length. The
// first wild one is kept for the status reply and printed (the kept log).
static void check_pose(const mii_pose *pose) {
	int i;

	if (!model.body)
		return;
	mii_body_skin(&model, pose);   // the CPU reference; the GPU drew from the palette
	if (!model.skin_pos)
		return;   // no memory for it
	for (i = 0; i < 3 * model.body->nverts; ++i) {
		float p = model.skin_pos[i], n = model.skin_nrm[i];

		if (p == p && n == n && p < 400 && p > -400 && n <= 1.01f && n >= -1.01f)
			continue;
		if (!bad++) {
			snprintf(bad_at, sizeof(bad_at), "anim %d frame %d.%03d vertex %d pos %d nrm %d",
					 pose->anim, (int) pose->frame, (int) (pose->frame * 1000) % 1000, i / 3,
					 p == p ? (int) p : -999999, n == n ? (int) (n * 1000) : -999999);
			printf("Avatar: wild vertex: %s\n", bad_at);
		}
		return;
	}
}
#endif

// The Mii's box in the entity, in view units: beside the list, the whole
// entity; in the playground, where its feet are, on a whole-screen entity.
static float box_x, box_y, box_w, box_h;

// mii_view's near and far planes fit a Mii where it stands. Some animations
// throw it far: bodySlap_00 starts it 630 back and 670 up, bodySlap_03 2660
// up, and past the far plane it was not drawn at all (an invisible wall).
// So the planes are widened, frame by frame, to where its bones are (a
// head's reach round each), never narrowed: the face's layers keep all the
// depth they had standing.
static void fit_depth(Mtx44 proj, Mtx view) {
	float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3];
	float a = proj[2][2], b = proj[2][3], n, f, lo, hi, c = cosf(pose.yaw), s = sinf(pose.yaw);
	int i;

	if (!anim_wait || pose.head_only || a == 1 || a == 0)
		return;
	f = b / a;               // GX: proj[2][2] = -n / (f - n), proj[2][3] = -f n / (f - n)
	n = a * f / (a - 1);
	lo = n;
	hi = f;
	mii_body_pose(&model, &pose, rot, pos);
	for (i = 0; i < body.nbones; ++i) {
		float x = c * pos[i][0] + s * pos[i][2], z = -s * pos[i][0] + c * pos[i][2];
		float d = -(view[2][0] * x + view[2][1] * pos[i][1] + view[2][2] * z + view[2][3]);

		if (d - 120 < lo)
			lo = d - 120;
		if (d + 120 > hi)
			hi = d + 120;
	}
	if (lo < 1)
		lo = 1;
	if (lo == n && hi == f)
		return;
	proj[2][2] = -lo / (hi - lo);
	proj[2][3] = -hi * lo / (hi - lo);
}

// HBC's frame reached the avatar: the Mii in its rectangle, a 3D scene of
// its own in a thin band of depth just in front of where a sprite would be.
static void avatar_draw(void *ctx, const gfx_rect *r) {
	u64 t = gettime();
	Mtx44 proj;
	Mtx view;
	float sx, sy, ox, oy;
	int i;

	(void) ctx;
	if (state != AV_READY || mii_view(&model, &pose, box_w / box_h, 0.04f, proj, view) < 0)
		return;
	// The camera frames the Mii in its box; the viewport is the whole
	// entity. So the box is moved and scaled to its place in it (in clip
	// space: x' = x * sx + w * ox, as w is what x is divided by), and an
	// arm or a jump past the box is drawn, not cut off.
	sx = box_w / entity.w;
	sy = box_h / entity.h;
	ox = 2 * (box_x + box_w / 2) / entity.w - 1;
	oy = 1 - 2 * (box_y + box_h / 2) / entity.h;
	for (i = 0; i < 4; ++i) {
		proj[0][i] = proj[0][i] * sx + proj[3][i] * ox;
		proj[1][i] = proj[1][i] * sy + proj[3][i] * oy;
	}
	fit_depth(proj, view);
	// The whole depth range in front of the sprite's, for the Mii's own
	// layers (a thin band left its face a few dozen Z steps: the mask,
	// nose line and glasses broke up on a real Wii); then the rectangle's
	// depth is put back to the sprite's (gfx_depth_fill).
	GX_SetViewport(r->x, r->y, r->w, r->h, 0, r->z_far);
	GX_SetScissor((u32) r->x, (u32) r->y, (u32) (r->w + 0.5f), (u32) (r->h + 0.5f));
	GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
#ifdef HBC_DIAG_GPU
	gfx_diag_token(0x1001);
#endif
	mii_draw(&model, &pose, view, NULL);
	if (fx_ok)
		mii_fx_draw(&fxs, view);   // after the Mii: see-through over it
	gfx_depth_fill(r);
#ifdef HBC_DIAG_GPU
	gfx_diag_token(0x1002);
	check_pose(&pose);
#endif
	draw_us = draw_us - draw_us / 64 + diff_usec(t, gettime()) / 64;
}

// The playground's Mii, each frame: taken hold of with A and B on it (the
// Mii Channel's way), carried, and dropped; it lands on its feet, or from
// higher up trips. It goes anywhere over the floor, all of it in sight (a
// Mii too tall for that keeps its feet on the floor).
static void pg_move(bool pointer, s32 px, s32 py, u32 down, u32 held, float w, float h, float dt) {
	bool both = (held & PADS_A) && (held & PADS_B);

	if (!pg.held && both && (down & (PADS_A | PADS_B)) && pointer &&
			px >= pg.x - w / 2 && px < pg.x + w / 2 && py >= pg.y - h && py < pg.y) {
		pg.held = true;
		pg.falling = false;
		pg.grab_dx = px - pg.x;
		pg.grab_dy = py - pg.y;
		pg.landed = NULL;
		// It squirms, as in the Mii Channel (bodyWallow; with a body
		// without it, its legs go).
		avatar_play(mii_body_anim(&body, "bodyWallow") >= 0 ? "bodyWallow" : "bodyRun");
	}
	if (pg.held) {
		if (both && pointer) {
			pg.x = px - pg.grab_dx;
			pg.y = py - pg.grab_dy;
		} else {
			pg.held = false;
			pg.falling = true;
			pg.vy = 0;
			pg.from_y = pg.y;
		}
	}
	if (pg.falling) {
		float ground = pg.floor;

		pg.vy += PG_GRAVITY * dt;
		pg.y += pg.vy * dt;
		if (pg.y >= ground) {
			pg.y = ground;
			pg.falling = false;
			pg.landed = pg.from_y - h < PG_TOP ? "bodySlip_01" : "bodyGetDown";
			avatar_play(pg.landed);
		}
	}
	// The whole Mii on the screen: its box (the Mii with a little room)
	// inside the edges.
	if (pg.y < h)
		pg.y = h;
	if (pg.y > pg.floor)
		pg.y = pg.floor;
	if (pg.x < w / 2)
		pg.x = w / 2;
	if (pg.x > view_width - w / 2)
		pg.x = view_width - w / 2;
}

const gfx_queue_entry *avatar_frame(bool pointer, s32 px, s32 py, u32 down, u32 held) {
	u64 start_ticks = gettime();
	float stretch = widescreen ? WIDESCREEN_RATIO : 1.0f;
	float w = AV_W * stretch, h = AV_H, x, y, dt;

	hit = false;
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
	dt = a.t;
	a.t = diff_msec(t0, gettime()) / 1000.0f;
	dt = a.t > dt && a.t - dt < 0.1f ? a.t - dt : 1 / 60.0f;
	// Every hour, the clock and every time kept against it start over, so
	// a float's step stays well under a frame however long HBC runs.
	if (a.t >= 3600) {
		t0 += secs_to_ticks(3600);
		a.t -= 3600;
		a.last -= 3600;
		a.next_look -= 3600;
		a.blink_at -= 3600;
		a.smile_until -= 3600;
		a.next_smile -= 3600;
		a.wave_from -= 3600;
		a.from_t -= 3600;
		a.next_wave -= 3600;
	}

	if (pg.on) {
		// The playground: the Mii's box where its feet are, on an entity
		// the size of the screen.
		h = PG_H * pg.size / 10.0f;
		if (pg.pose == AV_POSE_ICON)
			h *= 0.5f;   // the head alone fills its box: an icon's size
		w = h * AV_W / AV_H * stretch;
		if (pg.pose == AV_POSE_ICON) {   // the icon in the middle, over the buttons; not carried
			x = view_width / 2 - w / 2;
			y = pg.floor / 2 - h / 2;
		} else {
			pg_move(pointer, px, py, down, held, w, h, dt);
			x = pg.x - w / 2;
			y = pg.y - h;
		}
		box_x = x;
		box_y = y;
		pose_for(&pose, pointer, px, py, x, y, w, h);
		entity.w = view_width;
		entity.h = view_height;
		x = y = 0;
	} else {
		// In the space left of the app list (320 wide, centred), feet just
		// above the logo.
		x = (view_width / 2 - 160 - w) / 2;
		if (x < 8)
			x = 8;
		y = 404 - h;

		// A on the avatar: the Mii picker (main.c opens it, from the app
		// list).
		hit = (down & PADS_A) && pointer && px >= x && px < x + w && py >= y && py < y + h;
		pose_for(&pose, pointer, px, py, x, y, w, h);
		box_x = box_y = 0;
		entity.w = (u16) (w + 0.5f);
		entity.h = AV_H;
	}
	box_w = w;
	box_h = h;

	// Drawn in its place in HBC's frame (avatar_draw()), not into a texture
	// first: no copy, no pixel-format change.
	entity.type = GFXE_DRAW;
	entity.draw.fn = avatar_draw;
	entity.draw.ctx = NULL;
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
	info->frame_us = frame_us + draw_us;   // posing and drawing
	info->interval_us = interval_us;
	info->frames = frames;
	info->bad = bad;
	snprintf(info->bad_at, sizeof(info->bad_at), "%s", bad_at);
}

// ---- The playground ----------------------------------------------------------

bool avatar_pg_begin(int floor) {
	if (state != AV_READY)
		return false;
	memset(&pg, 0, sizeof(pg));
	pg.on = true;
	pg.expr = -1;
	pg.size = 10;
	pg.floor = floor;
	pg.x = view_width / 2;
	pg.y = floor;
	avatar_play("");   // whatever the PC had it playing stops
	return true;
}

void avatar_pg_end(void) {
	pg.on = false;
	avatar_play("");
	// Its waves were held off while it played; none due the moment it is
	// back.
	a.next_wave = a.t + 18 + rnd() * 20;
}

void avatar_pg_size(int step) {
	pg.size += step;
	if (pg.size < PG_SIZE_MIN)
		pg.size = PG_SIZE_MIN;
	if (pg.size > PG_SIZE_MAX)
		pg.size = PG_SIZE_MAX;
}

void avatar_pg_expression(int expr) {
	pg.expr = expr >= 0 && expr < MII_EXPR_COUNT ? expr : -1;
}

void avatar_pg_pose(int p) {
	// Freeze again plays on; else it holds the frame shown.
	if (p == AV_POSE_FREEZE && pg.pose == AV_POSE_FREEZE)
		p = AV_POSE_STAND;
	if (p == AV_POSE_FREEZE) {
		pg.freeze_anim = a.last_anim;
		pg.freeze_frame = a.last_frame;
	}
	pg.pose = p >= 0 && p < AV_POSE_COUNT ? p : AV_POSE_STAND;
}

const char *avatar_anim_name(int i) {
	return state == AV_READY && anim_wait && i >= 0 && i < body.nanims ? body.anim[i].name : NULL;
}

int avatar_pg_json(char *buf, int size) {
	return snprintf(buf, size, ",\"x\":%d,\"y\":%d,\"held\":%s,"
					"\"falling\":%s,\"landed\":\"%s\",\"expression\":%d,\"pose\":%d,\"size\":%d,"
					"\"floor\":%d,\"effects\":%s,\"particles\":%d,\"played\":%u", (int) pg.x, (int) pg.y,
					pg.held ? "true" : "false", pg.falling ? "true" : "false", pg.landed ? pg.landed : "", pg.expr,
					pg.pose, pg.size, (int) pg.floor, fx_ok ? "true" : "false",
					fx_ok ? mii_fx_particles(&fxs, NULL, 0) : 0, (unsigned) fx_played);
}

int avatar_anims_json(char *buf, int size) {
	int i, n;
	bool ready = state == AV_READY && anim_wait;

	n = snprintf(buf, size, ",\"anim\":\"%s\",\"anims\":[",
				 ready && pose.anim ? body.anim[pose.anim - 1].name : "");
	for (i = 0; ready && i < body.nanims && n < size; ++i)
		n += snprintf(buf + n, size - n, "%s\"%s\"", i ? "," : "", body.anim[i].name);
	if (n < size)
		n += snprintf(buf + n, size - n, "]");
	return n < size ? n : size - 1;
}
