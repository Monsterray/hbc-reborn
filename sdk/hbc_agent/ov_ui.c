// The overlay's layout, animation and input; see ov_ui.h.
//
// A status strip sits at the bottom: the app and clock, the four remotes'
// player LEDs and batteries, and five buttons (DEV, an app slot, Exit, an
// app slot that defaults to Shot, WiiMote). Exit grows the strip upward;
// DEV and WiiMote slide their menus in from the screen edge above their
// buttons. B goes back one level; HOME closes everything, each level
// animating away in turn.

#include <stdio.h>
#include <string.h>

#include "ov_ui.h"

enum { MENU_NONE, MENU_DEV, MENU_EXIT, MENU_WM, MENU_SLOT0, MENU_SLOT1 };
enum { PAGE_MAIN, PAGE_LOG, PAGE_INFO };

static const char *info_names[OV_INFO_PAGES] = { "System", "Video", "Storage", "USB",
												 "Network" };
enum { WM_GRID, WM_MORE, WM_TEST, WM_CAL, WM_SETTINGS };

enum {
	K_PANEL, K_BUTTON, K_TEXT, K_REMOTE, K_CARD, K_BAR, K_RULE, K_CLIP, K_NOCLIP
};

enum {
	F_FOCUS = 1, F_ON = 2, F_DIS = 4, F_LABEL = 8, F_RIGHT = 16, F_CENTER = 32,
	F_BLANK = 64, F_SEL = 128, F_SMALL = 256
};

// Focus ids by layer: bar 100s, exit row 110s, DEV 120s, WiiMote 140s+.
enum {
	ID_BAR = 100,
	ID_EX_HBC = 110, ID_EX_SYS, ID_EX_RESTART, ID_EX_POWER,
	ID_TAB_ACT = 120, ID_TAB_INFO, ID_RESTART_APP, ID_PAUSE, ID_SAVE, ID_LOG,
	ID_LOGPC, ID_CRASH_3S, ID_CRASH_STAY, ID_HBCPY, ID_RESET_REMOTES, ID_SYNC_CLOCK,
	ID_INFO_PAGE = 133,   // + OV_INFO_*
	ID_FIND = 140, ID_MORE = 150, ID_SETTINGS = 160,
	ID_RUMBLE = 170, ID_TEST, ID_CAL, ID_DISC, ID_VOL_MINUS, ID_VOL_PLUS, ID_SND_ADPCM, ID_SND_PCM, ID_SND_WAV,
	ID_CONNECT = 180, ID_DISC_ALL, ID_BAR_BELOW, ID_BAR_ABOVE, ID_IR_MINUS, ID_IR_PLUS,
	ID_OFF_MINUS, ID_OFF_PLUS, ID_RUMBLE_ALL,
	ID_SLOT_ITEM = 200    // + slot * 16 + item
};

#define M 20           // side margin
#define MB 22          // bottom margin
#define PAD 10
#define BH 30          // button height
#define GAP 6
#define STATUS_H 18
#define SH (PAD + STATUS_H + 6 + BH + PAD)
#define EH (BH + 10)
#define DW 400
#define IW (DW - 2 * PAD)
#define CW ((IW - 10) / 2)
#define LOG_LINES 14
#define LOG_COLS 47
#define SPEED 26       // animation step per frame, of 256 (about 0.17 s)

static int ease(int t) {
	return t * t * (3 * 256 - 2 * t) / (256 * 256);
}

static int approach(int t, bool up) {
	t += up ? SPEED : -SPEED;
	return t < 0 ? 0 : t > 256 ? 256 : t;
}

static ov_item *add(ov_ui *ui, int kind, int id, int x, int y, int w, int h, int flags,
					const char *text) {
	ov_item *it;

	if (ui->n >= OV_MAX_ITEMS)
		return &ui->items[OV_MAX_ITEMS - 1];
	it = &ui->items[ui->n++];
	memset(it, 0, sizeof(*it));
	it->kind = kind;
	it->id = id;
	it->x = x;
	it->y = y;
	it->w = w;
	it->h = h;
	it->flags = flags;
	it->font = ov_th->regular;
	if (text)
		snprintf(it->text, sizeof(it->text), "%s", text);
	return it;
}

static ov_item *button(ov_ui *ui, int id, int x, int y, int w, const char *text, int flags) {
	return add(ui, K_BUTTON, id, x, y, w, BH, F_FOCUS | flags, text);
}

static void label(ov_ui *ui, int x, int y, int w, const char *text) {
	add(ui, K_TEXT, 0, x, y, w, BH, F_LABEL, text);
}

static void value(ov_ui *ui, int x, int y, int w, const char *text, int flags) {
	add(ui, K_TEXT, 0, x, y, w, BH, flags, text);
}

static void title(ov_ui *ui, int x, int y, int w, const char *text) {
	ov_item *it = add(ui, K_TEXT, 0, x, y, w, 22, 0, text);

	it->font = ov_th->title;
}

// "label  [On]": one toggle, highlighted when on.
static void toggle(ov_ui *ui, int id, int y, const char *text, bool on, int flags) {
	label(ui, 0, y, CW, text);
	button(ui, id, CW + 10, y, CW, on ? "On" : "Off", (on ? F_ON : 0) | flags);
}

// "label  [a][b]": a choice between two, the chosen one highlighted.
static void choice(ov_ui *ui, int id_a, int id_b, int y, const char *text, const char *a,
				   const char *b, bool first, int flags) {
	int bw = (CW - GAP) / 2;

	label(ui, 0, y, CW, text);
	button(ui, id_a, CW + 10, y, bw, a, (first ? F_ON : 0) | flags);
	button(ui, id_b, CW + 10 + bw + GAP, y, bw, b, (first ? 0 : F_ON) | flags);
}

// "label  [-] value [+]"
static void stepper(ov_ui *ui, int id_minus, int id_plus, int y, const char *text,
					const char *val, int flags) {
	label(ui, 0, y, CW, text);
	button(ui, id_minus, CW + 10, y, 34, "-", flags);
	value(ui, CW + 10 + 34, y, CW - 68, val, F_CENTER | (flags & F_DIS));
	button(ui, id_plus, CW + 10 + CW - 34, y, 34, "+", flags);
}

static void remote_tag(ov_ui *ui, int x, int y, int r, bool dim) {
	ov_item *it = add(ui, K_REMOTE, 0, x, y, 64, 12, dim ? F_DIS : 0, NULL);

	it->arg = r;
}

// Starts a drawer: returns the index of its panel item. Children are laid
// out from y = 0 with x relative to the drawer's inside.
static int drawer_begin(ov_ui *ui) {
	return (int) (add(ui, K_PANEL, 0, 0, 0, DW, 0, 0, NULL) - ui->items);
}

