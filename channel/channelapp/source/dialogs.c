#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>

#include "../config.h"
#include "controls.h"
#include "theme.h"
#include "font.h"
#include "widgets.h"
#include "view.h"
#include "xml.h"
#include "panic.h"

#include "dialogs.h"
#include "loader.h"

#include "i18n.h"

#define TRANS_STEPS 15

static const char *app_entry_desc_default;

static const char *caption_info;
static const char *caption_confirm;
static const char *caption_warning;
static const char *caption_error;
static const char *caption_ok;
static const char *caption_cancel;
static const char *caption_yes;
static const char *caption_no;
static const char *caption_delete;
static const char *caption_load;
static const char *caption_back;
static const char *caption_options;
static const char *caption_device;
static const char *caption_device_names[DEVICE_COUNT];
static const char *caption_sort_by;
static const char *caption_sort_name;
static const char *caption_sort_date;
static const char *caption_sort_custom;
static const char *caption_progress;
static const char *caption_edges;
static const char *caption_edge_wrap;
static const char *caption_edge_page;
static const char *caption_progress_percent;
static const char *caption_progress_circle;

static const char *l_version;
static const char *l_coder;

static const char *string_about_pre;
static const char *string_about_post;
static const char *string_about_trans;
static const char *string_about_theme;
static char *string_about_gen;

void dialogs_theme_reinit (void) {
	app_entry_desc_default = _("no description available");

	caption_info = _("Information");
	caption_confirm = _("Confirmation");
	caption_warning = _("Warning");
	caption_error = _("Error");
	caption_ok = _("Ok");
	caption_cancel = _("Cancel");
	caption_yes = _("Yes");
	caption_no = _("No");
	caption_delete = _("Delete");
	caption_load = _("Load");
	caption_back = _("Back");
	caption_options = _("Options");
	caption_device = _("Device:");
	// Short enough for the theme's tiny buttons, three to a row.
	caption_device_names[0] = _("SD card");
	caption_device_names[1] = _("USB 1");
	caption_device_names[2] = _("SDGecko A");
	caption_device_names[3] = _("SDGecko B");
	caption_device_names[4] = _("USB 2");
	caption_sort_by = _("Sort applications by:");
	caption_sort_name = _("Name");
	caption_sort_date = _("Date");
	caption_sort_custom = _("Custom");
	// The transfer line's percentage or circle; beside its two tiny buttons.
	caption_progress = _("Progress:");
	caption_edges = _("Grid edges:");
	caption_edge_wrap = _("Wrap rows");
	caption_edge_page = _("Turn page");
	caption_progress_percent = _("Percent");
	caption_progress_circle = _("Circle");

	string_about_pre =
		"Credits\n\n"

		"The Homebrew Channel was made possible by the following people:\n\n"

		"dhewg (EWRONGCHAN) - application code, geckoloader code\n"
		"blasty (ESTONED) - application code\n"
		"marcan (EFAILURE) - reload stub, banner, installer, packaging\n"
		"bushing (EWANTPONY) - socket code, loader code\n"
		"segher (EBUGFOUND) - nandloader stub code\n"
		"souLLy (ENOTHERE) - banner graphics\n"
		"drmr (EMORECOWBELL) - banner audio, channel graphics\n"
		"mha (E404) - update server and hosting";

	string_about_post =
		"Powered by devkitPPC and libogc, by shagkur, WinterMute, "
		"and everyone else who contributed\n\n"

		"Thanks to all the beta testers\n\n"

		"Kind regards to the following people too:\n\n"

		"sepp256 - dropped some good GX hints\n"
		"chishm, svpe, rodries, hermes - libfat port\n"
		"alien - some graphics\n"
		"jodi - the lulz\n\n"

		"And last but not least, thanks to the authors of the following libraries:\n\n"

		"wiiuse - para's Wiimote library, now integrated with libogc\n"
		"libpng - the official PNG library\n"
		"Mini-XML - small and efficient XML parsing library\n"
		"FreeType - the free TrueType/OpenType font renderer\n";

	string_about_trans = _("<YourLanguageHere> translation by <YourNickNameHere>");

	if (!i18n_have_mo())
		string_about_trans = "";

	string_about_theme = _("Theme:");

	if (string_about_gen)
		free(string_about_gen);

	string_about_gen = pmalloc(strlen(string_about_pre) +
								strlen(string_about_post) +
								strlen(string_about_trans) +
								strlen(string_about_theme) + 128);

	l_version = _("Version: %s");
	l_coder = _("Author: %s");
}

