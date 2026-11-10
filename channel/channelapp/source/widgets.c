#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "../config.h"
#include "theme.h"
#include "i18n.h"
#include "panic.h"

#include "widgets.h"

static const char *app_entry_default_description;

// Options > App badges: a solid pill in a tile's top-right corner, "New" or
// "Updated", and for Updated an amber light inside the tile's top edge in the
// same colour. The shapes are white textures, tinted with the theme's badge colours
// (theme.badge_new, theme.badge_updated) when drawn.
#define BADGE_H 16
#define BADGE_MAX_W 96
#define BADGE_INSET 8
#define GLOW_Z 0                         // over the button, under the name
#define BADGE_GLOW_TAU (13.0f / 3.0f)   // how far the light reaches down, in rows

enum { BADGE_NEW, BADGE_UPDATED, BADGE_COUNT };

typedef struct {
	const char *label;
	int text_w;           // what the label measures, in view units
	gfx_entity pill;
	u8 pixels[BADGE_MAX_W * BADGE_H * 4] ATTRIBUTE_ALIGN(32);
} badge_pill;

static badge_pill badge_pills[BADGE_COUNT];
typedef struct {
	gfx_entity entity;
	u8 *pixels;           // malloc'd with the button's size, I8; NULL if none
} badge_glow;

static badge_glow badge_glow_grid, badge_glow_list;

// A white RGBA8 pixel with coverage a. GX RGBA8 is 4x4 tiles, each with an
// AR plane and a GB plane.
static void badge_pixel(u8 *pixels, int w, int x, int y, u8 a) {
	int p = ((y / 4) * (w / 4) + x / 4) * 64 + ((y % 4) * 4 + x % 4) * 2;

	pixels[p] = a;
	pixels[p + 1] = 255;
	pixels[p + 32] = 255;
	pixels[p + 33] = 255;
}

static void badge_gen_pill(badge_pill *bp, const char *label) {
	int x, y, w, r = BADGE_H / 2;
	float ratio = widescreen ? WIDESCREEN_RATIO : 1.0f;

	bp->label = label;
	bp->text_w = (int) (font_get_string_width(FONT_BADGE, label, strlen(label)) * ratio + 0.5f);
	w = (bp->text_w + BADGE_H + 3) & ~3;   // rounded ends and a little air
	if (w > BADGE_MAX_W)
		w = BADGE_MAX_W;
	memset(bp->pixels, 0, sizeof(bp->pixels));
	for (y = 0; y < BADGE_H; ++y)
		for (x = 0; x < w; ++x) {
			// Distance from this pixel's centre to the segment between the
			// two end circles' centres.
			float px = x + 0.5f, py = y + 0.5f;
			float cx = px < r ? r : px > w - r ? w - r : px;
			float dx = px - cx, dy = py - r;
			float cover = r + 0.5f - sqrtf(dx * dx + dy * dy);

			badge_pixel(bp->pixels, w, x, y,
					cover >= 1.0f ? 255 : cover <= 0.0f ? 0 : (u8) (cover * 255));
		}
	gfx_gen_tex(&bp->pill, w, BADGE_H, bp->pixels, GFXT_RGBA8);
}