// Places the drawer's panel and children: x of its left edge, bottom edge.
static void drawer_end(ov_ui *ui, int first, int h, int x, int bottom) {
	int top = bottom - h - 2 * PAD, i;

	ui->items[first].x = x;
	ui->items[first].y = top;
	ui->items[first].h = h + 2 * PAD;
	for (i = first + 1; i < ui->n; ++i) {
		ui->items[i].x += x + PAD;
		ui->items[i].y += top + PAD;
	}
}

static void tabs(ov_ui *ui, int y) {
	button(ui, ID_TAB_ACT, 0, y, CW, "Actions", ui->dev_tab == 0 ? F_SEL : 0);
	button(ui, ID_TAB_INFO, CW + 10, y, CW, "Info", ui->dev_tab == 1 ? F_SEL : 0);
}

static int build_dev(ov_ui *ui, const ov_ext *e) {
	int y = 0;

	if (ui->dev_page == PAGE_LOG) {
		const char *p = e->log_text ? e->log_text : "";
		struct { const char *s; int n; } seg[LOG_LINES];
		int count = 0, i;

		title(ui, 0, y, IW, "App output");
		y += 28;
		// Word-wrap the whole text, keeping the newest LOG_LINES pieces.
		while (*p) {
			const char *nl = strchr(p, '\n');
			int len = nl ? (int) (nl - p) : (int) strlen(p);

			do {
				int n = len, w;
				char tmp[LOG_COLS + 1];

				// The longest prefix that fits, broken at a space if possible.
				for (;;) {
					w = n < LOG_COLS ? n : LOG_COLS;
					memcpy(tmp, p, w);
					tmp[w] = 0;
					if (ov_text_width(ov_th->regular, tmp) <= IW || w <= 1)
						break;
					n = w - 1;
				}
				if (w < len) {
					int sp = w;

					while (sp > w / 2 && p[sp] != ' ')
						--sp;
					if (p[sp] == ' ')
						w = sp + 1;
				}
				seg[count % LOG_LINES].s = p;
				seg[count % LOG_LINES].n = w;
				count++;
				p += w;
				len -= w;
			} while (len > 0);
			p = nl ? nl + 1 : p;
		}
		for (i = count > LOG_LINES ? count - LOG_LINES : 0; i < count; ++i) {
			char line[48];
			int n = seg[i % LOG_LINES].n < 47 ? seg[i % LOG_LINES].n : 47;

			memcpy(line, seg[i % LOG_LINES].s, n);
			line[n] = 0;
			add(ui, K_TEXT, 0, 0, y, IW, 16, 0, line);
			y += 16;
		}
		if (!count) {
			value(ui, 0, y, IW, "No output yet.", F_LABEL);
			y += BH;
		}
		return y;
	}

	if (ui->dev_page == PAGE_INFO) {
		const ov_page *pg = e->info ? &e->info[ui->info_page] : NULL;
		int i;

		title(ui, 0, y, IW, info_names[ui->info_page]);
		y += 28;
		if (!pg || !pg->count) {
			value(ui, 0, y, IW, pg ? "Nothing found." : "Checking...", F_LABEL);
			return y + BH;
		}
		for (i = 0; i < pg->count; ++i) {
			add(ui, K_TEXT, 0, 0, y, 120, 18, F_LABEL, pg->row[i].label);
			add(ui, K_TEXT, 0, 120, y, IW - 120, 18, 0, pg->row[i].value);
			y += 19;
		}
		return y;
	}

	tabs(ui, y);
	y += BH + 12;

	if (ui->dev_tab == 0) {
		button(ui, ID_RESTART_APP, 0, y, CW, "Restart app", e->has_restart ? 0 : F_DIS);
		button(ui, ID_PAUSE, CW + 10, y, CW, "Pause", 0);
		y += BH + GAP;
		button(ui, ID_SAVE, 0, y, CW, "Save", e->has_save ? 0 : F_DIS);
		button(ui, ID_LOG, CW + 10, y, CW, "Log", 0);
		y += BH + GAP;
		button(ui, ID_RESET_REMOTES, 0, y, CW, "Reset remotes", 0);
		button(ui, ID_SYNC_CLOCK, CW + 10, y, CW, "Sync clock", 0);
		y += BH + 12;
		if (e->show_log_pc) {
			toggle(ui, ID_LOGPC, y, "Log to PC", e->log_pc, 0);
			y += BH + GAP;
		}
		if (e->show_crash) {
			choice(ui, ID_CRASH_3S, ID_CRASH_STAY, y, "Crash screen", "3 s", "Stay",
				   !e->crash_stay, 0);
			y += BH + GAP;
		}
		if (e->show_net) {
			add(ui, K_TEXT, 0, 0, y - 2, CW, 18, F_LABEL, "hbc.py connection");
			add(ui, K_TEXT, 0, 0, y + 15, CW, 16, F_LABEL | F_SMALL, "developer tools, port 4299");
			button(ui, ID_HBCPY, CW + 10, y, CW, e->hbcpy ? "On" : "Off", e->hbcpy ? F_ON : 0);
			y += BH + GAP;
		}
		return y - GAP + 4;
	}

	{
		static const char *keys[] = { "Time", "Playing for", "Network", "SD card", "App" };
		const char *vals[] = { e->date, e->playing, e->network, e->sd, e->app };
		char f[16], u[16], t[16];
		int i, col = (IW - 80) / 3;

		for (i = 0; i < 5; ++i) {
			add(ui, K_TEXT, 0, 0, y, 110, 20, F_LABEL, keys[i]);
			add(ui, K_TEXT, 0, 110, y, IW - 110, 20, 0, vals[i]);
			y += 22;
		}
		y += 4;
		add(ui, K_RULE, 0, 0, y, IW, 1, 0, NULL);
		y += 8;
		add(ui, K_TEXT, 0, 0, y, 80, 20, F_LABEL, "KB");
		add(ui, K_TEXT, 0, 80, y, col, 20, F_LABEL | F_RIGHT, "Free");
		add(ui, K_TEXT, 0, 80 + col, y, col, 20, F_LABEL | F_RIGHT, "Used");
		add(ui, K_TEXT, 0, 80 + 2 * col, y, col, 20, F_LABEL | F_RIGHT, "Total");
		y += 22;
		for (i = 0; i < 2; ++i) {
			snprintf(f, sizeof(f), "%u", e->mem_free_kb[i]);
			snprintf(u, sizeof(u), "%u", e->mem_total_kb[i] - e->mem_free_kb[i]);
			snprintf(t, sizeof(t), "%u", e->mem_total_kb[i]);
			add(ui, K_TEXT, 0, 0, y, 80, 20, F_LABEL, i ? "MEM2" : "MEM1");
			add(ui, K_TEXT, 0, 80, y, col, 20, F_RIGHT, f);
			add(ui, K_TEXT, 0, 80 + col, y, col, 20, F_RIGHT, u);
			add(ui, K_TEXT, 0, 80 + 2 * col, y, col, 20, F_RIGHT, t);
			y += 22;
		}
		// The pages with more: one row of buttons.
		y += 8;
		{
			int bw = (IW - (OV_INFO_PAGES - 1) * GAP) / OV_INFO_PAGES;

			for (i = 0; i < OV_INFO_PAGES; ++i)
				add(ui, K_BUTTON, ID_INFO_PAGE + i, i * (bw + GAP), y, bw, BH, F_FOCUS | F_SMALL,
					info_names[i]);
		}
		return y + BH;
	}
}