void dialogs_init (void) {
	string_about_gen = NULL;
	dialogs_theme_reinit();
}

void dialogs_deinit (void) {
	free(string_about_gen);
}

void dialog_fade (view *v, bool fade_in) {
	float val;
	float step;
	s16 y;
	float yf;
	u32 c;
	u8 stepa;
	u8 i;

	if (fade_in) {
		val = 0;
		step = M_PI / (2 * TRANS_STEPS);
		y = v->coords.y + view_height;
		c = DIALOG_MASK_COLOR & 0xffffff00;
		stepa = (DIALOG_MASK_COLOR & 0xff) / TRANS_STEPS;
	} else {
		val = M_PI;
		step = M_PI / (2 * TRANS_STEPS);
		y = v->coords.y;
		c = DIALOG_MASK_COLOR;
		stepa = -(DIALOG_MASK_COLOR & 0xff) / TRANS_STEPS;
	}

	yf = view_height;

	for (i = 0; i < TRANS_STEPS; ++i) {
		v->coords.y = y - roundf (yf * sinf (val));
		val += step;
		c += stepa;

		view_plot (v, c, NULL, NULL, NULL);
	}
}

view * dialog_app (const app_entry *entry, const view *sub_view) {
	view *v;
	u16 x, gap;
	char *name;
	char coder[64];
	char version[64];
	const char *desc;
	u16 ym, hm, yb;

	if (entry->meta && entry->meta->name)
		name = entry->meta->name;
	else
		name = entry->dirname;

	if (entry->meta && entry->meta->coder)
		snprintf (coder, sizeof (coder), l_coder, entry->meta->coder);
	else
		*coder = 0;

	if (entry->meta && entry->meta->version)
		snprintf (version, sizeof (version), l_version, entry->meta->version);
	else
		*version = 0;

	if (entry->meta && entry->meta->long_description)
		desc = entry->meta->long_description;
	else
		desc = app_entry_desc_default;

	v = view_new (11, sub_view, (view_width - theme_gfx[THEME_DIALOG]->w) / 2,
					44, TEX_LAYER_DIALOGS, PADS_B);

	widget_image(&v->widgets[0], 0, 0, 0, theme_gfx[THEME_DIALOG],
					NULL, false, NULL);

	widget_label (&v->widgets[1], 32, 16, 1, name,
				  theme_gfx[THEME_DIALOG]->w - 64, FA_CENTERED, FA_ASCENDER,
				  FONT_DLGTITLE);

	if (entry->icon)
		widget_image(&v->widgets[2], 32, 48, 1, entry->icon, NULL, false, NULL);

	widget_label (&v->widgets[3], 48 + APP_ENTRY_ICON_WIDTH, 72, 1, version,
					theme_gfx[THEME_DIALOG]->w - 72 - APP_ENTRY_ICON_WIDTH,
					FA_LEFT, FA_DESCENDER, FONT_LABEL);

	widget_label (&v->widgets[4], 48 + APP_ENTRY_ICON_WIDTH, 72, 1, coder,
					theme_gfx[THEME_DIALOG]->w - 72 - APP_ENTRY_ICON_WIDTH,
					FA_LEFT, FA_ASCENDER, FONT_LABEL);

	yb = theme_gfx[THEME_DIALOG]->h - theme_gfx[THEME_BUTTON_TINY]->h - 16;
	ym = 48 + APP_ENTRY_ICON_HEIGHT + 8;
	hm = yb - ym - 8;

	widget_memo_deco (&v->widgets[5], 32, ym, 1,
						theme_gfx[THEME_DIALOG]->w - 64, hm, desc, FA_LEFT);

	gap = (theme_gfx[THEME_DIALOG]->w -
			theme_gfx[THEME_BUTTON_TINY]->w * 3) / 4;

	x = gap;
	widget_button (&v->widgets[8], x, yb, 1, BTN_TINY, caption_delete);

	x += gap + theme_gfx[THEME_BUTTON_TINY]->w;
	widget_button (&v->widgets[9], x, yb, 1, BTN_TINY, caption_load);

	x += gap + theme_gfx[THEME_BUTTON_TINY]->w;
	widget_button (&v->widgets[10], x, yb, 1, BTN_TINY, caption_back);

	view_set_focus (v, 9);   // Load, the usual choice

	return v;
}