// The Updated light: from the button's own top outline it fades out downward,
// hugging the rounded corners. Built from the button picture's alpha: per
// column, the first row over half alpha is the edge, and the light there is
// e^(-(y - edge) / BADGE_GLOW_TAU) times the picture's alpha, clipped to the
// button. An I8 picture the size of the button, tinted with the badge colour.
static void badge_gen_glow(badge_glow *g, const gfx_entity *button) {
	int x, y, w = button->w, h = button->h;
	int wp = (w + 7) & ~7, hp = (h + 3) & ~3;
	const u8 *src = button->texture.pixels;
	u8 *pixels;

	if (w < 1 || h < 1 || !src)
		return;
	pixels = memalign(32, wp * hp);
	if (!pixels)
		return;
	memset(pixels, 0, wp * hp);
	for (x = 0; x < w; ++x) {
		int first = -1;

		for (y = 0; y < h; ++y) {
			// RGBA8 tiles: the AR plane comes first, A is the even byte.
			u8 a = src[((y / 4) * (button->w / 4) + x / 4) * 64 + ((y % 4) * 4 + x % 4) * 2];

			if (first < 0) {
				if (a <= 127)
					continue;
				first = y;
			}
			if (y - first > 40)
				break;
			pixels[((y / 4) * (wp / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8] =
					(u8) (expf(-(y - first) / BADGE_GLOW_TAU) * a * 0.95f + 0.5f);
		}
	}
	gfx_gen_tex(&g->entity, wp, hp, pixels, GFXT_I8);
	free(g->pixels);
	g->pixels = pixels;
}

static void badge_theme_reinit(void) {
	badge_gen_pill(&badge_pills[BADGE_NEW], _("New"));
	badge_gen_pill(&badge_pills[BADGE_UPDATED], _("Updated"));
	badge_gen_glow(&badge_glow_grid, theme_gfx[THEME_GRID_APP_ENTRY]);
	badge_gen_glow(&badge_glow_list, theme_gfx[THEME_APP_ENTRY]);
}

void widgets_theme_reinit (void) {
	app_entry_default_description = _("<no description>");
	badge_theme_reinit();
}

void widgets_init (void) {
	widgets_theme_reinit();
}

void widgets_deinit (void) {
}

void widget_free (widget *w) {
	u32 i;

	for (i = 0; i < w->layer_count; ++i)
		free (w->layers[i].gfx_entries);

	switch (w->type) {
	case WT_LABEL:
	case WT_BUTTON:
	case WT_APP_ENTRY:
	case WT_MEMO:
	case WT_IMAGE:
		break;

	case WT_PROGRESS:
		free (w->progress.gradient);
		break;

	case WT_GRADIENT:
		free (w->gradient.gradient);
		break;
	}

	free (w->layers);

	memset(w, 0, sizeof(widget));
}

void widget_set_flag (widget *w, u32 flag, bool set) {
	if (set)
		w->flags |= flag;
	else
		w->flags &= ~((u32) flag);
}

void widget_toggle_flag (widget *w, u32 flag) {
	widget_set_flag (w, flag, (w->flags & flag) ? false : true);
}

void widget_set_progress (widget *w, u32 progress) {
	float m = 0;
	char p[5];

	if (progress > 0)
		m = (float) progress / w->progress.max;

	if (m > 1)
		m = 1;

	gfx_gen_gradient (w->progress.gradient, roundf ((400 - 36 * 2 + 2) * m),
						112 - 36 * 2 + 2, theme.progress.ul, theme.progress.ur,
						theme.progress.lr, theme.progress.ll);
	gfx_qe_entity (&w->layers[0].gfx_entries[0], w->progress.gradient, 35, 35,
					0, COL_DEFAULT);

	sprintf (p, "%3.f%%", roundf (100.0 * m));
	font_plot_string (&w->layers[2].gfx_entries[0], 4, FONT_LABEL, p,
						32, theme_gfx[THEME_PROGRESS]->h - 20, 2,
						theme_gfx[THEME_PROGRESS]->w - 70,
						FA_RIGHT, FA_EM_CENTERED);
}

#define CCLAMP(x) (((x)>255)?255:(((x)<0)?0:(x)))

void widget_fade_gradient (widget *w, s8 modifier) {
	w->layers[0].gfx_entries[0].entity.entity->gradient.c1 =
		CCLAMP(w->layers[0].gfx_entries[0].entity.entity->gradient.c1 + modifier);
	w->layers[0].gfx_entries[0].entity.entity->gradient.c2 =
		CCLAMP(w->layers[0].gfx_entries[0].entity.entity->gradient.c2 + modifier);
	w->layers[0].gfx_entries[0].entity.entity->gradient.c3 =
		CCLAMP(w->layers[0].gfx_entries[0].entity.entity->gradient.c3 + modifier);
	w->layers[0].gfx_entries[0].entity.entity->gradient.c4 =
		CCLAMP(w->layers[0].gfx_entries[0].entity.entity->gradient.c4 + modifier);
}

bool widget_scroll_memo (widget *w, s16 modifier) {
	s16 y;

	y = w->coords.y + modifier;

	if (y < w->memo.y_min)
		y = w->memo.y_min;

	if (y > w->memo.y_max)
		y = w->memo.y_max;

	if (w->coords.y == y)
		return false;

	w->coords.y = y;

	return true;
}

bool widget_scroll_memo_deco (widget *w, s16 modifier) {
	bool res;

	res = widget_scroll_memo (&w[1], modifier);

	widget_set_flag (&w[0], WF_VISIBLE, w[1].coords.y < w[1].memo.y_max);
	widget_set_flag (&w[2], WF_VISIBLE, w[1].coords.y > w[1].memo.y_min);

	return res;
}

static void widget_init (widget *w, widget_type type, u32 focusable,
						 s16 x, s16 y, s16 z, u16 width, u16 height,
						 int layer_count) {
	w->type = type;
	w->flags = WF_VISIBLE | WF_ENABLED;

	w->width = width;
	w->height = height;

	w->cur = CUR_STD;

	if (focusable)
		w->flags |= WF_FOCUSABLE;

	w->coords.x = x;
	w->coords.y = y;
	w->coords.z = TEX_LAYER_WIDGETS + z;
	gfx_qe_origin_push (&w->qe_coords_push, &w->coords);
	gfx_qe_origin_pop (&w->qe_coords_pop);

	w->layer_count = layer_count;
	w->layers = (widget_layer *)
				pmalloc (w->layer_count * sizeof (widget_layer));
}

void widget_label (widget *w, s16 x, s16 y, s16 z, const char *caption,
				   u16 width, font_xalign xalign, font_yalign yalign,
				   font_id font) {
	widget_layer *l;

	widget_init (w, WT_LABEL, false, x, y, z, 0, 0, 1);

	l = &w->layers[0];
	l->flags = 0;
	l->gfx_entry_count = font_get_char_count (font, caption, width);
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	font_plot_string (&l->gfx_entries[0], l->gfx_entry_count, font, caption,
						0, 0, 0, width, xalign, yalign);
}

void widget_image(widget *w, s16 x, s16 y, s16 z, gfx_entity *image,
				  gfx_entity *image_disabled, bool rumble,
				  gfx_entity *image_cursor) {
	int c;
	widget_layer *l;

	c = 1;

	if (image_disabled)
		c++;

	if (image_cursor)
		c++;

	widget_init (w, WT_IMAGE, false, x, y, z, image->w, image->h, c);

	if (rumble)
		w->flags |= WF_RUMBLE;

	c = 0;

	l = &w->layers[c];
	if(image_cursor) {
		l->flags = WF_ENABLED | WF_FLAGS_AND | WF_CURSOR;
		l->flags_invert = WF_CURSOR;
	} else {
		l->flags = WF_ENABLED;
		l->flags_invert = 0;
	}

	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], image, 0, 0, 0, COL_DEFAULT);

	c++;

	if (image_disabled) {
		l = &w->layers[c];
		l->flags = WF_ENABLED;
		l->flags_invert = WF_ENABLED;
		l->gfx_entry_count = 1;
		l->gfx_entries = (gfx_queue_entry *) pmalloc (l->gfx_entry_count *
							sizeof (gfx_queue_entry));
		gfx_qe_entity (&l->gfx_entries[0], image_disabled, 0, 0, 1, COL_DEFAULT);

		c++;
	}

	if (image_cursor) {
		l = &w->layers[c];
		l->flags = WF_ENABLED | WF_FLAGS_AND | WF_CURSOR;
		l->flags_invert = 0;
		l->gfx_entry_count = 1;
		l->gfx_entries = (gfx_queue_entry *) pmalloc (l->gfx_entry_count *
							sizeof (gfx_queue_entry));
		gfx_qe_entity (&l->gfx_entries[0], image_cursor, 0, 0, 2, COL_DEFAULT);
	}
}

