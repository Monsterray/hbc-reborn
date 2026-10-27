#ifndef _GFX_H_
#define _GFX_H_

#include <ogc/gx.h>

#define COL_DEFAULT 0xFFFFFFFF

#define WIDESCREEN_RATIO (852.0/640.0)

typedef struct {
	f32 x, y, z;
} gfx_coordinates;

typedef enum {
	GFXT_RGBA8,
	GFXT_A8
} gfx_tex_type;

typedef enum {
	GFXE_GRADIENT,
	GFXE_TEXTURE,
	GFXE_DRAW
} gfx_entity_type;

// Where a GFXE_DRAW entity draws itself: its w x h in EFB pixels; z_far,
// the depth a sprite at its z would have, and z_near a thin band in front;
// and its width over its height as the TV shows it. An entity that needs
// more depth precision may draw from 0 to z_far if it then sets the
// rectangle's depth to z_far (gfx_depth_fill), as a sprite would leave it.
typedef struct {
	f32 x, y, w, h;
	f32 z_near, z_far;
	f32 aspect;
} gfx_rect;

typedef struct {
	gfx_entity_type type;
	u16 w, h;

	union {
		struct {
			u32 c1, c2, c3, c4;
		} gradient;

		struct {
			u8 *pixels;
			gfx_tex_type type;
			GXTexObj obj;
		} texture;

		// Draws with its own GX state inside the rectangle; HBC's state is
		// put back afterwards (gfx_restore()).
		struct {
			void (*fn) (void *ctx, const gfx_rect *r);
			void *ctx;
		} draw;
	};
} gfx_entity;

typedef enum {
	GFXQ_NULL = -1,
	GFXQ_ORIGIN,
	GFXQ_SCISSOR,
	GFXQ_ENTITY
} gfx_queue_type;

typedef struct {
	gfx_queue_type type;

	union {
		struct {
			gfx_coordinates *coords;
		} origin;

		struct {
			u16 x, y, z, w, h;
		} scissor;

		struct {
			gfx_coordinates coords;
			f32 scale;
			f32 rad;
			gfx_entity *entity;
			u32 color;
		} entity;
	};
} gfx_queue_entry;

extern bool widescreen;
extern u16 view_height, view_width;

void gfx_init_video (void);
void gfx_init (void);
void gfx_deinit (void);
void gfx_restore (void);
void gfx_depth_fill (const gfx_rect *r);

void gfx_get_efb_size(u16 *x, u16 *y);
// The video mode HBC set up, for the HOME overlay (home.c).
GXRModeObj *gfx_video_mode(void);
void gfx_set_efb_buffer(u32 *buffer);

void gfx_gen_gradient (gfx_entity *entity, u16 w, u16 h,
					   u32 c1, u32 c2, u32 c3, u32 c4);
void gfx_gen_tex (gfx_entity *entity, u16 w, u16 h, u8 *pixels, gfx_tex_type type);

void gfx_qe_origin_push (gfx_queue_entry *entry, gfx_coordinates *coords);
void gfx_qe_origin_pop (gfx_queue_entry *entry);
void gfx_qe_scissor_reset (gfx_queue_entry *entry);
void gfx_qe_scissor (gfx_queue_entry *entry, u16 x, u16 y, u16 z, u16 w, u16 h);
void gfx_qe_entity (gfx_queue_entry *entry, gfx_entity *entity, f32 x, f32 y,
					s16 layer, u32 color);

void gfx_frame_start (void);
void gfx_frame_push (const gfx_queue_entry *entries, int count);
void gfx_frame_end (void);
#ifdef HBC_DIAG_GPU
void gfx_diag_token (u16 token);
#endif

#endif