view * dialog_progress (const view *sub_view, const char *caption,
						const char *note, u32 max) {
	view *v;

	v = view_new (1, sub_view, (view_width - theme_gfx[THEME_PROGRESS]->w) / 2,
					(view_height - theme_gfx[THEME_PROGRESS]->h) / 2,
					TEX_LAYER_DIALOGS, 0);

	widget_progress (&v->widgets[0], 0, 0, 0, caption, note, max);
	widget_set_progress (&v->widgets[0], 0);

	return v;
}

void dialog_set_progress (const view *v, u32 progress) {
	widget_set_progress (&v->widgets[0], progress);
}

view * dialog_about (const view *sub_view) {
	view *v;
	u16 ym, hm, yb, hmn;
	u8 l, hf;

	strcpy(string_about_gen, string_about_pre);
	if (string_about_trans && strlen(string_about_trans)) {
		strcat(string_about_gen, "\n\n");
		strcat(string_about_gen, string_about_trans);
	}

	if (theme.description && strlen(theme.description)) {
		strcat(string_about_gen, "\n\n");
		strcat(string_about_gen, string_about_theme);
		strcat(string_about_gen, " ");
		strcat(string_about_gen, theme.description);
	}

	strcat(string_about_gen, "\n\n");
	strcat(string_about_gen, string_about_post);

	v = view_new (3, sub_view, (view_width - theme_gfx[THEME_DIALOG]->w) / 2,
					44, TEX_LAYER_DIALOGS, 0);

	widget_image (&v->widgets[0], 0, 0, 0, theme_gfx[THEME_DIALOG],
					NULL, false, NULL);

	widget_image (&v->widgets[1], (theme_gfx[THEME_DIALOG]->w -
					theme_gfx[THEME_ABOUT]->w) / 2, 16, 1,
					theme_gfx[THEME_ABOUT], NULL, false, NULL);

	yb = theme_gfx[THEME_DIALOG]->h - 16;
	ym = 16 + theme_gfx[THEME_ABOUT]->h + 8;
	hm = yb - ym - 8;
	hf = font_get_y_spacing(FONT_MEMO);
	l = hm / hf;
	hmn = l * hf;

	if (hmn < hm) {
		ym += (hm - hmn) / 2;
		hm = hmn;
	}

	widget_memo (&v->widgets[2], 32, ym, 1, theme_gfx[THEME_DIALOG]->w - 64,
					hm, string_about_gen, FA_CENTERED);

	v->widgets[2].cur = CUR_STD;

	return v;
}

