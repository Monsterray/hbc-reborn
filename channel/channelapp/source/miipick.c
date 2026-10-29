// The Mii picker: A on the avatar (avatar.c) opens it over the app list, in
// the options popup's dialog. Every Mii in the Mii Channel (RFL_DB.dat) as a
// coin, its head on its favourite colour, a page at a time; a second on a
// coin shows the Mii's name. + and -, or the round arrows beside the dialog,
// turn the page. A on a coin makes that Mii the avatar; B closes it.
//
// The heads are sdk/mii's icons (mii_icon), as the Wii Menu draws them. A
// thread loads the database and the face models and builds one Mii at a
// time, the page shown first; between frames the menu draws that Mii's head
// (mii_icon needs the EFB), lays it on its coin, and hands the model back
// for the thread to free and build the next. Until then the coin is plain.
// Coins are kept while the picker is open, so a page seen once turns at once.

#include "fpurelease.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "../config.h"
#include "controls.h"
#include "theme.h"
#include "font.h"
#include "widgets.h"
#include "view.h"
#include "dialogs.h"
#include "loader.h"
#include "i18n.h"
#include "mii.h"
#include "miidraw.h"
#include "miipick.h"
#include "miiplay.h"   // main_exit_asked

#define MP_COLS 5
#define MP_ROWS 3
#define MP_PAGE (MP_COLS * MP_ROWS)
#define MP_COIN 72          // a coin's side
#define MP_RING (MP_COIN + 12)
#define MP_TIP_W 160        // the name's tooltip
#define MP_TIP_H 32
#define MP_TIP_MS 1000      // on one coin this long: its name
#define MP_GAP_X 20
#define MP_GAP_Y 16
#define MP_TOP 60
#define MP_STACK (64 * 1024)
#define MP_PRIO 30          // under the menu and the avatar's loader

// The dialog's widgets. The tooltip comes last, to draw over the coins.
#define MP_BG 0
#define MP_TITLE 1
#define MP_PAGE_LABEL 2
#define MP_PREV 3
#define MP_NEXT 4
#define MP_TILE 5
#define MP_TIP (MP_TILE + MP_PAGE)
#define MP_TIP_TEXT (MP_TIP + 1)
#define MP_WIDGETS (MP_TIP_TEXT + 1)

typedef struct {
	int slot;
	mii m;
	char name[MII_NAME_UTF8_MAX + 1];
	char id[2 * MII_ID_SIZE + 1];
	bool tried;         // the thread built it (or could not)
	u8 *pixels;         // its coin, MP_COIN x MP_COIN RGBA8
	gfx_entity coin;
} mp_mii;

static mp_mii miis[MII_DB_SLOTS];
static int count;

// The thread's side. loaded: 0 while it reads the database, 1 after, or a
// MII_E_*. ready: the Mii whose model is built (main draws its head), -1
// none, -2 drawn (the thread frees it).
static volatile int loaded, ready = -1, want_page;
static volatile bool quit;
static mii_model built;
static lwp_t thread = LWP_THREAD_NULL;
static u8 stack[MP_STACK] ATTRIBUTE_ALIGN(32);

static gfx_entity ring, tip;

// What HBCS shows (miipick_status_json).
static volatile bool open_now;
static volatile int shown_page, pages, heads, tip_mii = -1, focus_mii = -1;

static void id_hex(const u8 *id, char *out) {
	int i;

	for (i = 0; i < MII_ID_SIZE; ++i)
		sprintf(out + 2 * i, "%02x", id[i]);
}

// The next Mii to build: the page shown first, then the rest in order.
static int next_to_build(void) {
	int i, from = want_page * MP_PAGE;

	for (i = from; i < from + MP_PAGE && i < count; ++i)
		if (!miis[i].tried)
			return i;
	for (i = 0; i < count; ++i)
		if (!miis[i].tried)
			return i;
	return -1;
}

static void *worker_body(void *arg);
static void *worker(void *arg) {
	void *r = worker_body(arg);

	fpu_release();
	return r;
}

