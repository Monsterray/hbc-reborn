/*
 * miidraw.h - drawing Miis on a Wii (libogc, GX): a Mii's head as the Wii
 * draws it, from the console's own face models, and a body to go with it.
 * sdk/mii, with mii.h. MIIDRAW_VERSION below.
 *
 *   mii_res_load(&res, NULL)      the face models: RFL_Res.dat, which the
 *                                 user saves to sd:/mii with HBC's HOME >
 *                                 HBC > Save Mii faces (never ship it)
 *   mii_model_make(&res, &m, ...) one Mii, built: its head's shapes placed
 *                                 as the Wii places them, its face drawn for
 *                                 each expression, a body in its colours
 *   mii_capture(model, ...)       draw it, posed, into a texture with alpha
 *                                 (the head alone: mii_icon)
 *   mii_draw(model, ...)          or draw it straight into your 3D scene
 *
 * How it draws (docs/research/mii-rendering.md has the sources):
 *   - The head is RFL's (the Wii's face library): each part's shape from
 *     RFL_Res.dat, placed by the faceline's nose, beard and hair anchors and
 *     the Mii's sizes and positions, coloured from RFL's tables.
 *   - The face (eyes, eyebrows, mouth, mustache, mole) is composited on the
 *     CPU into one texture per expression, as RFL lays them out; the parts
 *     RFL colours with TEV tricks (skin under make-up, the cap) are baked
 *     into plain textures. So drawing needs only "texture times light":
 *     no EFB format change, no TEV state beyond one stage.
 *   - The body is this library's own: RFL has none, and every game draws
 *     its own. A shirt in the favourite colour, trousers grey (gold for a
 *     special Mii), skin-coloured hands, scaled by height and build.
 *   - Units: the head's (a faceline is about 50 tall). Y is up, +Z is the
 *     way the Mii faces, the feet stand on Y = 0.
 *
 * Calling:
 *   - Every call that can fail returns >= 0 or a negative MII_E_* (mii.h).
 *   - mii_model_make takes about 0.1 s and 1 MiB (seven expressions, and
 *     each part as a display list): call it outside the frame loop, from
 *     any thread (it writes no GX commands). Drawing replays the lists, so
 *     a whole Mii puts about 2 KB into the GX FIFO, not its 240 KB of
 *     vertices: sent vertex by vertex they overran HBC's 256 KB FIFO on a
 *     Wii (garbage triangles, then a GPU hang; Dolphin never showed it).
 *   - mii_capture and mii_draw leave GX state changed (see each): restore
 *     yours after them.
 *
 * Building: compile miidraw.c with mii.c. With -DMII_NO_OGC the parts that
 * touch only bytes (the resource, meshes, the composited face) build on a
 * PC, for tools and tests. This file and miidraw.c are in the public domain.
 */

#ifndef MIIDRAW_H
#define MIIDRAW_H

#include <stddef.h>
#include <stdint.h>

#include "mii.h"

#ifndef MII_NO_OGC
#include <ogc/gx.h>
#include <ogc/gu.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MIIDRAW_VERSION "1.0.0"   /* sdk/mii's drawing, as of HBC 1.10.21 */
#define MII_RES_PATH "sd:/mii/RFL_Res.dat"

/* ---- The face models ----------------------------------------------------- */

/* RFL_Res.dat, checked: 18 archives, a version RFL accepts (0x39D on), every
 * archive and file inside the file. */
typedef struct {
	const uint8_t *data;
	uint32_t size;
	uint16_t version;
	uint32_t arc[18];      /* each archive's offset */
	uint16_t count[18];    /* its file count */
	void *owned;           /* what mii_res_load allocated */
} mii_res;

/* Checks a copy in memory and refers to it (keep it alive). */
int mii_res_attach(mii_res *res, const void *data, size_t size);
/* Reads and checks a file (path NULL: MII_RES_PATH). MII_E_NO_FILE when it
 * is not there: ask the user to save it from HBC's HOME menu. */
int mii_res_load(mii_res *res, const char *path);
void mii_res_free(mii_res *res);

/* ---- A Mii, built -------------------------------------------------------- */

enum {
	MII_EXPR_NORMAL,
	MII_EXPR_SMILE,
	MII_EXPR_ANGER,
	MII_EXPR_SORROW,
	MII_EXPR_SURPRISE,
	MII_EXPR_BLINK,
	MII_EXPR_OPEN_MOUTH,
	MII_EXPR_COUNT
};

/* One part's triangles, each vertex x y z, nx ny nz, s t. */
typedef struct {
	int count;             /* vertices, three to a triangle */
	float *v;
	uint32_t color;        /* 0xRRGGBBAA, for a part without a texture */
	int tex;               /* MII_TEX_*, or -1 */
	int flip;              /* mirrored: its triangles wind the other way */
	void *dl;              /* on the Wii: the triangles as a GX display list */
	uint32_t dl_size;
} mii_mesh;