void widget_button (widget *w, s16 x, s16 y, s16 z, button_size size,
					const char *caption) {
	gfx_entity *tex, *tex_focus;
	widget_layer *l;

	switch (size) {
	case BTN_SMALL:
		tex = theme_gfx[THEME_BUTTON_SMALL];
		tex_focus = theme_gfx[THEME_BUTTON_SMALL_FOCUS];
		break;

	case BTN_TINY:
		tex = theme_gfx[THEME_BUTTON_TINY];
		tex_focus = theme_gfx[THEME_BUTTON_TINY_FOCUS];
		break;

	default:
		tex = theme_gfx[THEME_BUTTON];
		tex_focus = theme_gfx[THEME_BUTTON_FOCUS];
		break;
	}

	widget_init (w, WT_BUTTON, true, x, y, z, tex->w, tex->h, 4);

	w->flags |= WF_RUMBLE;

	l = &w->layers[0];
	l->flags = WF_FOCUSED | WF_FLAGS_AND | WF_ENABLED;
	l->flags_invert = WF_FOCUSED;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	gfx_qe_entity (&l->gfx_entries[0], tex, 0, 0, 0, COL_DEFAULT);

	l = &w->layers[1];
	l->flags = WF_ENABLED;
	l->flags_invert = WF_ENABLED;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	gfx_qe_entity (&l->gfx_entries[0], tex, 0, 0, 0, WIDGET_DISABLED_COLOR);

	l = &w->layers[2];
	l->flags = WF_FOCUSED | WF_FLAGS_AND | WF_ENABLED;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], tex_focus, 0, 0, 0, COL_DEFAULT);

	l = &w->layers[3];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = 0;
	l->gfx_entries = NULL;

	widget_button_set_caption(w, FONT_BUTTON, caption);
}