static view *dialog_message(const view *sub_view, dialog_message_type type,
							dialog_message_buttons buttons, const char *text,
							u8 focus) {
	view *v;
	u8 c;
	u16 x, gap;
	const char *caption = NULL, *b1 = NULL, *b2 = NULL;
	u16 ym, hm, yb;
	u8 hf;
	gfx_entity *icon = NULL;

	c = 6;
	switch (buttons) {
	case DLGB_OK:
		c++;
		break;

	case DLGB_OKCANCEL:
	case DLGB_YESNO:
		c += 2;
		break;
	}

	v = view_new (c, sub_view, (view_width - theme_gfx[THEME_DIALOG]->w) / 2,
					44, TEX_LAYER_DIALOGS, PADS_B);

	widget_image (&v->widgets[0], 0, 0, 0, theme_gfx[THEME_DIALOG],
					NULL, false, NULL);

	switch (type) {
	case DLGMT_INFO:
		caption = caption_info;
		icon = theme_gfx[THEME_DLG_INFO];
		break;

	case DLGMT_CONFIRM:
		caption = caption_confirm;
		icon = theme_gfx[THEME_DLG_CONFIRM];
		break;

	case DLGMT_WARNING:
		caption = caption_warning;
		icon = theme_gfx[THEME_DLG_WARNING];
		break;

	case DLGMT_ERROR:
		caption = caption_error;
		icon = theme_gfx[THEME_DLG_ERROR];
		break;
	}

	widget_image (&v->widgets[1], 128, 24, 0, icon, NULL, false, NULL);
	widget_label (&v->widgets[2], 32, 32, 1, caption,
				  theme_gfx[THEME_DIALOG]->w - 64, FA_CENTERED, FA_ASCENDER,
				  FONT_DLGTITLE);

	hf = font_get_height (FONT_DLGTITLE);
	yb = theme_gfx[THEME_DIALOG]->h - theme_gfx[THEME_BUTTON_SMALL]->h - 32;
	ym = 32 + hf + 8;
	hm = yb - ym - 8;

	widget_memo_deco (&v->widgets[3], 32, ym, 1,
						theme_gfx[THEME_DIALOG]->w - 64, hm, text, FA_CENTERED);

	switch (buttons) {
	case DLGB_OK:
		b1 = caption_ok;
		b2 = NULL;
		break;

	case DLGB_OKCANCEL:
		b1 = caption_ok;
		b2 = caption_cancel;
		break;

	case DLGB_YESNO:
		b1 = caption_yes;
		b2 = caption_no;
		break;
	}

	if (b2) {
		gap = (theme_gfx[THEME_DIALOG]->w -
				theme_gfx[THEME_BUTTON_SMALL]->w * 2) / 3;

		x = gap;
		widget_button (&v->widgets[6], x, yb, 1, BTN_SMALL, b1);

		x += gap + theme_gfx[THEME_BUTTON_SMALL]->w;
		widget_button (&v->widgets[7], x, yb, 1, BTN_SMALL, b2);
	} else {
		gap = (theme_gfx[THEME_DIALOG]->w -
				theme_gfx[THEME_BUTTON_SMALL]->w) / 2;

		x = gap;
		widget_button (&v->widgets[6], x, yb, 1, BTN_SMALL, b1);
	}

	view_set_focus (v, 6 + focus);

	return v;
}

s8 show_message (const view *sub_view, dialog_message_type type,
					dialog_message_buttons buttons, const char *text, u8 focus) {
	return show_message_timed (sub_view, type, buttons, text, focus, 0);
}