static int build_wm(ov_ui *ui, const ov_ext *e) {
	const ov_remote *sel = &e->remote[ui->wm_sel];
	char buf[48];
	int y = 0, i;

	switch (ui->wm_page) {
	case WM_GRID:
		for (i = 0; i < OV_REMOTES; ++i) {
			const ov_remote *r = &e->remote[i];
			int cx = (i & 1) * (CW + 10), cy = (i >> 1) * 92, bw = (CW - 16 - GAP) / 2;
			int dis = r->connected ? 0 : F_DIS;

			add(ui, K_CARD, 0, cx, cy, CW, 86, dis, NULL);
			remote_tag(ui, cx + 8, cy + 9, i, !r->connected);
			snprintf(buf, sizeof(buf), "%s%s", r->connected ? (r->ext[0] ? r->ext : "Wii Remote")
							: "Not connected", r->connected && r->motionplus ? " + M+" : "");
			add(ui, K_TEXT, 0, cx + 8, cy + 24, CW - 16, 18, F_LABEL | F_SMALL | dis, buf);
			button(ui, ID_FIND + i, cx + 8, cy + 48, bw, "Find", dis | (r->finding ? F_ON : 0));
			button(ui, ID_MORE + i, cx + 8 + bw + GAP, cy + 48, bw, "More", dis);
		}
		y = 2 * 92;
		button(ui, ID_SETTINGS, 0, y, IW, "Settings", 0);
		return y + BH;
	case WM_MORE:
		remote_tag(ui, 0, y + 5, ui->wm_sel, false);
		snprintf(buf, sizeof(buf), "Remote %d", ui->wm_sel + 1);
		title(ui, 70, y, IW - 70, buf);
		y += 30;
		snprintf(buf, sizeof(buf), "%d%%", sel->battery_pct);
		label(ui, 0, y, CW, "Battery");
		value(ui, CW + 10, y, CW, buf, 0);
		y += 24;
		label(ui, 0, y, CW, "Extension");
		value(ui, CW + 10, y, CW, sel->ext[0] ? sel->ext : "None", 0);
		y += 24;
		label(ui, 0, y, CW, "MotionPlus");
		value(ui, CW + 10, y, CW, sel->motionplus ? "Yes" : "No", 0);
		y += 30;
		toggle(ui, ID_RUMBLE, y, "Rumble", sel->rumble, 0);
		y += BH + GAP;
		snprintf(buf, sizeof(buf), "%d of 10", sel->volume);
		stepper(ui, ID_VOL_MINUS, ID_VOL_PLUS, y, "Speaker volume", buf, 0);
		y += BH + 12;
		button(ui, ID_TEST, 0, y, CW, "Test", 0);
		button(ui, ID_CAL, CW + 10, y, CW, "Calibrate", 0);
		y += BH + GAP;
		button(ui, ID_DISC, 0, y, IW, "Disconnect this remote", 0);
		return y + BH;
	case WM_TEST:
		remote_tag(ui, 0, y + 5, ui->wm_sel, false);
		snprintf(buf, sizeof(buf), "Remote %d input test", ui->wm_sel + 1);
		title(ui, 70, y, IW - 70, buf);
		y += 30;
		for (i = 0; i < 6 && e->test[i][0]; ++i) {
			const char *tab = strchr(e->test[i], '\t');
			char key[40];
			int kl = tab ? (int) (tab - e->test[i]) : 0;

			memcpy(key, e->test[i], kl);
			key[kl] = 0;
			label(ui, 0, y, 120, key);
			value(ui, 120, y, IW - 120, tab ? tab + 1 : e->test[i], 0);
			y += 24;
		}
		// The speaker, the same notes both ways: point and press A, or
		// press 1 or 2 (A and the D-pad are being tested here).
		y += 4;
		{
			int bw = (IW - 2 * GAP) / 3;

			button(ui, ID_SND_ADPCM, 0, y, bw, "ADPCM (1)", 0);
			button(ui, ID_SND_PCM, bw + GAP, y, bw, "PCM (2)", 0);
			button(ui, ID_SND_WAV, 2 * (bw + GAP), y, IW - 2 * (bw + GAP), "speaker.wav", 0);
		}
		y += BH;
		add(ui, K_TEXT, 0, 0, y + 4, IW, 16, F_LABEL | F_SMALL, "Press + and - together to go back.");
		return y + 22;
	case WM_CAL:
		remote_tag(ui, 0, y + 5, ui->wm_sel, false);
		snprintf(buf, sizeof(buf), "Remote %d calibration", ui->wm_sel + 1);
		title(ui, 70, y, IW - 70, buf);
		y += 32;
		if (sel->motionplus) {
			add(ui, K_TEXT, 0, 0, y, IW, 18, F_LABEL, "Place the remote face down on a flat");
			add(ui, K_TEXT, 0, 0, y + 18, IW, 18, F_LABEL, "surface and keep it still.");
		} else {
			add(ui, K_TEXT, 0, 0, y, IW, 18, F_LABEL, "Hold the remote still, pointing at the");
			add(ui, K_TEXT, 0, 0, y + 18, IW, 18, F_LABEL, "screen, and let go of any stick.");
		}
		y += 46;
		{
			ov_item *bar = add(ui, K_BAR, 0, 0, y, IW, 8, 0, NULL);

			bar->arg = e->cal_progress;
		}
		y += 16;
		if (e->cal_wait_s > 0)
			snprintf(buf, sizeof(buf), "Starting in %d s...", e->cal_wait_s);
		else
			snprintf(buf, sizeof(buf), "%s", e->cal_progress >= 100 ? e->cal_result : "Measuring...");
		value(ui, 0, y, IW, buf, F_LABEL);
		return y + 24;
	default:
		title(ui, 0, y, IW, "Controller settings");
		add(ui, K_TEXT, 0, 196, y + 3, IW - 196, 18, F_LABEL | F_SMALL, "until you exit");
		y += 32;
		button(ui, ID_CONNECT, 0, y, CW, e->searching ? "Press 1+2..." : "Connect remote",
			   e->searching ? F_ON : 0);
		button(ui, ID_DISC_ALL, CW + 10, y, CW, "Disconnect all", 0);
		y += BH + 12;
		choice(ui, ID_BAR_BELOW, ID_BAR_ABOVE, y, "Sensor bar", "Below", "Above", !e->sensor_above,
			   e->can_leds ? 0 : F_DIS);
		y += BH + GAP;
		snprintf(buf, sizeof(buf), "%d of 5", e->ir_sens);
		stepper(ui, ID_IR_MINUS, ID_IR_PLUS, y, "IR sensitivity", buf, e->can_leds ? 0 : F_DIS);
		y += BH + GAP;
		stepper(ui, ID_OFF_MINUS, ID_OFF_PLUS, y, "Auto power-off", e->auto_off, 0);
		y += BH + GAP;
		toggle(ui, ID_RUMBLE_ALL, y, "Rumble, all", e->rumble_all, 0);
		return y + BH;
	}
}