// Draw a button smaller, its box and its caption's room too, where a dialog
// is short of room. Call it before the caption is set.
void widget_button_scale(widget *w, f32 scale) {
	gfx_entity *tex = w->layers[0].gfx_entries[0].entity.entity;
	// A scaled entity shrinks about its middle: move it back to the corner.
	f32 dx = tex->w * (1.0f - scale) / 2, dy = tex->h * (1.0f - scale) / 2;
	int i;

	for (i = 0; i < 3; ++i) {
		gfx_queue_entry *e = &w->layers[i].gfx_entries[0];

		e->entity.scale = scale;
		e->entity.coords.x = -dx;
		e->entity.coords.y = tex->h - dy;
	}
	w->width = tex->w * scale;
	w->height = tex->h * scale;
}

void widget_button_set_caption(widget *w, font_id font, const char *caption) {
	int c;
	u16 bw, bh, oy, margin;
	widget_layer *l;

	l = &w->layers[3];
	free(l->gfx_entries);
	l->gfx_entry_count = 0;
	l->gfx_entries = NULL;

	if (!caption)
		return;

	bw = w->width;
	bh = w->height;
	// A scaled button (widget_button_scale) has less to spare.
	margin = bw < w->layers[0].gfx_entries[0].entity.entity->w ? 12 : 32;

	c = font_get_char_count(font, caption, bw - margin);

	oy = bh/2;

	l->gfx_entry_count = c;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	font_plot_string (&l->gfx_entries[0], c, font, caption, margin / 2, oy, 1,
						bw - margin, FA_CENTERED, FA_EM_CENTERED);
}

