// The Mii playground: HOME > HBC > Mii playground. The avatar's Mii
// (avatar.c) comes out from beside the app list to stand on the whole
// screen. As in the Mii Channel, point at it and hold A and B to pick it
// up; let go to drop it. The buttons along the bottom make it play the Mii
// Channel body's animations, take a pose, or pull a face: three tabs, a
// page of buttons each, the arrows (or the D-pad along the buttons) for the
// next page. + and - make the Mii bigger and smaller. B, or 1, goes back to
// the app list.
//
// The Mii itself, its posing, carrying and dropping are avatar.c's
// (avatar_pg_*); this is the menu around it.

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <ogcsys.h>

#include "../config.h"
#include "controls.h"
#include "theme.h"
#include "font.h"
#include "widgets.h"
#include "view.h"
#include "dialogs.h"
#include "loader.h"
#include "panic.h"
#include "i18n.h"
#include "miidraw.h"
#include "avatar.h"
#include "miiplay.h"

#define PL_TABS 3
#define PL_MAX 6            // buttons on a page, at most
#define PL_ITEMS 24         // on a tab, at most
#define PL_GAP 8
#define PL_SCALE 0.8f       // the buttons and arrows, of the theme's tiny ones
#define PL_ITEM_Y 430       // the bottom row: its top
#define PL_FLOOR_GAP 6      // the Mii's feet, over the tabs

// Smaller copies of the theme's tiny button and arrows: the same textures,
// drawn at the entity's size.
static gfx_entity btn, btn_focus, arrow_l, arrow_l_focus, arrow_r, arrow_r_focus;

// The view's widgets.
#define PL_HINT 0
#define PL_TAB 1
#define PL_PREV (PL_TAB + PL_TABS)
#define PL_NEXT (PL_PREV + 1)
#define PL_PAGE (PL_NEXT + 1)
#define PL_ITEM (PL_PAGE + 1)
#define PL_WIDGETS (PL_ITEM + PL_MAX)

enum { TAB_ANIMS, TAB_POSES, TAB_FACES };

static const char *tab_names[PL_TABS] = { "Moves", "Poses", "Faces" };
static const char *pose_names[AV_POSE_COUNT] = {
	"Stand", "Turn left", "Turn right", "Around", "Look up", "Look down", "Tilt head", "Freeze", "Icon"
};
// The faces: its own first, then MII_EXPR_* in order.
static const char *face_names[1 + MII_EXPR_COUNT] = {
	"Its own", "Normal", "Smile", "Angry", "Sad", "Surprised", "Blink", "Talking"
};

// What the tab shown has: each button's caption and what it does (an
// animation's name; a pose; 1 + an expression).
static struct {
	const char *caption;
	const char *anim;
	int value;
} items[PL_ITEMS];
static int nitems;

static volatile bool open_now, ask;
static volatile int shown_tab, shown_page, shown_pages, per_page;
static char focus_caption[32];

void miiplay_ask(void) {
	ask = true;
}

bool miiplay_asked(void) {
	bool r = ask;

	ask = false;
	return r;
}

static void fill(int tab) {
	int i;
	const char *name;

	nitems = 0;
	if (tab == TAB_ANIMS) {
		items[nitems++] = (typeof(items[0])) { "Stop", "", 0 };
		for (i = 0; (name = avatar_anim_name(i)) && nitems < PL_ITEMS; ++i) {
			items[nitems++] = (typeof(items[0])) {
				!strncmp(name, "body", 4) && name[4] ? name + 4 : name, name, 0
			};
		}
	} else if (tab == TAB_POSES) {
		for (i = 0; i < AV_POSE_COUNT; ++i)
			items[nitems++] = (typeof(items[0])) { pose_names[i], NULL, i };
	} else {
		for (i = 0; i <= MII_EXPR_COUNT; ++i)
			items[nitems++] = (typeof(items[0])) { face_names[i], NULL, i };
	}
}

static void scaled(gfx_entity *e, const gfx_entity *from) {
	*e = *from;
	e->w = (u16) (from->w * PL_SCALE + 0.5f);
	e->h = (u16) (from->h * PL_SCALE + 0.5f);
}