s8 show_message_timed (const view *sub_view, dialog_message_type type,
						dialog_message_buttons buttons, const char *text,
						u8 focus, u32 timeout_s) {
	view *v;
	u8 fhw;
	u32 bd, bu;
	s8 res;
	s16 mm;
	u64 start = gettime ();
	u32 shown = 0, gone;
	const char *last;
	char caption[48];

	v = dialog_message (sub_view, type, buttons, text, focus);
	last = buttons == DLGB_OK ? caption_ok :
			buttons == DLGB_OKCANCEL ? caption_cancel : caption_no;

	dialog_fade (v, true);

	fhw = font_get_y_spacing(FONT_MEMO);

	while (true) {
		// A countdown on the last button, which it picks when it runs out;
		// any button press stops it, for someone reading the message.
		if (timeout_s) {
			gone = ticks_to_secs (diff_ticks (start, gettime ()));
			if (gone >= timeout_s) {
				view_set_focus (v, v->widget_count - 1);
				break;
			}
			if (timeout_s - gone != shown) {
				shown = timeout_s - gone;
				snprintf (caption, sizeof (caption), "%s (%u)", last, shown);
				widget_button_set_caption (&v->widgets[v->widget_count - 1],
											FONT_BUTTON, caption);
			}
		}

		view_plot (v, DIALOG_MASK_COLOR, &bd, NULL, &bu);
		// Keep answering the network meanwhile: hbc.py reads an upload's
		// result while its popup is up.
		loader_signal_threads ();

		if (timeout_s && bd) {
			timeout_s = 0;
			widget_button_set_caption (&v->widgets[v->widget_count - 1],
										FONT_BUTTON, last);
		}

		if (bd & PADS_LEFT)
			view_set_focus_prev (v);

		if (bd & PADS_RIGHT)
			view_set_focus_next (v);

		mm = 0;
		if (bd & PADS_UP)
			mm += fhw;

		if (bd & PADS_DOWN)
			mm -= fhw;

		mm += controls_sticky() / 8;

		if (v->drag && (v->drag_widget == 4))
			mm += -v->drag_y / 32;

		widget_scroll_memo_deco (&v->widgets[3], mm);

		// B backs out: the last button (OK, Cancel or No).
		if (view_back (v, bu)) {
			view_set_focus (v, v->widget_count - 1);
			break;
		}

		if ((bd & PADS_A) && (v->focus != -1))
			break;
	}

	res = v->focus - 6;

	dialog_fade (v, false);

	view_free (v);

	return res;
}

// The device buttons: three on the first row (SD and the two USB drives),
// the two SDGecko slots on the second, in the theme's tiny buttons. Widget
// DLG_DEV_FIRST + n is the n-th button here; dlg_devices[n] is its device
// (appentry.c's order, which settings.xml stores).
#define DLG_DEV_FIRST 6
// Below them, one row per setting as in the HOME overlay: its label on the
// left, a small button for each value on the right, the value in use bright.
// The sort buttons, DLG_SORT_FIRST + an app_sort.
#define DLG_SORT_FIRST (DLG_DEV_FIRST + DEVICE_COUNT)
#define DLG_SORTS 3
// Transfer progress, DLG_PROG_FIRST + settings.progress_arc.
#define DLG_PROG_FIRST (DLG_SORT_FIRST + DLG_SORTS)
#define DLG_PROGS 2
// At the grid's edge, DLG_EDGE_FIRST + settings.edge_paging.
#define DLG_EDGE_FIRST (DLG_PROG_FIRST + DLG_PROGS)
#define DLG_EDGES 2
#define DLG_OK (DLG_EDGE_FIRST + DLG_EDGES)
// The row buttons are tiny buttons drawn at this size, this far apart.
#define DLG_SMALL_SCALE 0.7f
#define DLG_SMALL_GAP 6
#define DLG_BACK (DLG_OK + 1)
#define DLG_WIDGETS (DLG_BACK + 1)

static const int dlg_devices[DEVICE_COUNT] = { 0, 1, 4, 2, 3 };

// Rows of widgets for up and down, -1 where a row has fewer.
static const int dlg_rows[][3] = {
	{ DLG_DEV_FIRST, DLG_DEV_FIRST + 1, DLG_DEV_FIRST + 2 },
	{ DLG_DEV_FIRST + 3, DLG_DEV_FIRST + 4, -1 },
	{ DLG_SORT_FIRST, DLG_SORT_FIRST + 1, DLG_SORT_FIRST + 2 },
	{ DLG_PROG_FIRST, DLG_PROG_FIRST + 1, -1 },
	{ DLG_EDGE_FIRST, DLG_EDGE_FIRST + 1, -1 },
	{ DLG_OK, DLG_BACK, -1 },
};
#define DLG_ROWS (int) (sizeof(dlg_rows) / sizeof(dlg_rows[0]))

