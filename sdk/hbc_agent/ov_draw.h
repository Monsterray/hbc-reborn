/* Software drawing for the overlay, straight into a Wii external framebuffer
 * (YUYV: each 4 bytes hold Y0 Cb Y1 Cr for two pixels). Portable C, so the
 * host preview (tests/overlay_preview) draws exactly what the Wii shows. */

#ifndef OV_DRAW_H
#define OV_DRAW_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint8_t w, h;      /* bitmap size */
	int8_t x, y;       /* bitmap offset from the pen, y from the line top */
	uint8_t adv;       /* pen advance */
	uint16_t off;      /* offset into the bits */
} ov_glyph;

typedef struct {
	const ov_glyph *glyphs;   /* ASCII 32 to 126 */
	const uint8_t *bits;      /* 8-bit coverage */
	int ascent, height;
} ov_font;

typedef struct {
	const uint8_t *rgba;
	int w, h, hot_x, hot_y;
} ov_sprite;

/* The fonts and pointer to draw with: condensed ones on a 16:9 TV, which
 * stretches the 640-pixel picture sideways (ov_set_widescreen). */
typedef struct {
	const ov_font *regular, *bold, *title;
	const ov_sprite *cursor;
	int sx;               /* horizontal scale for sprites, in 1/256: 192 on 16:9 */
} ov_theme;

extern const ov_theme *ov_th;
void ov_set_widescreen(bool wide);

typedef struct {
	uint8_t *fb;     /* YUYV pairs */
	int w, h;        /* pixels; w is even */
	int stride;      /* bytes per row */
	int clip_x0, clip_y0, clip_x1, clip_y1;
} ov_canvas;

typedef struct {
	uint8_t y, cb, cr;
} ov_color;

ov_color ov_rgb(int r, int g, int b);
void ov_clip(ov_canvas *c, int x, int y, int w, int h);
void ov_noclip(ov_canvas *c);

/* dst = src darkened to level/256 (0 black, 256 unchanged), whole frame. */
void ov_dim_copy(ov_canvas *dst, const uint8_t *src, int level);
void ov_fill(ov_canvas *c, int x, int y, int w, int h, ov_color col, int alpha);
/* A rounded box with a vertical gradient (top, middle, bottom colors) and
 * a 1-pixel border. */
void ov_panel(ov_canvas *c, int x, int y, int w, int h, int radius, ov_color top,
			  ov_color mid, ov_color bot, ov_color border, int alpha);
void ov_disc(ov_canvas *c, int cx, int cy, int r, ov_color col);
/* A sprite with its hotspot at x, y, turned by angle degrees (clockwise, as
 * the remote twists) and scaled horizontally by sx/256, as HBC draws it. */
void ov_sprite_at(ov_canvas *c, const ov_sprite *s, int x, int y, float angle, int sx);
/* An RGBA image (8 bits each, rows top to bottom) with its top-left at x, y. */
void ov_image(ov_canvas *c, int x, int y, int w, int h, const uint8_t *rgba);
int ov_text_width(const ov_font *f, const char *s);
/* Draws s with its line top at y; returns the pen position after it. */
int ov_text(ov_canvas *c, const ov_font *f, int x, int y, const char *s, ov_color col);

#endif