// An app's badge goes in the widget's last layer, empty when unbadged.
static void widget_new_badge(widget *w, const app_entry *entry, badge_glow *glow_src) {
	widget_layer *l = &w->layers[w->layer_count - 1];
	const badge_pill *bp;
	const theme_badge_t *colors;
	bool updated = entry->run_state == APP_RUN_UPDATED;
	const badge_glow *glow = updated && glow_src->pixels ? glow_src : NULL;
	int count, glyphs, n = 0, x;
	u16 pill_x, text_w;

	memset(l, 0, sizeof(*l));
	if (!settings.new_badges || entry->run_state == APP_RUN_UNCHANGED || entry->type == AET_THEME)
		return;
	bp = &badge_pills[updated ? BADGE_UPDATED : BADGE_NEW];
	colors = updated ? &theme.badge_updated : &theme.badge_new;
	// A tiny theme tile would wrap the u16.
	x = w->width - BADGE_INSET - bp->pill.w;
	pill_x = x < 0 ? 0 : x;
	text_w = bp->pill.w - 6;
	glyphs = font_get_char_count(FONT_BADGE, bp->label, text_w);
	count = 1 + glyphs + (glow ? 1 : 0);
	l->gfx_entry_count = count;
	l->gfx_entries = pmalloc(count * sizeof(gfx_queue_entry));
	gfx_qe_entity(&l->gfx_entries[n++], (gfx_entity *) &bp->pill, pill_x, BADGE_INSET, 2,
			colors->fill);
	if (glyphs) {
		int i;

		font_plot_string(&l->gfx_entries[n], glyphs, FONT_BADGE, bp->label, pill_x + 3,
				BADGE_INSET + BADGE_H / 2, 3, text_w, FA_CENTERED, FA_EM_CENTERED);
		// The glyphs take the pill's text colour, not the theme's font colour.
		for (i = 0; i < glyphs; ++i)
			if (l->gfx_entries[n + i].type == GFXQ_ENTITY)
				l->gfx_entries[n + i].entity.color = colors->text;
		n += glyphs;
	}
	if (glow)
		gfx_qe_entity(&l->gfx_entries[n++], &glow->entity, 0, 0, GLOW_Z, colors->fill);
}

void widget_grid_app_entry(widget *w, s16 x, s16 y, s16 z,
						   const app_entry *entry) {

	widget_layer *l;

	if (entry->icon)
		widget_init(w, WT_APP_ENTRY, true, x, y, z,
					theme_gfx[THEME_GRID_APP_ENTRY]->w,
					theme_gfx[THEME_GRID_APP_ENTRY]->h, 5);
	else
		widget_init(w, WT_APP_ENTRY, true, x, y, z,
					theme_gfx[THEME_GRID_APP_ENTRY]->w,
					theme_gfx[THEME_GRID_APP_ENTRY]->h, 4);

	// Enable rumbling for this widget
	w->flags |= WF_RUMBLE;

	l = &w->layers[0];
	l->flags = WF_FOCUSED | WF_FLAGS_AND | WF_ENABLED;
	l->flags_invert = WF_FOCUSED;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], theme_gfx[THEME_GRID_APP_ENTRY],
					0, 0, 0, COL_DEFAULT);

	l = &w->layers[1];
	l->flags = WF_ENABLED;
	l->flags_invert = WF_ENABLED;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	gfx_qe_entity (&l->gfx_entries[0], theme_gfx[THEME_GRID_APP_ENTRY],
					0, 0, 0, WIDGET_DISABLED_COLOR);

	l = &w->layers[2];
	l->flags = WF_FOCUSED | WF_FLAGS_AND | WF_ENABLED;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], theme_gfx[THEME_GRID_APP_ENTRY_FOCUS],
					0, 0, 0, COL_DEFAULT);

	if (entry->icon) {
		l = &w->layers[3];
		l->flags = 0;
		l->flags_invert = 0;
		l->gfx_entry_count = 1;

		l->gfx_entry_count = 1;
		l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

		gfx_qe_entity (&l->gfx_entries[0], entry->icon, GRID_APP_ENTRY_ICON_X,
						APP_ENTRY_ICON_Y, 1, COL_DEFAULT);
	}
	widget_new_badge(w, entry, &badge_glow_grid);
}

// A Mii in the Mii picker (miipick.c): its coin, and the glow ring around
// it while it has the focus.
void widget_mii_coin(widget *w, s16 x, s16 y, s16 z, gfx_entity *coin,
						gfx_entity *ring) {
	widget_layer *l;

	widget_init(w, WT_APP_ENTRY, true, x, y, z, coin->w, coin->h, 2);
	w->flags |= WF_RUMBLE;

	l = &w->layers[0];
	l->flags = WF_FOCUSED | WF_FLAGS_AND | WF_ENABLED;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *) pmalloc (sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], ring, -(ring->w - coin->w) / 2,
					-(ring->h - coin->h) / 2, 0, COL_DEFAULT);

	l = &w->layers[1];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *) pmalloc (sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], coin, 0, 0, 1, COL_DEFAULT);
}