static int dlg_middle(const view *v, int w) {
	return v->widgets[w].coords.x + v->widgets[w].width / 2;
}

// Up or down: the enabled button in the next row nearest the focused one
// across; a row with none enabled (no device) is skipped.
static void dlg_move_row(view *v, int dir) {
	int r, c, x, d, best = 0, to = -1;

	for (r = 0; r < DLG_ROWS; ++r)
		for (c = 0; c < 3; ++c)
			if (dlg_rows[r][c] == v->focus)
				goto found;
	view_set_focus (v, DLG_BACK);
	return;
found:
	x = dlg_middle(v, v->focus);
	for (r += dir; r >= 0 && r < DLG_ROWS && to < 0; r += dir)
		for (c = 0; c < 3; ++c) {
			if (dlg_rows[r][c] < 0 || !(v->widgets[dlg_rows[r][c]].flags & WF_ENABLED))
				continue;
			d = abs(dlg_middle(v, dlg_rows[r][c]) - x);
			if (to < 0 || d < best) {
				to = dlg_rows[r][c];
				best = d;
			}
		}
	if (to >= 0)
		view_set_focus (v, to);
}

static int dlg_slot(int device) {
	int i;

	for (i = 0; i < DEVICE_COUNT; ++i)
		if (dlg_devices[i] == device)
			return i;
	return -1;
}

// A choice's buttons, first + n, with the value in use bright.
static void dlg_choice_captions(view *v, int first, int count, int value,
								const char **captions[]) {
	int i;

	for (i = 0; i < count; ++i)
		widget_button_set_caption(&v->widgets[first + i],
									i == value ? FONT_BUTTON : FONT_BUTTON_DESEL, *captions[i]);
}

static void dlg_sort_captions(view *v, app_sort sort) {
	static const char **captions[DLG_SORTS] = {
		&caption_sort_name, &caption_sort_date, &caption_sort_custom
	};

	dlg_choice_captions(v, DLG_SORT_FIRST, DLG_SORTS, (int) sort, captions);
}

// What Options shows, for HBCS: a test on the bench Wii checks the focus
// before it presses A (`hbc.py buttons`).
static volatile bool options_open;
static volatile int options_focus = -1, options_arc, options_paging;

static const char *dlg_focus_name(int focus) {
	if (focus >= DLG_DEV_FIRST && focus < DLG_DEV_FIRST + DEVICE_COUNT)
		return "device";
	if (focus >= DLG_SORT_FIRST && focus < DLG_SORT_FIRST + DLG_SORTS)
		return "sort";
	if (focus == DLG_PROG_FIRST)
		return "percent";
	if (focus == DLG_PROG_FIRST + 1)
		return "circle";
	if (focus == DLG_EDGE_FIRST)
		return "wrap";
	if (focus == DLG_EDGE_FIRST + 1)
		return "page";
	if (focus == DLG_OK)
		return "ok";
	if (focus == DLG_BACK)
		return "back";
	return "none";
}

int dialogs_options_json(char *buf, int size) {
	bool open = options_open;

	return snprintf(buf, size, ",\"options\":{\"open\":%s,\"focus\":\"%s\",\"choice\":\"%s\","
					"\"edge_choice\":\"%s\",\"progress\":\"%s\",\"edges\":\"%s\"}",
					open ? "true" : "false", dlg_focus_name(open ? options_focus : -1),
					open ? (options_arc ? "circle" : "percent") : "none",
					open ? (options_paging ? "page" : "wrap") : "none",
					settings.progress_arc ? "circle" : "percent",
					settings.edge_paging ? "page" : "wrap");
}

static void dlg_progress_captions(view *v, int arc) {
	static const char **captions[DLG_PROGS] = {
		&caption_progress_percent, &caption_progress_circle
	};

	dlg_choice_captions(v, DLG_PROG_FIRST, DLG_PROGS, arc, captions);
}