enum { MII_TEX_FACE, MII_TEX_CAP, MII_TEX_NOSELINE, MII_TEX_GLASS, MII_TEX_COUNT };

/* A texture as linear RGBA bytes (and, on the Wii, GX's RGBA8 tiles). */
typedef struct {
	int w, h;
	int wrap_s, wrap_t;    /* 0 clamp, 1 repeat, 2 mirror (GX's values) */
	uint8_t *rgba;
#ifndef MII_NO_OGC
	void *gx;              /* GX_TF_RGBA8, 32-byte aligned */
	GXTexObj obj;
#endif
} mii_tex;

/* Head parts, in drawing order: opaque, then (from MII_PART_MASK) the
 * translucent ones. Body parts follow. */
enum {
	MII_PART_BEARD, MII_PART_NOSE, MII_PART_FOREHEAD, MII_PART_HAIR, MII_PART_CAP,
	MII_PART_FACELINE, MII_PART_MASK, MII_PART_NOSELINE, MII_PART_GLASS,
	MII_HEAD_PARTS,
	MII_BODY_TORSO = MII_HEAD_PARTS, MII_BODY_HIPS, MII_BODY_LEG, MII_BODY_SHOE,
	MII_BODY_ARM, MII_BODY_HAND, MII_BODY_NECK,
	MII_PARTS
};

typedef struct {
	mii m;
	mii_mesh part[MII_PARTS];
	mii_tex tex[MII_TEX_COUNT];
	mii_tex mask[MII_EXPR_COUNT];
	float body_scale[2];   /* width and height of the body, from build and height */
	float head_y;          /* where the chin sits, body scaled */
	float shoulder[3];     /* the right shoulder (mirror X for the left) */
	float hip[3];          /* the right leg's top */
} mii_model;

/* Builds a Mii. mask_size is the face texture's side (64, 128 or 256; 0:
 * 128). Free it with mii_model_free. */
int mii_model_make(const mii_res *res, const mii *m, int mask_size, mii_model *out);
void mii_model_free(mii_model *model);

/* ---- Posing ---------------------------------------------------------------- */

/* Angles in radians. All 0: standing, facing the camera, arms down. */
typedef struct {
	float yaw;             /* the whole Mii, about Y */
	float head_yaw, head_pitch, head_roll;
	float arm_raise[2];    /* right, left: out to the side (pi/2 level, pi up) */
	float arm_swing[2];    /* forward (+) and back */
	float leg_swing[2];
	float bob;             /* up and down, in head units (a step, a jump) */
	float breathe;         /* 0 to 1: the chest a little fuller */
	int expression;        /* MII_EXPR_* */
	int head_only;         /* no body: the head, chin at Y = 0 */
} mii_pose;

/* The bounds of a standing Mii (feet at 0), for framing a camera. */
void mii_model_bounds(const mii_model *model, int head_only, float *top, float *half_width);

#ifndef MII_NO_OGC

/* ---- Drawing ----------------------------------------------------------------- */

/* Draws the Mii with your projection already loaded and `view` the camera
 * (the Mii stands at the world's origin; move it with `place`, or NULL).
 * Lit by one white light from the front-left-above, in view space. It sets
 * the vertex format (VTXFMT0: position, normal, one texture coordinate, all
 * F32), one colour channel lit by GX_LIGHT0, one TEV stage, Z (compare and
 * update), culling, blending (source alpha) and the alpha compare; restore
 * yours after. */
void mii_draw(const mii_model *model, const mii_pose *pose, Mtx view, Mtx place);

/* Draws the Mii into the EFB's top-left w x h pixels and copies that, with
 * alpha (clear where there is no Mii), to `dst` as GX_TF_RGBA8 (w*h*4 bytes,
 * 32-byte aligned; w and h multiples of 4, at most 640 x 528), clearing the
 * area to `clear` afterwards. The camera frames the whole Mii, or the head
 * for head_only, with `margin` (0.05 is a little room) around it;
 * `stretch_x` widens it for a widescreen sprite (1: none).
 *
 * Call it between frames: after your last copy to the screen, before you
 * draw the next frame. It changes the EFB pixel format (RGBA6 while it
 * draws, then RGB8_Z24), the viewport, scissor, projection, copy filter and
 * everything mii_draw changes: set yours again after (or at frame start). */
int mii_capture(const mii_model *model, const mii_pose *pose, int w, int h, float margin,
				float stretch_x, GXColor clear, void *dst);

/* A head icon, as the Wii Menu's: size x size, the head framed as RFL frames
 * it, on `background` (alpha 0 for none). Same rules as mii_capture. */
int mii_icon(const mii_model *model, int expression, int size, GXColor background, void *dst);

#endif /* MII_NO_OGC */

#ifdef __cplusplus
}
#endif

#endif