// An app slot's menu: a title, then its items in order. Info rows take a
// whole row; buttons pair up two to a row.
static int build_slot(ov_ui *ui, const ov_ext *e, int slot) {
	const ov_menu *m = &e->menu[slot];
	int y = 0, col = 0, i;

	title(ui, 0, y, IW, m->title);
	y += 30;
	for (i = 0; i < m->count; ++i) {
		const ov_menu_item *it = &m->item[i];

		if (it->flags & OV_ITEM_INFO) {
			if (col) {
				y += BH + GAP;
				col = 0;
			}
			add(ui, K_TEXT, 0, 0, y, 150, 20, F_LABEL, it->label);
			add(ui, K_TEXT, 0, 150, y, IW - 150, 20, 0, it->value);
			y += 22;
			continue;
		}
		if (!col && y > 30)
			y += 6;
		button(ui, ID_SLOT_ITEM + slot * 16 + i, col * (CW + 10), y, CW, it->label,
			   it->flags & OV_ITEM_DISABLED ? F_DIS : 0);
		if (col) {
			y += BH + GAP;
			col = 0;
		} else {
			col = 1;
		}
	}
	if (col)
		y += BH + GAP;
	return y - GAP;
}

static void layout(ov_ui *ui, const ov_ext *e) {
	int o = ease(ui->open_t), ex = ease(ui->exit_t);
	int sw = ui->w - 2 * M;
	int hide = (SH + EH + MB + 10) * (256 - o) / 256;
	int strip_y = ui->h - MB - SH - EH * ex / 256 + hide;
	int x, i, bw;
	char buf[16];

	ui->n = 0;
	if (ui->paused)
		return;

	// Drawers first, so the strip draws over their sliding edges.
	if (ui->dev_t) {
		int first = drawer_begin(ui), h = build_dev(ui, e);

		drawer_end(ui, first, h, M - (DW + M + 20) * (256 - ease(ui->dev_t)) / 256,
				   strip_y - 8);
	}
	if (ui->wm_t) {
		int first = drawer_begin(ui), h = build_wm(ui, e);

		drawer_end(ui, first, h, ui->w - M - DW + (DW + M + 20) * (256 - ease(ui->wm_t)) / 256,
				   strip_y - 8);
	}

	// App slot menus slide in from the side of their button.
	for (i = 0; i < 2; ++i)
		if (ui->slot_t[i]) {
			int first = drawer_begin(ui), h = build_slot(ui, e, i);
			int in = (DW + M + 20) * (256 - ease(ui->slot_t[i])) / 256;

			drawer_end(ui, first, h, i ? ui->w - M - DW + in : M - in, strip_y - 8);
		}

	add(ui, K_PANEL, 0, M, strip_y, sw, SH + EH * ex / 256, 0, NULL);
	add(ui, K_CLIP, 0, M, strip_y, sw, SH + EH * ex / 256, 0, NULL);

	// The Exit row, revealed as the strip grows.
	if (ui->exit_t) {
		static const char *names[] = { "The Homebrew Channel", "System Menu", "Restart Wii", "Power off" };
		int y = strip_y + PAD, shown = 0, units = 0, iw;

		for (i = 0; i < 4; ++i)
			if (e->exit_mask & (1 << i)) {
				shown++;
				units += i ? 10 : 13;
			}
		iw = sw - 2 * PAD - (shown - 1) * GAP;
		x = M + PAD;
		for (i = 0; i < 4; ++i) {
			int w;

			if (!(e->exit_mask & (1 << i)))
				continue;
			w = iw * (i ? 10 : 13) / units;
			button(ui, ID_EX_HBC + i, x, y, w, names[i], 0);
			x += w + GAP;
		}
	}

	// Status line: app, clock, player LEDs and batteries.
	{
		int y = strip_y + PAD + EH * ex / 256;
		ov_item *it = add(ui, K_TEXT, 0, M + PAD, y, sw / 2, STATUS_H, 0, e->app);

		it->font = ov_th->bold;
		x = ui->w - M - PAD;
		for (i = OV_REMOTES - 1; i >= 0; --i) {
			x -= e->remote[i].connected ? 80 : 50;
			remote_tag(ui, x, y + 3, i, !e->remote[i].connected);
		}
		add(ui, K_TEXT, 0, x - 64, y, 48, STATUS_H, F_RIGHT, e->clock);

		// Buttons: DEV, app slot, Exit, app slot (Shot by default), WiiMote.
		y += STATUS_H + 6;
		bw = (sw - 2 * PAD - 4 * GAP) / 5;
		for (i = 0; i < 5; ++i) {
			const char *t = i == 0 ? "DEV" : i == 2 ? "Exit" : i == 4 ? "WiiMote" : e->slot[i == 3];
			int flags = 0;

			if ((i == 1 || i == 3) && !t[0])
				flags = F_BLANK | F_DIS;
			if ((i == 0 && ui->menu == MENU_DEV) || (i == 2 && ui->menu == MENU_EXIT) ||
					(i == 4 && ui->menu == MENU_WM) || (i == 1 && ui->menu == MENU_SLOT0) ||
					(i == 3 && ui->menu == MENU_SLOT1))
				flags |= F_SEL;
			snprintf(buf, sizeof(buf), "%s", t);
			button(ui, ID_BAR + i, M + PAD + i * (bw + GAP), y, bw, buf, flags);
		}
	}
	add(ui, K_NOCLIP, 0, 0, 0, 0, 0, 0, NULL);

	if (e->toast[0]) {
		int w = ov_text_width(ov_th->regular, e->toast) + 32;
		// At the top, clear of the menus.
		add(ui, K_PANEL, 0, (ui->w - w) / 2, 24, w, 32, 0, NULL);
		add(ui, K_TEXT, 0, (ui->w - w) / 2, 24, w, 32, F_CENTER, e->toast);
	}
}