static void dlg_edge_captions(view *v, int paging) {
	static const char **captions[DLG_EDGES] = { &caption_edge_wrap, &caption_edge_page };

	dlg_choice_captions(v, DLG_EDGE_FIRST, DLG_EDGES, paging, captions);
}

dialog_options_result show_options_dialog(const view *sub_view) {
	u32 frame = 0;
	view *v;
	dialog_options_result ret;
	int device, slot;
	app_sort sort;
	bool status[DEVICE_COUNT];
	u32 i, bd;
	u16 dw = theme_gfx[THEME_DIALOG]->w, dh = theme_gfx[THEME_DIALOG]->h;
	u16 tw = theme_gfx[THEME_BUTTON_TINY]->w, th = theme_gfx[THEME_BUTTON_TINY]->h;
	u16 sw = theme_gfx[THEME_BUTTON_SMALL]->w, sh = theme_gfx[THEME_BUTTON_SMALL]->h;
	u16 gap = (dw - 3 * tw) / 4, row2 = 64 + th + 8, bottom_y = dh - sh - 6;
	// The setting rows: buttons right-aligned to the device rows' edge, each
	// label in what is left of its row.
	u16 stw = tw * DLG_SMALL_SCALE, sth = th * DLG_SMALL_SCALE;
	u16 step = sth + DLG_SMALL_GAP, sort_y = row2 + th + 14, right = dw - gap;
	u16 label_w = right - 3 * stw - 2 * DLG_SMALL_GAP - 32 - 8;
	static const int row_first[3] = { DLG_SORT_FIRST, DLG_PROG_FIRST, DLG_EDGE_FIRST };
	static const int row_count[3] = { DLG_SORTS, DLG_PROGS, DLG_EDGES };
	const char *row_label[3] = { caption_sort_by, caption_progress, caption_edges };
	int r;

	app_entry_poll_status(true);

	v = view_new (DLG_WIDGETS, sub_view, (view_width - dw) / 2,
					44, TEX_LAYER_DIALOGS, PADS_B);

	widget_image (&v->widgets[0], 0, 0, 0, theme_gfx[THEME_DIALOG],
					NULL, false, NULL);
	widget_label (&v->widgets[1], 32, 16, 1, caption_options,
					dw - 64, FA_CENTERED, FA_ASCENDER, FONT_DLGTITLE);

	widget_label (&v->widgets[2], 32, 60, 1, caption_device,
					dw - 64, FA_LEFT, FA_DESCENDER, FONT_LABEL);

	for (i = 0; i < 3; ++i)
		widget_button (&v->widgets[DLG_DEV_FIRST + i], gap + i * (gap + tw), 64, 1,
						BTN_TINY, NULL);
	for (i = 0; i < 2; ++i)
		widget_button (&v->widgets[DLG_DEV_FIRST + 3 + i],
						(dw - 2 * tw - gap) / 2 + i * (gap + tw), row2, 1, BTN_TINY, NULL);

	for (r = 0; r < 3; ++r) {
		u16 y = sort_y + r * step;

		widget_label (&v->widgets[3 + r], 32, y + sth / 2, 1, row_label[r], label_w,
						FA_LEFT, FA_EM_CENTERED, FONT_LABEL);
		// Two values take the wider small buttons: the row is as wide as three.
		bool two = row_count[r] == 2;
		u16 bw = two ? sw * DLG_SMALL_SCALE : stw;

		for (i = 0; i < (u32) row_count[r]; ++i) {
			widget_button (&v->widgets[row_first[r] + i],
							right - (row_count[r] - i) * (bw + DLG_SMALL_GAP) + DLG_SMALL_GAP,
							y, 1, two ? BTN_SMALL : BTN_TINY, NULL);
			widget_button_scale (&v->widgets[row_first[r] + i], DLG_SMALL_SCALE);
		}
	}

	widget_button (&v->widgets[DLG_OK], 32, bottom_y, 1, BTN_SMALL, caption_ok);
	widget_button (&v->widgets[DLG_BACK], dw - sw - 32, bottom_y, 1, BTN_SMALL,
					caption_back);

	device = app_entry_get_status(status);
	sort = app_entry_get_sort();

	ret.confirmed = false;
	ret.device = device;
	ret.sort = sort;
	ret.progress_arc = settings.progress_arc;
	ret.edge_paging = settings.edge_paging;

	for (i = 0; i < DEVICE_COUNT; ++i) {
		widget_button_set_caption(&v->widgets[DLG_DEV_FIRST + i],
									dlg_devices[i] == device ? FONT_BUTTON : FONT_BUTTON_DESEL,
									caption_device_names[dlg_devices[i]]);
		widget_set_flag (&v->widgets[DLG_DEV_FIRST + i], WF_ENABLED, status[dlg_devices[i]]);
	}

	dlg_sort_captions(v, ret.sort);
	dlg_progress_captions(v, ret.progress_arc);
	dlg_edge_captions(v, ret.edge_paging);

	view_set_focus (v, DLG_BACK);

	dialog_fade (v, true);

	while (true) {
		app_entry_get_status(status);

		for (i = 0; i < DEVICE_COUNT; ++i)
			widget_set_flag (&v->widgets[DLG_DEV_FIRST + i], WF_ENABLED,
								status[dlg_devices[i]]);

		view_plot (v, DIALOG_MASK_COLOR, &bd, NULL, NULL);
		frame++;
		// hbc.py keeps getting answers while Options is open.
		loader_signal_threads ();

		if (bd & PADS_LEFT)
			view_set_focus_prev (v);

		if (bd & PADS_RIGHT)
			view_set_focus_next (v);

		if (bd & PADS_UP)
			dlg_move_row(v, -1);

		if (bd & PADS_DOWN)
			dlg_move_row(v, 1);

		if (bd & (PADS_B | PADS_1))
			break;

		if ((bd & PADS_A) && (v->focus != -1)) {
			if ((v->focus >= DLG_DEV_FIRST) &&
					(v->focus < DLG_DEV_FIRST + DEVICE_COUNT)) {
				slot = dlg_slot(ret.device);
				if (slot >= 0)
					widget_button_set_caption(&v->widgets[DLG_DEV_FIRST + slot],
												FONT_BUTTON_DESEL,
												caption_device_names[ret.device]);
				ret.device = dlg_devices[v->focus - DLG_DEV_FIRST];
				widget_button_set_caption(&v->widgets[v->focus], FONT_BUTTON,
											caption_device_names[ret.device]);
			} else if ((v->focus >= DLG_SORT_FIRST) &&
					(v->focus < DLG_SORT_FIRST + DLG_SORTS)) {
				ret.sort = (app_sort) (v->focus - DLG_SORT_FIRST);
				dlg_sort_captions(v, ret.sort);
			} else if ((v->focus >= DLG_PROG_FIRST) &&
					(v->focus < DLG_PROG_FIRST + DLG_PROGS)) {
				ret.progress_arc = v->focus - DLG_PROG_FIRST;
				dlg_progress_captions(v, ret.progress_arc);
			} else if ((v->focus >= DLG_EDGE_FIRST) &&
					(v->focus < DLG_EDGE_FIRST + DLG_EDGES)) {
				ret.edge_paging = v->focus - DLG_EDGE_FIRST;
				dlg_edge_captions(v, ret.edge_paging);
			} else if ((v->focus == DLG_OK) || (v->focus == DLG_BACK)) {
				break;
			}
		}

		if ((frame % 30) == 0)
			app_entry_poll_status(false);

		options_focus = v->focus;
		options_arc = ret.progress_arc;
		options_paging = ret.edge_paging;
		options_open = true;
	}
	options_open = false;

	if ((bd & PADS_A) && (v->focus == DLG_OK))
		ret.confirmed = true;

	dialog_fade (v, false);

	view_free (v);

	return ret;
}


