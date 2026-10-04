// Software drawing into a YUYV framebuffer; see ov_draw.h.

#include <math.h>
#include <string.h>

#include "ov_draw.h"
#include "ov_font.h"

static const ov_theme theme_4x3 = { &ov_font_regular, &ov_font_bold, &ov_font_title,
									&ov_cursor, 256 };
static const ov_theme theme_16x9 = { &ov_font_regular_wide, &ov_font_bold_wide,
									 &ov_font_title_wide, &ov_cursor, 192 };
const ov_theme *ov_th = &theme_4x3;

void ov_set_widescreen(bool wide) {
	ov_th = wide ? &theme_16x9 : &theme_4x3;
}

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

// v * a / 255 without a divide (Broadway's takes up to 35 cycles), exact
// for the 0 to 255 range used here. v may be negative.
static inline int mul255(int v, int a) {
	int t = v * a + 128;

	return (t + (t >> 8)) >> 8;
}

// Blend one pixel. Luma is per pixel; chroma is shared by the pair, so an
// edge pixel tints its neighbour slightly.
static inline void put(ov_canvas *c, int x, int y, ov_color col, int a) {
	uint8_t *p;

	if (x < c->clip_x0 || x >= c->clip_x1 || y < c->clip_y0 || y >= c->clip_y1 || a <= 0)
		return;
	p = c->fb + y * c->stride + (x >> 1) * 4;
	if (a >= 255) {
		p[(x & 1) * 2] = col.y;
		p[1] = col.cb;
		p[3] = col.cr;
		return;
	}
	p[(x & 1) * 2] += mul255(col.y - p[(x & 1) * 2], a);
	p[1] += mul255(col.cb - p[1], a);
	p[3] += mul255(col.cr - p[3], a);
}

// An opaque run of pixels on row y from x0 to x1 (exclusive): whole pairs
// as one 32-bit store each, a lone pixel at either end through put().
static void span(ov_canvas *c, int x0, int x1, int y, ov_color col) {
	uint32_t word;
	uint8_t *p;
	int x;

	if (y < c->clip_y0 || y >= c->clip_y1)
		return;
	if (x0 < c->clip_x0)
		x0 = c->clip_x0;
	if (x1 > c->clip_x1)
		x1 = c->clip_x1;
	if (x0 >= x1)
		return;
	if (x0 & 1)
		put(c, x0++, y, col, 255);
	if (x1 & 1)
		put(c, --x1, y, col, 255);
	p = c->fb + y * c->stride + x0 * 2;
	memcpy(&word, (uint8_t[4]) { col.y, col.cb, col.y, col.cr }, 4);
	for (x = x0; x < x1; x += 2, p += 4)
		memcpy(p, &word, 4);
}

void ov_dim_copy(ov_canvas *dst, const uint8_t *src, int level) {
	static uint8_t lut_y[256], lut_c[256];
	static int lut_level = -1;
	int i, n = dst->h * dst->stride;

	// Two lookups per byte pair instead of two multiplies.
	if (lut_level != level) {
		for (i = 0; i < 256; ++i) {
			lut_y[i] = 16 + (((i - 16) * level) >> 8);
			lut_c[i] = 128 + (((i - 128) * level) >> 8);
		}
		lut_level = level;
	}
	if (level >= 256) {
		memcpy(dst->fb, src, n);
		return;
	}
	// One 32-bit load and store per pixel pair (Y Cb Y Cr, big-endian):
	// a quarter of the memory operations of byte at a time.
	{
		const uint32_t *s = (const uint32_t *) src;
		uint32_t *d = (uint32_t *) dst->fb;

		for (i = 0; i + 4 <= n; i += 4) {
			uint32_t v = *s++;

			*d++ = (uint32_t) lut_y[v >> 24] << 24 | (uint32_t) lut_c[(v >> 16) & 0xff] << 16 |
				   (uint32_t) lut_y[(v >> 8) & 0xff] << 8 | lut_c[v & 0xff];
		}
	}
}