// A button from the smaller copies, its caption with less room either side
// than widget_button_set_caption leaves.
static void button(widget *w, s16 x, s16 y, font_id font, const char *caption) {
	widget_layer *l;
	int i, c;

	widget_button(w, x, y, 1, BTN_TINY, NULL);
	w->width = btn.w;
	w->height = btn.h;
	// Queued again, not just swapped: gfx_qe_entity places an entity by
	// its height.
	for (i = 0; i < 3; ++i) {
		gfx_queue_entry *e = &w->layers[i].gfx_entries[0];

		gfx_qe_entity(e, i == 2 ? &btn_focus : &btn, 0, 0, e->entity.coords.z, e->entity.color);
	}
	l = &w->layers[3];
	free(l->gfx_entries);
	c = font_get_char_count(font, caption, btn.w - 12);
	l->gfx_entry_count = c;
	l->gfx_entries = (gfx_queue_entry *) pmalloc(c * sizeof(gfx_queue_entry));
	font_plot_string(&l->gfx_entries[0], c, font, caption, 6, btn.h / 2, 1, btn.w - 12, FA_CENTERED,
					 FA_EM_CENTERED);
}

static void show(view *v, int tab, int page) {
	u16 bw = btn.w;
	char label[32];
	s16 x0 = (view_width - per_page * bw - (per_page - 1) * PL_GAP) / 2;
	int i, k;

	if (tab != shown_tab || !nitems)
		fill(tab);
	shown_tab = tab;
	shown_pages = (nitems + per_page - 1) / per_page;
	shown_page = page = (page + shown_pages) % shown_pages;
	for (i = 0; i < PL_TABS; ++i) {
		widget *w = &v->widgets[PL_TAB + i];

		widget_free(w);
		button(w, (view_width - PL_TABS * bw - (PL_TABS - 1) * PL_GAP) / 2 + i * (bw + PL_GAP),
			   PL_ITEM_Y - btn.h - PL_GAP, i == tab ? FONT_BUTTON : FONT_BUTTON_DESEL, tab_names[i]);
	}
	// Where they are: page 2 of 5, right of the tabs.
	widget_free(&v->widgets[PL_PAGE]);
	snprintf(label, sizeof(label), _("Page %d of %d"), page + 1, shown_pages);
	widget_label(&v->widgets[PL_PAGE], view_width - 16 - 160, PL_ITEM_Y - btn.h / 2 - PL_GAP, 1, label,
				 160, FA_RIGHT, FA_EM_CENTERED, FONT_LABEL);
	for (i = 0; i < per_page; ++i) {
		widget *w = &v->widgets[PL_ITEM + i];

		k = page * per_page + i;
		widget_free(w);
		if (k >= nitems)
			continue;
		button(w, x0 + i * (bw + PL_GAP), PL_ITEM_Y, FONT_BUTTON, items[k].caption);
	}
	widget_set_flag(&v->widgets[PL_PREV], WF_VISIBLE, shown_pages > 1);
	widget_set_flag(&v->widgets[PL_NEXT], WF_VISIBLE, shown_pages > 1);
	// The buttons are new: the focused one shows it again.
	if (v->focus >= 0)
		widget_set_flag(&v->widgets[v->focus], WF_FOCUSED, true);
}

static void press(int k) {
	if (k < 0 || k >= nitems)
		return;
	if (shown_tab == TAB_ANIMS)
		avatar_play(items[k].anim);
	else if (shown_tab == TAB_POSES)
		avatar_pg_pose(items[k].value);
	else
		avatar_pg_expression(items[k].value - 1);
}