static bool in_layer(const ov_ui *ui, int id) {
	switch (ui->menu) {
	case MENU_DEV: return id >= 120 && id < 140;
	case MENU_EXIT: return id >= 110 && id < 120;
	case MENU_WM: return id >= 140 && id < 200;
	case MENU_SLOT0: return id >= ID_SLOT_ITEM && id < ID_SLOT_ITEM + 16;
	case MENU_SLOT1: return id >= ID_SLOT_ITEM + 16 && id < ID_SLOT_ITEM + 32;
	default: return id >= 100 && id < 110;
	}
}

static const ov_item *find(const ov_ui *ui, int id) {
	int i;

	for (i = 0; i < ui->n; ++i)
		if (ui->items[i].id == id && ui->items[i].kind == K_BUTTON)
			return &ui->items[i];
	return NULL;
}

static bool usable(const ov_item *it) {
	return it && (it->flags & F_FOCUS) && !(it->flags & F_DIS);
}

// Keep the focus on a usable item of the active layer.
static void fix_focus(ov_ui *ui, int preferred) {
	const ov_item *f = find(ui, ui->focus);
	int i;

	if (usable(f) && in_layer(ui, ui->focus))
		return;
	if (usable(find(ui, preferred)) && in_layer(ui, preferred)) {
		ui->focus = preferred;
		return;
	}
	for (i = 0; i < ui->n; ++i)
		if (usable(&ui->items[i]) && in_layer(ui, ui->items[i].id)) {
			ui->focus = ui->items[i].id;
			return;
		}
}

// Move to the nearest usable item in a direction, weighting sideways
// distance double so rows and columns feel straight.
static void move_focus(ov_ui *ui, int dx, int dy) {
	const ov_item *f = find(ui, ui->focus);
	int best = -1, best_d = 1 << 30, fx, fy, i;

	if (!f)
		return;
	fx = f->x + f->w / 2;
	fy = f->y + f->h / 2;
	for (i = 0; i < ui->n; ++i) {
		const ov_item *it = &ui->items[i];
		int cx = it->x + it->w / 2, cy = it->y + it->h / 2;
		int along = dx ? (cx - fx) * dx : (cy - fy) * dy;
		int side = dx ? cy - fy : cx - fx;
		int d;

		// Items sharing the row (or column) come first.
		bool overlap = dx ? it->y < f->y + f->h && f->y < it->y + it->h
						  : it->x < f->x + f->w && f->x < it->x + it->w;

		if (!usable(it) || it->id == ui->focus || !in_layer(ui, it->id) || along <= 0)
			continue;
		d = along + 2 * (side < 0 ? -side : side) + (overlap ? 0 : 100000);
		if (d < best_d) {
			best_d = d;
			best = it->id;
		}
	}
	if (best >= 0)
		ui->focus = best;
}

void ov_init(ov_ui *ui, int w, int h) {
	memset(ui, 0, sizeof(*ui));
	ui->w = w;
	ui->h = h;
	ui->focus = ui->bar_focus = ID_BAR + 2;
	ui->pointer = -1;
}

void ov_point(ov_ui *ui, const int x[OV_REMOTES], const int y[OV_REMOTES],
			  const float angle[OV_REMOTES], unsigned valid, int active) {
	int i;

	for (i = 0; i < OV_REMOTES; ++i) {
		ui->px[i] = x[i];
		ui->py[i] = y[i];
		ui->pa[i] = angle ? angle[i] : 0;
	}
	ui->pointing = valid;
	// Keep the remote in use while it points; otherwise take any that does.
	if (active >= 0 && (valid & (1u << active)))
		ui->pointer = active;
	else if (ui->pointer < 0 || !(valid & (1u << ui->pointer))) {
		ui->pointer = -1;
		for (i = 0; i < OV_REMOTES; ++i)
			if (valid & (1u << i)) {
				ui->pointer = i;
				break;
			}
	}
}

// The usable item under the pointer: in the menu that is open, or on the
// bar, whose buttons switch menus.
static int hit(const ov_ui *ui, int x, int y) {
	int i;

	for (i = ui->n - 1; i >= 0; --i) {
		const ov_item *it = &ui->items[i];

		if (it->kind == K_BUTTON && usable(it) &&
				(in_layer(ui, it->id) || (it->id >= ID_BAR && it->id < ID_BAR + 5)) &&
				x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h)
			return it->id;
	}
	return 0;
}

static void open_menu(ov_ui *ui, int menu, int bar_id) {
	ui->menu = menu;
	ui->bar_focus = bar_id;
	ui->focus = 0;
	ui->dev_page = PAGE_MAIN;
	ui->wm_page = WM_GRID;
}

static void close_menu(ov_ui *ui) {
	ui->menu = MENU_NONE;
	ui->focus = ui->bar_focus;
}

static void back(ov_ui *ui, ov_act_fn act, void *user) {
	if (ui->menu == MENU_WM && (ui->wm_page == WM_TEST || ui->wm_page == WM_CAL)) {
		if (ui->wm_page == WM_TEST)
			act(OVA_TEST_STOP, ui->wm_sel, user);
		ui->wm_page = WM_MORE;
		ui->focus = ID_RUMBLE;
	} else if (ui->menu == MENU_WM && ui->wm_page != WM_GRID) {
		ui->focus = ui->wm_page == WM_MORE ? ID_MORE + ui->wm_sel : ID_SETTINGS;
		ui->wm_page = WM_GRID;
	} else if (ui->menu == MENU_DEV && ui->dev_page == PAGE_LOG) {
		ui->dev_page = PAGE_MAIN;
		ui->focus = ID_LOG;
	} else if (ui->menu == MENU_DEV && ui->dev_page == PAGE_INFO) {
		ui->dev_page = PAGE_MAIN;
		ui->focus = ID_INFO_PAGE + ui->info_page;
	} else if (ui->menu != MENU_NONE) {
		close_menu(ui);
	} else {
		ui->closing = true;
	}
}

// The highlighted item, which A presses: what the pointer is on, if the
// pointer was used last and is on something; otherwise the D-pad's focus.
static int target(const ov_ui *ui) {
	if (ui->aiming && ui->pointer >= 0 && ui->hover)
		return ui->hover;
	// The Test page's D-pad is under test, so only the pointer highlights.
	if (ui->menu == MENU_WM && ui->wm_page == WM_TEST)
		return 0;
	return in_layer(ui, ui->focus) ? ui->focus : 0;
}