void widget_app_entry (widget *w, s16 x, s16 y, s16 z, const app_entry *entry) {
	const char *line1, *line2;
	int l1, l2;

	widget_layer *l;

	widget_init (w, WT_APP_ENTRY, true, x, y, z, theme_gfx[THEME_APP_ENTRY]->w,
					theme_gfx[THEME_APP_ENTRY]->h, 5);

	w->flags |= WF_RUMBLE;

	l = &w->layers[0];
	l->flags = WF_FOCUSED | WF_FLAGS_AND | WF_ENABLED;
	l->flags_invert = WF_FOCUSED;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], theme_gfx[THEME_APP_ENTRY],
					0, 0, 0, COL_DEFAULT);

	l = &w->layers[1];
	l->flags = WF_ENABLED;
	l->flags_invert = WF_ENABLED;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	gfx_qe_entity (&l->gfx_entries[0], theme_gfx[THEME_APP_ENTRY],
					0, 0, 0, WIDGET_DISABLED_COLOR);

	l = &w->layers[2];
	l->flags = WF_FOCUSED | WF_FLAGS_AND | WF_ENABLED;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], theme_gfx[THEME_APP_ENTRY_FOCUS],
					0, 0, 0, COL_DEFAULT);

	line1 = NULL;
	line2 = NULL;

	if (entry->meta) {
		line1 = entry->meta->name;
		line2 = entry->meta->short_description;
	}

	if (!line1)
		line1 = entry->dirname;

	if (!line2)
		line2 = app_entry_default_description;

	l1 = font_get_char_count(FONT_APPNAME, line1,
							theme_gfx[THEME_APP_ENTRY]->w -
							APP_ENTRY_TEXT1_X - 16);
	l2 = font_get_char_count(FONT_APPDESC, line2,
							theme_gfx[THEME_APP_ENTRY]->w -
							APP_ENTRY_TEXT2_X - 16);

	l = &w->layers[3];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = l1 + l2;

	if (entry->icon)
		l->gfx_entry_count++;

	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	font_plot_string (&l->gfx_entries[0], l1, FONT_APPNAME, line1,
						APP_ENTRY_TEXT1_X, APP_ENTRY_TEXT1_Y, 1,
						theme_gfx[THEME_APP_ENTRY]->w - APP_ENTRY_TEXT1_X -
						16, FA_LEFT, FA_ASCENDER);
	font_plot_string (&l->gfx_entries[l1], l2, FONT_APPDESC, line2,
						APP_ENTRY_TEXT2_X, APP_ENTRY_TEXT2_Y, 1,
						theme_gfx[THEME_APP_ENTRY]->w - APP_ENTRY_TEXT2_X -
						16, FA_LEFT, FA_DESCENDER);

	if (entry->icon)
		gfx_qe_entity (&l->gfx_entries[l1 + l2], entry->icon,
						APP_ENTRY_ICON_X, APP_ENTRY_ICON_Y, 1, COL_DEFAULT);
	widget_new_badge(w, entry, &badge_glow_list);
}

void widget_progress (widget *w, s16 x, s16 y, s16 z, const char *caption,
					  const char *note, u32 max) {
	widget_layer *l;

	widget_init (w, WT_PROGRESS, false, x, y, z, 0, 0, 3);

	l = &w->layers[0];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	l = &w->layers[1];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	gfx_qe_entity (&l->gfx_entries[0], theme_gfx[THEME_PROGRESS],
					0, 0, 1, COL_DEFAULT);

	int chars = font_get_char_count(FONT_LABEL, caption,
									theme_gfx[THEME_PROGRESS]->w - 64);
	// The note sits lower left, clear of the percentage on the right.
	u16 note_w = theme_gfx[THEME_PROGRESS]->w - 64 - 64;
	int note_chars = note ? font_get_char_count(FONT_LABEL, note, note_w) : 0;

	l = &w->layers[2];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = 4 + chars + note_chars;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));
	font_plot_string (&l->gfx_entries[4], chars, FONT_LABEL, caption, 38, 19, 2,
						theme_gfx[THEME_PROGRESS]->w - 64, FA_LEFT, FA_EM_CENTERED);
	if (note_chars)
		font_plot_string (&l->gfx_entries[4 + chars], note_chars, FONT_LABEL, note,
							38, theme_gfx[THEME_PROGRESS]->h - 20, 2, note_w,
							FA_LEFT, FA_EM_CENTERED);

	w->progress.max = max;
	w->progress.gradient = (gfx_entity *) pmalloc (sizeof (gfx_entity));
}

