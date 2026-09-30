// Software drawing into a YUYV framebuffer; see ov_draw.h.

#include <string.h>

#include "ov_draw.h"

ov_color ov_rgb(int r, int g, int b) {
	ov_color c;
	int y = (66 * r + 129 * g + 25 * b + 128) / 256 + 16;
	int cb = (-38 * r - 74 * g + 112 * b + 128) / 256 + 128;
	int cr = (112 * r - 94 * g - 18 * b + 128) / 256 + 128;

	c.y = y < 16 ? 16 : y > 235 ? 235 : y;
	c.cb = cb < 16 ? 16 : cb > 240 ? 240 : cb;
	c.cr = cr < 16 ? 16 : cr > 240 ? 240 : cr;
	return c;
}

void ov_clip(ov_canvas *c, int x, int y, int w, int h) {
	c->clip_x0 = x < 0 ? 0 : x;
	c->clip_y0 = y < 0 ? 0 : y;
	c->clip_x1 = x + w > c->w ? c->w : x + w;
	c->clip_y1 = y + h > c->h ? c->h : y + h;
}

void ov_noclip(ov_canvas *c) {
	ov_clip(c, 0, 0, c->w, c->h);
}

// Blend one pixel. Luma is per pixel; chroma is shared by the pair, so an
// edge pixel tints its neighbour slightly.
static inline void put(ov_canvas *c, int x, int y, ov_color col, int a) {
	uint8_t *p;

	if (x < c->clip_x0 || x >= c->clip_x1 || y < c->clip_y0 || y >= c->clip_y1 || a <= 0)
		return;
	if (a > 255)
		a = 255;
	p = c->fb + y * c->stride + (x >> 1) * 4;
	p[(x & 1) * 2] += ((col.y - p[(x & 1) * 2]) * a) / 255;
	p[1] += ((col.cb - p[1]) * a) / 255;
	p[3] += ((col.cr - p[3]) * a) / 255;
}

void ov_dim_copy(ov_canvas *dst, const uint8_t *src, int level) {
	int x, y;

	for (y = 0; y < dst->h; ++y) {
		const uint8_t *s = src + y * dst->stride;
		uint8_t *d = dst->fb + y * dst->stride;

		for (x = 0; x < dst->w * 2; x += 2) {
			d[x] = 16 + (((s[x] - 16) * level) >> 8);
			d[x + 1] = 128 + (((s[x + 1] - 128) * level) >> 8);
		}
	}
}

void ov_fill(ov_canvas *c, int x, int y, int w, int h, ov_color col, int alpha) {
	int i, j;

	for (j = y; j < y + h; ++j)
		for (i = x; i < x + w; ++i)
			put(c, i, j, col, alpha);
}

static ov_color mix(ov_color a, ov_color b, int t, int n) {
	ov_color c;

	c.y = a.y + (b.y - a.y) * t / n;
	c.cb = a.cb + (b.cb - a.cb) * t / n;
	c.cr = a.cr + (b.cr - a.cr) * t / n;
	return c;
}

// How much of row j's span a rounded corner cuts off, in pixels.
static int corner_inset(int j, int h, int r) {
	int d, k;

	if (j >= r && j < h - r)
		return 0;
	d = j < r ? r - j : j - (h - r - 1);
	for (k = 0; k <= r; ++k)
		if ((r - k) * (r - k) + (d - 0) * (d - 0) <= r * r)
			return k;
	return r;
}

void ov_panel(ov_canvas *c, int x, int y, int w, int h, int radius, ov_color top,
			  ov_color mid, ov_color bot, ov_color border, int alpha) {
	int j, i;

	if (radius * 2 > h)
		radius = h / 2;
	for (j = 0; j < h; ++j) {
		int in = corner_inset(j, h, radius);
		ov_color col = j < h / 2 ? mix(top, mid, j, h / 2) : mix(mid, bot, j - h / 2, h - h / 2);
		bool edge = j == 0 || j == h - 1;

		for (i = x + in; i < x + w - in; ++i)
			put(c, i, y + j, edge || i == x + in || i == x + w - in - 1 ? border : col, alpha);
	}
}

void ov_disc(ov_canvas *c, int cx, int cy, int r, ov_color col) {
	int i, j;

	for (j = -r; j <= r; ++j)
		for (i = -r; i <= r; ++i) {
			int d = i * i + j * j;

			if (d <= r * r)
				put(c, cx + i, cy + j, col, d > (r - 1) * (r - 1) ? 140 : 255);
		}
}

int ov_text_width(const ov_font *f, const char *s) {
	int w = 0;

	for (; *s; ++s)
		if (*s >= 32 && *s <= 126)
			w += f->glyphs[*s - 32].adv;
	return w;
}

int ov_text(ov_canvas *c, const ov_font *f, int x, int y, const char *s, ov_color col) {
	for (; *s; ++s) {
		const ov_glyph *g;
		const uint8_t *bits;
		int i, j;

		if (*s < 32 || *s > 126)
			continue;
		g = &f->glyphs[*s - 32];
		bits = f->bits + g->off;
		for (j = 0; j < g->h; ++j)
			for (i = 0; i < g->w; ++i)
				put(c, x + g->x + i, y + g->y + j, col, bits[j * g->w + i]);
		x += g->adv;
	}
	return x;
}