static void press(ov_ui *ui, const ov_ext *e, ov_act_fn act, void *user) {
	int id = target(ui);

	if (!id)
		return;
	if (id >= ID_BAR && id < ID_BAR + 5) {
		int i = id - ID_BAR;
		static const int menus[5] = { MENU_DEV, MENU_SLOT0, MENU_EXIT, MENU_SLOT1, MENU_WM };

		// A bar button of the menu that is open closes it.
		if (ui->menu != MENU_NONE && ui->menu == menus[i]) {
			close_menu(ui);
			return;
		}
		if (i == 0)
			open_menu(ui, MENU_DEV, id);
		else if (i == 2)
			open_menu(ui, MENU_EXIT, id);
		else if (i == 4)
			open_menu(ui, MENU_WM, id);
		else if ((i == 1 || i == 3) && e->menu[i == 3].count)
			open_menu(ui, i == 1 ? MENU_SLOT0 : MENU_SLOT1, id);
		else if (i == 3 && !strcmp(e->slot[1], "Shot"))
			act(OVA_SHOT, 0, user);
		else {
			ui->after = OVA_SLOT;
			ui->after_arg = i == 3;
			ui->closing = true;
		}
		return;
	}
	if (id >= ID_SLOT_ITEM && id < ID_SLOT_ITEM + 32) {
		int slot = (id - ID_SLOT_ITEM) / 16, item = (id - ID_SLOT_ITEM) % 16;

		if (e->menu[slot].item[item].flags & OV_ITEM_CLOSE) {
			ui->after = OVA_SLOT_ITEM;
			ui->after_arg = slot * 16 + item;
			ui->closing = true;
		} else {
			act(OVA_SLOT_ITEM, slot * 16 + item, user);
		}
		return;
	}
	if (id >= ID_EX_HBC && id <= ID_EX_POWER) {
		act(OVA_HBC + (id - ID_EX_HBC), 0, user);
		return;
	}
	if (id >= ID_FIND && id < ID_FIND + OV_REMOTES) {
		act(OVA_FIND, id - ID_FIND, user);
		return;
	}
	if (id >= ID_MORE && id < ID_MORE + OV_REMOTES) {
		ui->wm_sel = id - ID_MORE;
		ui->wm_page = WM_MORE;
		ui->focus = ID_RUMBLE;
		return;
	}
	switch (id) {
	case ID_TAB_ACT: ui->dev_tab = 0; break;
	case ID_TAB_INFO: ui->dev_tab = 1; break;
	case ID_RESTART_APP: ui->after = OVA_RESTART_APP; ui->closing = true; break;
	case ID_PAUSE: ui->paused = true; act(OVA_PAUSE, 1, user); break;
	case ID_SAVE: act(OVA_SAVE, 0, user); break;
	case ID_LOG: ui->dev_page = PAGE_LOG; ui->focus = 0; break;
	case ID_INFO_PAGE:
	case ID_INFO_PAGE + 1:
	case ID_INFO_PAGE + 2:
	case ID_INFO_PAGE + 3:
	case ID_INFO_PAGE + 4:
		ui->info_page = id - ID_INFO_PAGE;
		ui->dev_page = PAGE_INFO;
		ui->focus = 0;
		act(OVA_INFO, ui->info_page, user);
		break;
	case ID_LOGPC: act(OVA_LOG_PC, !e->log_pc, user); break;
	case ID_CRASH_3S: act(OVA_CRASH_STAY, 0, user); break;
	case ID_CRASH_STAY: act(OVA_CRASH_STAY, 1, user); break;
	case ID_HBCPY: act(OVA_HBCPY, !e->hbcpy, user); break;
	case ID_SETTINGS: ui->wm_page = WM_SETTINGS; ui->focus = ID_CONNECT; break;
	case ID_RUMBLE: act(OVA_RUMBLE, ui->wm_sel, user); break;
	case ID_VOL_MINUS: act(OVA_VOLUME, ui->wm_sel, user); break;
	case ID_VOL_PLUS: act(OVA_VOLUME, ui->wm_sel | 16, user); break;
	case ID_RESET_REMOTES: act(OVA_RESET_REMOTES, 0, user); break;
	case ID_SYNC_CLOCK: act(OVA_SYNC_CLOCK, 0, user); break;
	case ID_SND_ADPCM: act(OVA_SOUND_TEST, ui->wm_sel, user); break;
	case ID_SND_PCM: act(OVA_SOUND_TEST, ui->wm_sel | 16, user); break;
	case ID_SND_WAV: act(OVA_SOUND_TEST, ui->wm_sel | 32, user); break;
	case ID_TEST: ui->wm_page = WM_TEST; act(OVA_TEST_START, ui->wm_sel, user); break;
	case ID_CAL: ui->wm_page = WM_CAL; act(OVA_CAL_START, ui->wm_sel, user); break;
	case ID_DISC:
		act(OVA_DISCONNECT, ui->wm_sel, user);
		ui->wm_page = WM_GRID;
		ui->focus = ID_SETTINGS;
		break;
	case ID_CONNECT: act(OVA_CONNECT, 0, user); break;
	case ID_DISC_ALL: act(OVA_DISCONNECT_ALL, 0, user); break;
	case ID_BAR_BELOW: act(OVA_SENSOR_ABOVE, 0, user); break;
	case ID_BAR_ABOVE: act(OVA_SENSOR_ABOVE, 1, user); break;
	case ID_IR_MINUS: act(OVA_IR_SENS, e->ir_sens > 1 ? e->ir_sens - 1 : 1, user); break;
	case ID_IR_PLUS: act(OVA_IR_SENS, e->ir_sens < 5 ? e->ir_sens + 1 : 5, user); break;
	case ID_OFF_MINUS: act(OVA_AUTO_OFF, -1, user); break;
	case ID_OFF_PLUS: act(OVA_AUTO_OFF, 1, user); break;
	case ID_RUMBLE_ALL: act(OVA_RUMBLE_ALL, !e->rumble_all, user); break;
	}
}