void ov_fill(ov_canvas *c, int x, int y, int w, int h, ov_color col, int alpha) {
	int i, j;

	for (j = y; j < y + h; ++j) {
		if (alpha >= 255) {
			span(c, x, x + w, j, col);
			continue;
		}
		for (i = x; i < x + w; ++i)
			put(c, i, j, col, alpha);
	}
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

		if (edge || alpha < 255) {
			for (i = x + in; i < x + w - in; ++i)
				put(c, i, y + j, edge || i == x + in || i == x + w - in - 1 ? border : col, alpha);
			continue;
		}
		// Opaque rows: the left border pixel, one run, the right one, in
		// that order (a pixel pair shares its colour, so order matters).
		put(c, x + in, y + j, border, 255);
		span(c, x + in + 1, x + w - in - 1, y + j, col);
		put(c, x + w - in - 1, y + j, border, 255);
	}
}

void ov_glow(ov_canvas *c, int x, int y, int w, int h, int radius, int size, ov_color col,
			 int alpha) {
	int j, i, x0 = x - size, x1 = x + w + size;

	if (radius * 2 > h)
		radius = h / 2;
	if (size <= 0 || alpha <= 0)
		return;
	for (j = y - size; j < y + h + size; ++j) {
		// Vertical distance from the box's straight part.
		int dy = j < y + radius ? y + radius - j : j > y + h - 1 - radius ? j - (y + h - 1 - radius) : 0;
		bool band = j >= y + radius && j <= y + h - 1 - radius;

		for (i = x0; i < x1; ++i) {
			int dx = i < x + radius ? x + radius - i : i > x + w - 1 - radius ? i - (x + w - 1 - radius) : 0;
			float d, t;

			// Inside the button (drawn over this): skip the whole run.
			if (band && !dx) {
				i = x + w - 1;
				continue;
			}
			d = (dx && dy ? sqrtf((float) (dx * dx + dy * dy)) : (float) (dx + dy)) - radius;
			if (d <= 0) {
				if (!band && dy <= radius && i >= x + radius && i < x + w - radius)
					i = x + w - radius - 1;
				continue;
			}
			if (d >= size)
				continue;
			// Smoothstep: full at the edge, easing out to nothing.
			t = 1.f - d / size;
			put(c, i, j, col, (int) (alpha * t * t * (3.f - 2.f * t)));
		}
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

void ov_sprite_at(ov_canvas *c, const ov_sprite *s, int x, int y, float angle, int sx) {
	float a = angle * 3.14159265f / 180.f, ca = cosf(a), sa = sinf(a);
	float kx = sx / 256.f;
	int r = (int) sqrtf((float) (s->w * s->w + s->h * s->h)) + 1, i, j;

	// Every screen pixel near the hotspot, mapped back into the sprite:
	// undo the squeeze, then the turn, then sample the nearest texel.
	for (j = -r; j <= r; ++j)
		for (i = -r; i <= r; ++i) {
			float dx = i / kx, dy = (float) j;
			int u = (int) floorf(ca * dx + sa * dy + 0.5f) + s->hot_x;
			int v = (int) floorf(-sa * dx + ca * dy + 0.5f) + s->hot_y;
			const uint8_t *px;

			if (u < 0 || v < 0 || u >= s->w || v >= s->h)
				continue;
			px = s->rgba + (v * s->w + u) * 4;
			if (px[3])
				put(c, x + i, y + j, ov_rgb(px[0], px[1], px[2]), px[3]);
		}
}

void ov_image(ov_canvas *c, int x, int y, int w, int h, const uint8_t *rgba) {
	int i, j;

	for (j = 0; j < h; ++j)
		for (i = 0; i < w; ++i) {
			const uint8_t *px = rgba + (j * w + i) * 4;

			if (px[3])
				put(c, x + i, y + j, ov_rgb(px[0], px[1], px[2]), px[3]);
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