void widget_gradient (widget *w, s16 x, s16 y, s16 z,
						u16 width, u16 height,
						u32 c1, u32 c2, u32 c3,
						u32 c4) {
	widget_layer *l;

	widget_init (w, WT_GRADIENT, false, x, y, z, 0, 0, 1);

	w->gradient.gradient = (gfx_entity *) pmalloc (sizeof (gfx_entity));
	gfx_gen_gradient (w->gradient.gradient, width, height, c1, c2, c3, c4);

	l = &w->layers[0];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	gfx_qe_entity (&l->gfx_entries[0], w->gradient.gradient, 0, 0, 0,
					COL_DEFAULT);
}

void widget_memo (widget *w, s16 x, s16 y, s16 z, u16 width, u16 height,
				  const char *text, font_xalign align) {
	widget_layer *l;
	int hf;
	int count, i, c;
	char **lines;
	int cl, ot;
	int oy;

	hf = font_get_y_spacing(FONT_MEMO);

	count = font_wrap_string (&lines, FONT_MEMO, text, width);

	c = 0;
	for (i = 0; i < count; ++i)
		if (lines[i])
			c++;

	ot = 0;
	cl = (height + hf - 1) / hf;
	if (cl > count)
		ot = ((cl - count) / 2) * hf;

	widget_init (w, WT_MEMO, false, x, y + ot, z, width, height, c + 2);

	if (c > cl)
		w->cur = CUR_DRAG;

	l = &w->layers[0];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	gfx_qe_scissor (&l->gfx_entries[0], x, y, z, width, height);

	c = 1;
	oy = 0;

	w->memo.y_max = w->coords.y;

	for (i = 0; i < count; ++i) {
		if (!lines[i]) {
			oy += hf;
			continue;
		}

		l = &w->layers[c];
		l->flags = 0;
		l->flags_invert = 0;
		l->gfx_entry_count = font_get_char_count(FONT_MEMO, lines[i], 0);
		l->gfx_entries = (gfx_queue_entry *) pmalloc (l->gfx_entry_count *
							sizeof (gfx_queue_entry));
		font_plot_string (&l->gfx_entries[0], l->gfx_entry_count, FONT_MEMO,
							lines[i], 0, oy, 0, width, align, FA_ASCENDER);
		c++;
		oy += hf;
	}

	font_free_lines (lines, count);

	l = &w->layers[c];
	l->flags = 0;
	l->flags_invert = 0;
	l->gfx_entry_count = 1;
	l->gfx_entries = (gfx_queue_entry *)
						pmalloc (l->gfx_entry_count * sizeof (gfx_queue_entry));

	gfx_qe_scissor_reset (&l->gfx_entries[0]);

	w->memo.y_min = w->memo.y_max - oy + height;
}

void widget_memo_deco (widget *w, s16 x, s16 y, s16 z, u16 width, u16 height,
					   const char *text, font_xalign align) {
	widget_image (&w[0], x + (width - theme_gfx[THEME_CONTENT_ARROW_UP]->w) / 2,
					y, z, theme_gfx[THEME_CONTENT_ARROW_UP], NULL, false, NULL);
	widget_memo (&w[1], x, y + theme_gfx[THEME_CONTENT_ARROW_UP]->h + 8, z,
					width, height - theme_gfx[THEME_CONTENT_ARROW_UP]->h -
					8 - theme_gfx[THEME_CONTENT_ARROW_DOWN]->h - 8, text,
					align);
	widget_image (&w[2],
					x + (width - theme_gfx[THEME_CONTENT_ARROW_DOWN]->w) / 2,
					y + height - theme_gfx[THEME_CONTENT_ARROW_DOWN]->h, z,
					theme_gfx[THEME_CONTENT_ARROW_DOWN], NULL, false, NULL);
	widget_scroll_memo_deco (w, 0);
}