bool ov_step(ov_ui *ui, const ov_ext *e, unsigned pressed, ov_act_fn act, void *user) {
	bool settled;

	ui->frame++;
	if (ui->paused) {
		if (pressed & OV_HOME) {
			ui->paused = false;
			act(OVA_PAUSE, 0, user);
			ui->closing = true;
		} else if (pressed) {
			ui->paused = false;
			act(OVA_PAUSE, 0, user);
		}
		pressed = 0;
	}

	// The Test page shows every button, B included: only + and - together
	// (or HOME) leave it. Its sound buttons play with 1 and 2, or with A
	// while the pointer is on one.
	if (ui->menu == MENU_WM && ui->wm_page == WM_TEST) {
		bool on_sound = ui->aiming && ui->pointer >= 0 &&
						(ui->hover == ID_SND_ADPCM || ui->hover == ID_SND_PCM || ui->hover == ID_SND_WAV);

		if (pressed & (OV_1 | OV_2 | OV_WAV))
			act(OVA_SOUND_TEST, ui->wm_sel | (pressed & OV_2 ? 16 : pressed & OV_WAV ? 32 : 0), user);
		pressed = (pressed & OV_HOME) | (pressed & OV_TEST_EXIT ? OV_B : 0) |
				  (on_sound ? pressed & OV_A : 0);
	}

	if (pressed & OV_HOME) {
		if (ui->menu == MENU_WM && ui->wm_page == WM_TEST)
			act(OVA_TEST_STOP, ui->wm_sel, user);
		ui->menu = MENU_NONE;
		ui->closing = true;
	} else if (!ui->closing && ui->open_t == 256) {
		if (pressed & OV_B)
			back(ui, act, user);
		else if (pressed & OV_A)
			press(ui, e, act, user);
		else if (pressed & (OV_UP | OV_DOWN | OV_LEFT | OV_RIGHT))
			ui->aiming = false;   // the D-pad takes over the highlight
		if (!(pressed & (OV_A | OV_B)) && (pressed & OV_UP))
			move_focus(ui, 0, -1);
		else if (!(pressed & (OV_A | OV_B)) && (pressed & OV_DOWN))
			move_focus(ui, 0, 1);
		else if (!(pressed & (OV_A | OV_B)) && (pressed & OV_LEFT))
			move_focus(ui, -1, 0);
		else if (!(pressed & (OV_A | OV_B)) && (pressed & OV_RIGHT))
			move_focus(ui, 1, 0);
	}
	if (ui->closing)
		ui->menu = MENU_NONE;

	ui->dev_t = approach(ui->dev_t, ui->menu == MENU_DEV);
	ui->wm_t = approach(ui->wm_t, ui->menu == MENU_WM);
	ui->exit_t = approach(ui->exit_t, ui->menu == MENU_EXIT);
	ui->slot_t[0] = approach(ui->slot_t[0], ui->menu == MENU_SLOT0);
	ui->slot_t[1] = approach(ui->slot_t[1], ui->menu == MENU_SLOT1);
	settled = !ui->dev_t && !ui->wm_t && !ui->exit_t && !ui->slot_t[0] && !ui->slot_t[1];
	// The strip leaves only once every menu has closed.
	ui->open_t = approach(ui->open_t, !(ui->closing && settled));

	layout(ui, e);
	ui->hover = ui->pointer >= 0 ? hit(ui, ui->px[ui->pointer], ui->py[ui->pointer]) : 0;
	// The pointer takes the highlight back once it moves a few pixels.
	if (ui->pointer >= 0) {
		int dx = ui->px[ui->pointer] - ui->aim_x, dy = ui->py[ui->pointer] - ui->aim_y;

		if (dx * dx + dy * dy > 36) {
			ui->aiming = true;
			ui->aim_x = ui->px[ui->pointer];
			ui->aim_y = ui->py[ui->pointer];
		}
	} else {
		ui->aiming = false;
	}
	// Pointing moves the D-pad's focus too, so the pad carries on from there.
	if (ui->aiming && ui->hover && in_layer(ui, ui->hover))
		ui->focus = ui->hover;
	fix_focus(ui, ui->menu == MENU_DEV ? ID_TAB_ACT : ui->menu == MENU_EXIT ? ID_EX_HBC :
			  ui->menu == MENU_WM ? ID_FIND : ui->menu == MENU_SLOT0 ? ID_SLOT_ITEM :
			  ui->menu == MENU_SLOT1 ? ID_SLOT_ITEM + 16 : ui->bar_focus);
	return !(ui->closing && !ui->open_t);
}

// FNV-1a over everything a frame's pixels depend on.
static uint32_t fnv(uint32_t h, const void *p, size_t n) {
	const uint8_t *b = p;

	while (n--)
		h = (h ^ *b++) * 16777619u;
	return h;
}

bool ov_changed(ov_ui *ui, const ov_ext *e) {
	uint32_t h = 2166136261u;
	int i, blink = 0;

	h = fnv(h, &ui->n, sizeof(ui->n));
	h = fnv(h, ui->items, ui->n * sizeof(ui->items[0]));
	h = fnv(h, &ui->focus, sizeof(ui->focus));
	h = fnv(h, &ui->hover, sizeof(ui->hover));
	h = fnv(h, &ui->aiming, sizeof(ui->aiming));
	h = fnv(h, &ui->pointing, sizeof(ui->pointing));
	h = fnv(h, &ui->pointer, sizeof(ui->pointer));
	h = fnv(h, ui->px, sizeof(ui->px));
	h = fnv(h, ui->py, sizeof(ui->py));
	h = fnv(h, ui->pa, sizeof(ui->pa));
	h = fnv(h, &ui->open_t, sizeof(ui->open_t));
	h = fnv(h, &ui->menu, sizeof(ui->menu));
	h = fnv(h, &ui->paused, sizeof(ui->paused));
	for (i = 0; i < OV_REMOTES; ++i) {
		h = fnv(h, &e->remote[i], sizeof(e->remote[i]));
		blink |= e->remote[i].finding;
	}
	// Find blinks the LEDs from the frame count.
	if (blink)
		h = fnv(h, &ui->frame, sizeof(ui->frame));
	if (h == ui->drawn)
		return false;
	ui->drawn = h;
	return true;
}

// HBC's look: dialog_background.png runs from dark blue at the top through
// black to dark blue; buttons are dark gray, focused ones blue.
// Two looks that must not be confused:
//  - the highlight (what A presses) glows: a blue bloom out from its edge,
//    like HBC's focused buttons;
//  - On (a setting that is active) is a solid blue ring round the button,
//    with no glow.
#define GLOW_FOCUS_SIZE 8
#define GLOW_FOCUS_ALPHA 130
#define ON_RING 2

static bool is_layer(int kind) {
	return kind == K_PANEL || kind == K_CARD || kind == K_CLIP || kind == K_NOCLIP;
}

static void glow(const ov_ui *ui, const ov_item *it, ov_canvas *c, ov_color col) {
	bool focused = !(it->flags & F_BLANK) && it->id == target(ui);

	if (it->kind != K_BUTTON || (it->flags & (F_DIS | F_BLANK)) || !focused)
		return;
	ov_glow(c, it->x, it->y, it->w, it->h, 6, GLOW_FOCUS_SIZE, col, GLOW_FOCUS_ALPHA);
}