bool show_mii_playground(void) {
	view *v;
	u32 bd, bh, bu;
	u16 bw, aw;
	s16 ay;
	bool b_used = false;

	scaled(&btn, theme_gfx[THEME_BUTTON_TINY]);
	scaled(&btn_focus, theme_gfx[THEME_BUTTON_TINY_FOCUS]);
	scaled(&arrow_l, theme_gfx[THEME_ARROW_LEFT]);
	scaled(&arrow_l_focus, theme_gfx[THEME_ARROW_LEFT_FOCUS]);
	scaled(&arrow_r, theme_gfx[THEME_ARROW_RIGHT]);
	scaled(&arrow_r_focus, theme_gfx[THEME_ARROW_RIGHT_FOCUS]);
	bw = btn.w;
	aw = arrow_l.w;
	ay = PL_ITEM_Y + (btn.h - arrow_l.h) / 2;

	// The floor is just over the tabs.
	if (!avatar_pg_begin(PL_ITEM_Y - btn.h - PL_GAP - PL_FLOOR_GAP))
		return false;

	// As many buttons on a page as fit between the arrows.
	per_page = (view_width - 2 * (16 + aw + 8) + PL_GAP) / (bw + PL_GAP);
	if (per_page > PL_MAX)
		per_page = PL_MAX;
	nitems = 0;
	shown_tab = -1;

	v = view_new(PL_WIDGETS, NULL, 0, 0, TEX_LAYER_DIALOGS, 0);
	widget_label(&v->widgets[PL_HINT], 24, 20, 1, _("A+B: pick up   +/-: size   B: back"),
				 view_width - 48, FA_LEFT, FA_ASCENDER, FONT_HINT);
	widget_image(&v->widgets[PL_PREV], 16, ay, 1, &arrow_l, NULL, true, &arrow_l_focus);
	widget_image(&v->widgets[PL_NEXT], view_width - 16 - aw, ay, 1, &arrow_r, NULL, true,
				 &arrow_r_focus);
	show(v, TAB_ANIMS, 0);
	view_show_logo(false);   // the buttons are where it stands
	open_now = true;
	dialog_fade(v, true);
	view_set_focus(v, PL_ITEM);

	while (true) {
		s8 at;
		int tab = shown_tab, page = shown_page;

		view_plot(v, 0, &bd, &bh, &bu);
		// hbc.py keeps getting answers while the playground is open.
		loader_signal_threads();

		// B on its own goes back; B as half of A+B (the Mii picked up, or
		// a try at it) does not.
		if (bd & PADS_B)
			b_used = (bh & PADS_A) != 0;
		if ((bh & PADS_A) && (bh & PADS_B))
			b_used = true;
		if (((bu & PADS_B) && !b_used) || (bd & PADS_1) || main_exit_asked() ||
				loader_handshaked())
			break;

		// The D-pad: up to the tabs, down to the buttons; along the
		// buttons, off the end of a page onto the next.
		at = v->focus;
		if (bd & (PADS_LEFT | PADS_RIGHT)) {
			int d = bd & PADS_RIGHT ? 1 : -1;

			if (at >= PL_TAB && at < PL_TAB + PL_TABS) {
				view_set_focus(v, PL_TAB + (at - PL_TAB + d + PL_TABS) % PL_TABS);
			} else if (at >= PL_ITEM && at < PL_ITEM + per_page) {
				int k = (shown_page * per_page + at - PL_ITEM + d + nitems) % nitems;

				page = k / per_page;
				if (page != shown_page)
					show(v, tab, page);
				view_set_focus(v, PL_ITEM + k % per_page);
			} else {
				view_set_focus(v, PL_ITEM);
			}
		}
		if (bd & PADS_UP)
			view_set_focus(v, PL_TAB + (shown_tab >= 0 ? shown_tab : 0));
		if (bd & PADS_DOWN)
			view_set_focus(v, PL_ITEM);
		if (bd & PADS_MINUS)
			avatar_pg_size(-1);
		if (bd & PADS_PLUS)
			avatar_pg_size(1);
		// A alone presses a button (A held for A+B does not). The arrows
		// are pictures, which take no focus: the pointer's on them.
		if ((bd & PADS_A) && !(bh & PADS_B)) {
			at = v->focus >= 0 ? v->focus : view_widget_at_ir(v);
			if (at == PL_PREV)
				--page;
			else if (at == PL_NEXT)
				++page;
			else if (at >= PL_TAB && at < PL_TAB + PL_TABS)
				tab = at - PL_TAB;
			else if (at >= PL_ITEM && at < PL_ITEM + per_page)
				press(shown_page * per_page + at - PL_ITEM);
		}
		if (tab != shown_tab)
			show(v, tab, 0);
		else if (page != shown_page)
			show(v, tab, page);
		// A page with fewer buttons: the focus on its last.
		if (v->focus >= PL_ITEM && v->focus < PL_ITEM + per_page &&
				!(v->widgets[v->focus].flags & WF_VISIBLE))
			view_set_focus(v, PL_ITEM + (nitems - 1) % per_page);

		at = v->focus;
		snprintf(focus_caption, sizeof(focus_caption), "%s",
				 at >= PL_TAB && at < PL_TAB + PL_TABS ? tab_names[at - PL_TAB] :
				 at >= PL_ITEM && at < PL_ITEM + per_page &&
				 shown_page * per_page + at - PL_ITEM < nitems ?
				 items[shown_page * per_page + at - PL_ITEM].caption :
				 at == PL_PREV ? "<" : at == PL_NEXT ? ">" : "");
	}

	dialog_fade(v, false);
	open_now = false;
	view_show_logo(true);
	view_free(v);
	avatar_pg_end();
	return true;
}

int miiplay_status_json(char *buf, int size) {
	int n = snprintf(buf, size, ",\"playground\":{\"open\":%s,\"tab\":\"%s\",\"page\":%d,"
					 "\"pages\":%d,\"per_page\":%d,\"focus\":\"%s\"",
					 open_now ? "true" : "false",
					 shown_tab >= 0 && shown_tab < PL_TABS ? tab_names[shown_tab] : "",
					 shown_page, shown_pages, per_page, open_now ? focus_caption : "");

	if (n < size)
		n += avatar_pg_json(buf + n, size - n);
	if (n < size)
		n += snprintf(buf + n, size - n, "}");
	return n < size ? n : size - 1;
}