static void *worker_body(void *arg) {
	static mii_db db;
	static mii_res res;
	mii_cache *cache;
	int i, r;

	(void) arg;
	count = 0;
	if ((r = mii_db_load(&db)) < 0) {
		hlog("Mii picker: no Mii database (%s)\n", mii_strerror(r));
		loaded = r;
		return NULL;
	}
	for (i = 0; i < MII_DB_SLOTS; ++i) {
		mp_mii *p = &miis[count];

		if (mii_db_get(&db, i, &p->m) < 0)
			continue;
		p->slot = i;
		mii_get_name(p->m.name, p->name, sizeof(p->name));
		id_hex(p->m.id, p->id);
		p->tried = false;
		p->pixels = NULL;
		++count;
	}
	mii_db_free(&db);
	loaded = 1;

	// Without the face models (no Save Mii faces yet) the plain coins.
	if (quit || (r = mii_res_load(&res, NULL)) < 0)
		return NULL;
	cache = mii_cache_new();
	while (!quit) {
		mii_model_opts o = { 64, MII_EXPR_BIT(MII_EXPR_NORMAL), cache, 0, 0 };

		if (ready == -1) {
			if ((i = next_to_build()) < 0)
				break;
			miis[i].tried = true;
			r = mii_model_build(&res, &miis[i].m, &o, &built);
			if (r < 0)
				hlog("Mii picker: slot %d did not build (%s)\n", miis[i].slot, mii_strerror(r));
			else
				ready = i;
			continue;
		}
		if (ready == -2) {
			mii_model_free(&built);
			ready = -1;
			continue;
		}
		usleep(5 * 1000);
	}
	if (ready >= 0 || ready == -2)
		mii_model_free(&built);
	ready = -1;
	if (cache)
		mii_cache_free(cache);
	mii_res_free(&res);
	return NULL;
}

// ---- Textures ---------------------------------------------------------------

// Pixel (x, y) of a w-wide GX RGBA8 texture: 4x4 tiles of 64 bytes, alpha
// and red in the first 32, green and blue in the next. p[0] A, p[1] R,
// p[32] G, p[33] B.
static u8 *texel(u8 *t, int w, int x, int y) {
	return t + ((y / 4) * (w / 4) + x / 4) * 64 + ((y % 4) * 4 + x % 4) * 2;
}