void ov_draw(const ov_ui *ui, const ov_ext *e, ov_canvas *c) {
	const ov_color blue_top = ov_rgb(0x1f, 0x46, 0x5c), black = ov_rgb(0, 0, 0);
	const ov_color blue_bot = ov_rgb(0x12, 0x29, 0x37), edge = ov_rgb(0, 0, 0);
	const ov_color btn_top = ov_rgb(0x4a, 0x4a, 0x4a), btn_mid = ov_rgb(0x26, 0x26, 0x26);
	const ov_color btn_bot = ov_rgb(0x1a, 0x1a, 0x1a);
	const ov_color on_top = ov_rgb(0x5a, 0x94, 0xb4), on_mid = ov_rgb(0x2d, 0x68, 0x82);
	const ov_color on_bot = ov_rgb(0x1d, 0x4a, 0x60), on_rim = ov_rgb(0x6c, 0xc4, 0xff);
	const ov_color white = ov_rgb(0xff, 0xff, 0xff), grey = ov_rgb(0xa8, 0xa8, 0xa8);
	const ov_color labelc = ov_rgb(0xc9, 0xd6, 0xde), dimc = ov_rgb(0x5a, 0x60, 0x66);
	const ov_color bloom = ov_rgb(0x4f, 0xb8, 0xff), card = ov_rgb(0x1c, 0x3a, 0x4c);
	const ov_color hi_top = ov_rgb(0x8a, 0x8a, 0x8a), hi_mid = ov_rgb(0x55, 0x55, 0x55);
	const ov_color hi_bot = ov_rgb(0x40, 0x40, 0x40), on_hi = ov_rgb(0x8c, 0xc8, 0xe6);
	const ov_color led_on = ov_rgb(0x7f, 0xc4, 0xe0), led_off = ov_rgb(0x38, 0x38, 0x38);
	int i, glow_until = 0;

	(void) e;
	ov_noclip(c);
	for (i = 0; i < ui->n; ++i) {
		const ov_item *it = &ui->items[i];
		bool dis = it->flags & F_DIS;

		switch (it->kind) {
		case K_CLIP:
			ov_clip(c, it->x, it->y, it->w, it->h);
			break;
		case K_NOCLIP:
			ov_noclip(c);
			break;
		case K_PANEL:
			ov_panel(c, it->x, it->y, it->w, it->h, 10, blue_top, black, blue_bot, edge, 255);
			break;
		case K_CARD:
			ov_panel(c, it->x, it->y, it->w, it->h, 6, black, black, black, card, dis ? 90 : 160);
			break;
		case K_RULE:
			ov_fill(c, it->x, it->y, it->w, 1, card, 255);
			break;
		case K_BAR:
			ov_panel(c, it->x, it->y, it->w, it->h, 4, card, card, card, card, 255);
			ov_panel(c, it->x, it->y, it->w * (it->arg > 100 ? 100 : it->arg) / 100, it->h, 4,
					 led_on, led_on, led_on, led_on, 255);
			break;
		case K_REMOTE: {
			const ov_remote *r = &e->remote[it->arg];
			int k, x = it->x;

			for (k = 0; k < 4; ++k) {
				bool lit = k == it->arg ? !r->finding || (ui->frame / 8) & 1 : r->finding && (ui->frame / 8 + k) & 1;

				ov_disc(c, x + 3, it->y + 6, 3, lit && !dis ? led_on : dis && k == it->arg ?
						dimc : led_off);
				x += 8;
			}
			if (r->connected)
				for (k = 0; k < 4; ++k)
					ov_fill(c, x + 3 + k * 6, it->y + 1, 4, 10, k < r->battery ? led_on : led_off, 255);
			break;
		}
		case K_BUTTON: {
			bool focused = !(it->flags & F_BLANK) && it->id == target(ui);
			const ov_font *f = (it->flags & (F_ON | F_SEL)) || focused ? ov_th->bold : ov_th->regular;
			ov_color tc = dis ? dimc : (it->flags & (F_ON | F_SEL)) || focused ? white : grey;
			int tw = ov_text_width(f, it->text);

			if (it->flags & F_BLANK) {
				ov_panel(c, it->x, it->y, it->w, it->h, 6, black, black, black, dimc, 110);
				break;
			}
			// Every glow on this layer (up to the next panel, card or
			// clip) goes down before its first button, so a wide glow
			// never tints a neighbouring button, only what lies under it.
			if (i >= glow_until) {
				for (glow_until = i; glow_until < ui->n && !is_layer(ui->items[glow_until].kind);
					 ++glow_until)
					glow(ui, &ui->items[glow_until], c, bloom);
			}
			// On: the ring, then the button inside it. A highlighted button
			// has no black edge, so its bloom runs into it.
			if (it->flags & F_ON) {
				if (!dis)
					ov_panel(c, it->x - ON_RING, it->y - ON_RING, it->w + 2 * ON_RING,
							 it->h + 2 * ON_RING, 6 + ON_RING, on_rim, on_rim, on_rim, on_rim, 255);
				ov_panel(c, it->x, it->y, it->w, it->h, 6, focused ? on_hi : on_top, on_mid, on_bot,
						 dis ? edge : on_mid, dis ? 110 : 255);
			}
			else if (focused)
				ov_panel(c, it->x, it->y, it->w, it->h, 6, hi_top, hi_mid, hi_bot, hi_mid, 255);
			else
				ov_panel(c, it->x, it->y, it->w, it->h, 6, btn_top, btn_mid, btn_bot,
						 it->flags & F_SEL ? on_mid : edge, dis ? 110 : 255);
			ov_text(c, f, it->x + (it->w - tw) / 2, it->y + (it->h - f->height) / 2, it->text, tc);
			break;
		}
		case K_TEXT: {
			const ov_font *f = it->font;
			ov_color tc = dis ? dimc : it->flags & F_LABEL ? labelc : white;
			int tw = ov_text_width(f, it->text), x = it->x;

			if (it->flags & F_RIGHT)
				x = it->x + it->w - tw;
			else if (it->flags & F_CENTER)
				x = it->x + (it->w - tw) / 2;
			ov_text(c, f, x, it->y + (it->h - f->height) / 2, it->text, tc);
			break;
		}
		}
	}
	ov_noclip(c);

	// A hand per remote pointing at the screen, the one in use on top.
	if (!ui->paused) {
		int r;

		for (r = 0; r <= OV_REMOTES; ++r) {
			int k = r < OV_REMOTES ? r : ui->pointer;

			if (k < 0 || !(ui->pointing & (1u << k)) || (r < OV_REMOTES && k == ui->pointer))
				continue;
			ov_sprite_at(c, ov_th->cursor, ui->px[k], ui->py[k], ui->pa[k], ov_th->sx);
		}
	}
}