static float clamp01(float v) {
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

// The favourite colours, as the Mii Channel shows them (MII_RED ...).
static const u8 fav_rgb[12][3] = {
	{ 0xd2, 0x1e, 0x14 }, { 0xff, 0x6e, 0x19 }, { 0xff, 0xd8, 0x20 }, { 0x78, 0xd2, 0x20 },
	{ 0x00, 0x78, 0x30 }, { 0x0a, 0x48, 0xb4 }, { 0x3c, 0xaa, 0xde }, { 0xf5, 0x5a, 0x7d },
	{ 0x73, 0x28, 0xad }, { 0x48, 0x38, 0x18 }, { 0xe0, 0xe0, 0xe0 }, { 0x18, 0x18, 0x14 }
};

// A Mii's coin, in place: the head mii_icon left in t (alpha 0 where there is
// none, all of it before the head is drawn) over a disc in its favourite
// colour, lighter at the top, inside a rim; clear outside the circle.
static void make_coin(u8 *t, int fav) {
	const u8 *c = fav_rgb[fav >= 0 && fav < 12 ? fav : 10];
	float mid = (MP_COIN - 1) / 2.0f, r = MP_COIN / 2.0f - 0.5f;
	int x, y, k;

	for (y = 0; y < MP_COIN; ++y)
		for (x = 0; x < MP_COIN; ++x) {
			u8 *p = texel(t, MP_COIN, x, y);
			float d = sqrtf((x - mid) * (x - mid) + (y - mid) * (y - mid));
			float head = p[0] / 255.0f * clamp01(r - 4.5f - d);
			float light = 0.45f - 0.3f * y / MP_COIN;   // white mixed in
			float rim = clamp01(d - (r - 5)) * clamp01(r - 3 - d + 1);
			float edge = clamp01(d - (r - 1.5f));
			u8 src[3] = { p[1], p[32], p[33] };
			u8 out[3];

			for (k = 0; k < 3; ++k) {
				float bg = c[k] + (255 - c[k]) * light;

				bg = bg + (255 - bg) * 0.75f * rim;     // a pale rim
				bg *= 1 - 0.45f * edge;                 // a darker edge
				out[k] = (u8) (src[k] * head + bg * (1 - head) + 0.5f);
			}
			p[0] = (u8) (255 * clamp01(r - d + 0.5f) + 0.5f);
			p[1] = out[0];
			p[32] = out[1];
			p[33] = out[2];
		}
}

// The focus ring: a soft white glow just outside a coin.
static void make_ring(u8 *t) {
	float mid = (MP_RING - 1) / 2.0f, r = MP_COIN / 2.0f + 2.5f;
	int x, y;

	for (y = 0; y < MP_RING; ++y)
		for (x = 0; x < MP_RING; ++x) {
			u8 *p = texel(t, MP_RING, x, y);
			float d = sqrtf((x - mid) * (x - mid) + (y - mid) * (y - mid));
			float a = clamp01(1 - fabsf(d - r) / 3.5f);

			p[0] = (u8) (255 * a * a);
			p[1] = 0xc0;
			p[32] = 0xf0;
			p[33] = 0xff;
		}
}

// The tooltip: a dark rounded box with a faint light border.
static void make_tip(u8 *t) {
	float rr = MP_TIP_H / 2.0f;
	int x, y;

	for (y = 0; y < MP_TIP_H; ++y)
		for (x = 0; x < MP_TIP_W; ++x) {
			u8 *p = texel(t, MP_TIP_W, x, y);
			float cx = x + 0.5f, cy = y + 0.5f, dx = 0, d;

			if (cx < rr)
				dx = rr - cx;
			else if (cx > MP_TIP_W - rr)
				dx = cx - (MP_TIP_W - rr);
			d = sqrtf(dx * dx + (cy - rr) * (cy - rr));   // from the box's spine
			{
				float in = clamp01(rr - d), border = clamp01(1.5f - fabsf(rr - 1 - d));
				u8 v = (u8) (0x18 + (0xd0 - 0x18) * border);

				p[0] = (u8) (in * (0xe0 + 0x1f * border));
				p[1] = v;
				p[32] = (u8) (v + 0x08 * (1 - border));
				p[33] = (u8) (v + 0x18 * (1 - border));
			}
		}
}

static u8 *new_tex(gfx_entity *e, int w, int h, void (*fill)(u8 *)) {
	u8 *t = memalign(32, w * h * 4);

	if (!t)
		return NULL;
	fill(t);
	gfx_gen_tex(e, w, h, t, GFXT_RGBA8);   // flushes it
	return t;
}

// The coins once the database is read: plain, in each Mii's colour.
static void plain_coins(void) {
	int i;

	for (i = 0; i < count; ++i) {
		mp_mii *p = &miis[i];

		p->pixels = memalign(32, MP_COIN * MP_COIN * 4);
		if (!p->pixels)
			continue;
		memset(p->pixels, 0, MP_COIN * MP_COIN * 4);
		make_coin(p->pixels, p->m.fav_color);
		gfx_gen_tex(&p->coin, MP_COIN, MP_COIN, p->pixels, GFXT_RGBA8);
	}
}

// Between frames: the head of the Mii the thread just built, onto its
// coin. Returns its index, or -1.
static int draw_head(void) {
	int i = ready;
	mp_mii *p;

	if (i < 0)
		return -1;
	p = &miis[i];
	if (p->pixels) {
		DCInvalidateRange(p->pixels, MP_COIN * MP_COIN * 4);
		if (mii_icon(&built, MII_EXPR_NORMAL, MP_COIN, (GXColor) { 0, 0, 0, 0 }, p->pixels) < 0)
			memset(p->pixels, 0, MP_COIN * MP_COIN * 4);
		else
			++heads;
		// The CPU lays the head on its coin: wait for the copy first.
		GX_DrawDone();
		DCInvalidateRange(p->pixels, MP_COIN * MP_COIN * 4);
		make_coin(p->pixels, p->m.fav_color);
		DCFlushRange(p->pixels, MP_COIN * MP_COIN * 4);
		gfx_restore();
	}
	ready = -2;
	return i;
}

// ---- The dialog -----------------------------------------------------------

static s16 tile_x(int cell) {
	u16 dw = theme_gfx[THEME_DIALOG]->w;

	return (dw - MP_COLS * MP_COIN - (MP_COLS - 1) * MP_GAP_X) / 2 +
			(cell % MP_COLS) * (MP_COIN + MP_GAP_X);
}

static s16 tile_y(int cell) {
	return MP_TOP + (cell / MP_COLS) * (MP_COIN + MP_GAP_Y);
}

// Cell `cell` of the page shown: Mii i, or nothing past the last.
static void set_tile(view *v, int cell, int i) {
	widget *w = &v->widgets[MP_TILE + cell];

	widget_free(w);
	if (i >= count || !miis[i].pixels)
		return;
	widget_mii_coin(w, tile_x(cell), tile_y(cell), 1, &miis[i].coin, &ring);
	if (v->focus == MP_TILE + cell)
		widget_set_flag(w, WF_FOCUSED, true);
	if (v->cursor == MP_TILE + cell)
		widget_set_flag(w, WF_CURSOR, true);
}

static void show_page(view *v, int page) {
	char label[32];
	int c;

	want_page = shown_page = page;
	for (c = 0; c < MP_PAGE; ++c)
		set_tile(v, c, page * MP_PAGE + c);
	widget_free(&v->widgets[MP_PAGE_LABEL]);
	if (count)
		snprintf(label, sizeof(label), "%d / %d", page + 1, pages);
	else
		snprintf(label, sizeof(label), "%s", loaded < 0 ? _("No Mii database") : _("No Miis"));
	widget_label(&v->widgets[MP_PAGE_LABEL], 32, theme_gfx[THEME_DIALOG]->h - 28, 1, label,
				 theme_gfx[THEME_DIALOG]->w - 64, FA_CENTERED, FA_EM_CENTERED, FONT_LABEL);
	widget_set_flag(&v->widgets[MP_PREV], WF_VISIBLE, pages > 1);
	widget_set_flag(&v->widgets[MP_NEXT], WF_VISIBLE, pages > 1);
}

// The name of Mii i in its tooltip, under its coin (over it on the bottom
// row); i < 0 hides it.
static void show_tip(view *v, int i) {
	widget *w = &v->widgets[MP_TIP];
	int cell = i % MP_PAGE;
	s16 x, y;

	tip_mii = i;
	widget_free(&v->widgets[MP_TIP_TEXT]);
	widget_set_flag(w, WF_VISIBLE, i >= 0);
	if (i < 0)
		return;
	x = tile_x(cell) + (MP_COIN - MP_TIP_W) / 2;
	y = cell / MP_COLS < MP_ROWS - 1 ? tile_y(cell) + MP_COIN + 2 : tile_y(cell) - MP_TIP_H - 2;
	w->coords.x = x;
	w->coords.y = y;
	widget_label(&v->widgets[MP_TIP_TEXT], x + 8, y + MP_TIP_H / 2, 4, miis[i].name,
				 MP_TIP_W - 16, FA_CENTERED, FA_EM_CENTERED, FONT_LABEL);
}

// The D-pad: through the grid, and off its sides to the page before or after.
static int move(view *v, int page, u32 bd) {
	int cell = v->focus - MP_TILE, on_page = count - page * MP_PAGE;

	if (on_page > MP_PAGE)
		on_page = MP_PAGE;
	if (cell < 0 || cell >= MP_PAGE) {
		// Nothing focused (the pointer was on the screen): start on the grid.
		view_set_focus(v, MP_TILE);
		return page;
	}
	if (bd & PADS_LEFT) {
		if (cell % MP_COLS)
			--cell;
		else if (pages > 1) {
			page = (page + pages - 1) % pages;
			cell += MP_COLS - 1;
		}
	} else if (bd & PADS_RIGHT) {
		if (cell % MP_COLS < MP_COLS - 1 && cell + 1 < on_page)
			++cell;
		else if (pages > 1) {
			page = (page + 1) % pages;
			cell -= cell % MP_COLS;
		}
	} else if (bd & PADS_UP) {
		if (cell >= MP_COLS)
			cell -= MP_COLS;
	} else if (bd & PADS_DOWN) {
		if (cell + MP_COLS < on_page)
			cell += MP_COLS;
	}
	on_page = count - page * MP_PAGE;
	if (cell >= on_page)
		cell = on_page - 1;
	view_set_focus(v, MP_TILE + cell);
	return page;
}

int show_mii_picker(const view *sub_view, const char *current, char *id_out) {
	view *v;
	u32 bd, bu;
	int page = -1, i, chosen = -1, at, hover = -1;
	u64 hover_at = 0;
	u8 *ring_px, *tip_px;
	u16 dw = theme_gfx[THEME_DIALOG]->w;
	u16 aw = theme_gfx[THEME_ARROW_LEFT]->w, ah = theme_gfx[THEME_ARROW_LEFT]->h;
	s16 dx = (view_width - dw) / 2, margin = dx, ax;
	s16 ay = MP_TOP + (MP_ROWS * (MP_COIN + MP_GAP_Y) - MP_GAP_Y - ah) / 2;

	count = 0;
	loaded = 0;
	ready = -1;
	want_page = 0;
	quit = false;
	heads = pages = shown_page = 0;
	tip_mii = -1;
	if (LWP_CreateThread(&thread, worker, NULL, stack, MP_STACK, MP_PRIO) < 0) {
		thread = LWP_THREAD_NULL;
		loaded = MII_E_MEM;
	}
	ring_px = new_tex(&ring, MP_RING, MP_RING, make_ring);
	tip_px = new_tex(&tip, MP_TIP_W, MP_TIP_H, make_tip);

	v = view_new(MP_WIDGETS, sub_view, dx, 44, TEX_LAYER_DIALOGS, PADS_B);
	widget_image(&v->widgets[MP_BG], 0, 0, 0, theme_gfx[THEME_DIALOG], NULL, false, NULL);
	widget_label(&v->widgets[MP_TITLE], 32, 16, 1, _("Choose your Mii"), dw - 64, FA_CENTERED,
				 FA_ASCENDER, FONT_DLGTITLE);
	// The app list's round arrows, in the middle of the space either side
	// of the dialog (on a 4:3 screen that space is a little narrower than
	// an arrow: then at the screen's edge).
	ax = (margin - aw) / 2;
	if (ax < 0)
		ax = 0;
	widget_image(&v->widgets[MP_PREV], ax - dx, ay, 2, theme_gfx[THEME_ARROW_LEFT], NULL, true,
				 theme_gfx[THEME_ARROW_LEFT_FOCUS]);
	widget_image(&v->widgets[MP_NEXT], view_width - ax - aw - dx, ay, 2,
				 theme_gfx[THEME_ARROW_RIGHT], NULL, true, theme_gfx[THEME_ARROW_RIGHT_FOCUS]);
	widget_set_flag(&v->widgets[MP_PREV], WF_VISIBLE, false);
	widget_set_flag(&v->widgets[MP_NEXT], WF_VISIBLE, false);
	if (tip_px) {
		widget_image(&v->widgets[MP_TIP], 0, 0, 3, &tip, NULL, false, NULL);
		// Under the pointer it is only a picture: the coin keeps the pointer.
		v->widgets[MP_TIP].width = v->widgets[MP_TIP].height = 0;
		widget_set_flag(&v->widgets[MP_TIP], WF_VISIBLE, false);
	}
	open_now = true;

	view_show_throbber(true);
	dialog_fade(v, true);

	while (true) {
		if (page < 0 && loaded) {
			// The database is read: the page with the avatar's Mii on it.
			view_show_throbber(false);
			plain_coins();
			pages = (count + MP_PAGE - 1) / MP_PAGE;
			at = 0;
			for (i = 0; i < count; ++i)
				if (current && !strcmp(miis[i].id, current))
					at = i;
			page = count ? at / MP_PAGE : 0;
			show_page(v, page);
			if (count)
				view_set_focus(v, MP_TILE + at % MP_PAGE);
		}

		view_plot(v, DIALOG_MASK_COLOR, &bd, NULL, &bu);
		// hbc.py keeps getting answers while the picker is open.
		loader_signal_threads();

		// Between frames, the head the thread built.
		draw_head();

		if (view_back(v, bu) || (bd & PADS_1) || main_exit_asked() ||
				loader_handshaked())
			break;
		if (page < 0 || !count)
			continue;

		at = page;
		if (bd & PADS_MINUS)
			at = (page + pages - 1) % pages;
		if (bd & PADS_PLUS)
			at = (page + 1) % pages;
		if (bd & PADS_DPAD)
			at = move(v, page, bd);
		if (bd & PADS_A) {
			s8 w = view_widget_at_ir(v);

			if (w == MP_PREV && (v->widgets[MP_PREV].flags & WF_VISIBLE))
				at = (page + pages - 1) % pages;
			else if (w == MP_NEXT && (v->widgets[MP_NEXT].flags & WF_VISIBLE))
				at = (page + 1) % pages;
			else if (v->focus >= MP_TILE && v->focus < MP_TILE + MP_PAGE) {
				chosen = page * MP_PAGE + v->focus - MP_TILE;
				break;
			}
		}
		if (at != page) {
			int f = v->focus;

			page = at;
			show_page(v, page);
			// The D-pad's focus stays in its cell, or the last one there.
			if (f >= MP_TILE && f < MP_TILE + MP_PAGE &&
					!(v->widgets[f].flags & WF_VISIBLE))
				view_set_focus(v, MP_TILE + (count - 1) % MP_PAGE);
		}

		// A second on one coin, by pointer or D-pad: its name.
		i = v->focus >= MP_TILE && v->focus < MP_TILE + MP_PAGE ?
			page * MP_PAGE + v->focus - MP_TILE : -1;
		focus_mii = i;
		if (i != hover) {
			hover = i;
			hover_at = gettime();
			if (tip_mii >= 0)
				show_tip(v, -1);
		} else if (hover >= 0 && tip_mii != hover && tip_px &&
				diff_msec(hover_at, gettime()) >= MP_TIP_MS) {
			show_tip(v, hover);
		}
	}

	view_show_throbber(false);
	dialog_fade(v, false);
	view_free(v);

	quit = true;
	if (thread != LWP_THREAD_NULL) {
		LWP_JoinThread(thread, NULL);
		thread = LWP_THREAD_NULL;
	}
	if (chosen >= 0) {
		strcpy(id_out, miis[chosen].id);
		hlog("Mii picker: %s (slot %d)\n", miis[chosen].name, miis[chosen].slot);
		chosen = miis[chosen].slot;
	}
	for (i = 0; i < count; ++i) {
		free(miis[i].pixels);
		miis[i].pixels = NULL;
	}
	free(ring_px);
	free(tip_px);
	open_now = false;
	tip_mii = focus_mii = -1;
	return chosen;
}

int miipick_status_json(char *buf, int size) {
	return snprintf(buf, size, ",\"mii_picker\":{\"open\":%s,\"miis\":%d,\"page\":%d,"
					"\"pages\":%d,\"heads\":%d,\"focus\":%d,\"tip\":%d}",
					open_now ? "true" : "false", open_now ? count : 0, shown_page + 1, pages, heads,
					open_now ? focus_mii : -1, open_now ? tip_mii : -1);
}
