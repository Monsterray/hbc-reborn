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
 *   - The body: RFL has none. With the Mii Channel's own (maleBody.brres,
 *     femaleBody.brres, which Save Mii faces copies beside RFL_Res.dat:
 *     mii_body_load, mii_model_set_body) it is the Wii's, posed by the Mii
 *     Channel's animations; without, this library's own. Either way a shirt
 *     in the favourite colour, trousers grey (gold for a special Mii), and
 *     scaled by height and build.
 *   - Units: the head's (a faceline is about 50 tall). Y is up, +Z is the
 *     way the Mii faces, the feet stand on Y = 0.
 *
 * Calling:
 *   - Every call that can fail returns >= 0 or a negative MII_E_* (mii.h).
 *   - mii_model_make takes about 0.1 s and 1 MiB (seven expressions, and
 *     each part as a display list): call it outside the frame loop, from
 *     any thread (it writes no GX commands). Drawing replays the lists, so
 *     a whole Mii puts about 2 KB into the GX FIFO, not its 240 KB of
 *     vertices (a frame's commands should stay well under the FIFO).
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

#define MIIDRAW_VERSION "1.1.0"   /* 1.1.0 (HBC 1.11.0): the Wii's body, mii_view */
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

/* ---- The Wii's own bodies (miibody.c) ------------------------------------ */

/* The Mii Channel's male and female bodies (maleBody.brres, femaleBody.brres
 * in its Character.szs), which HBC's Save Mii faces copies next to the
 * faces. Nintendo's, like RFL_Res.dat: read the user's own, never ship them.
 * Each holds a 20-bone skeleton, two skinned meshes (the shirt with its
 * hands, and the trousers) and 21 animations (bodyWait, bodyGreeting,
 * bodyWalk, bodyRun, bodyTalk, ...). Without them mii_model_make builds a
 * body of its own. */
#define MII_BODY_MALE_PATH "sd:/mii/maleBody.brres"
#define MII_BODY_FEMALE_PATH "sd:/mii/femaleBody.brres"
#define MII_BODY_BONES 32
#define MII_BODY_ANIMS 32

typedef struct {
	char name[20];
	int parent;            /* bone index, or -1 */
	int matrix;            /* the MDL0 matrix id its vertices use */
	float s[3], r[3], t[3];   /* local bind scale, rotation (degrees, X then Y then Z), translation */
	float bind_rot[3][3];  /* world rotation in the bind pose */
} mii_bone;

typedef struct {
	char name[24];
	int frames;
	struct {
		uint32_t code;     /* CHR0 v3 code: what is animated, constant, its format */
		float c[9];        /* constants: scale, rotation, translation (X Y Z each) */
		const uint8_t *keys[9];   /* keyframes, where not constant */
	} track[MII_BODY_BONES];
	int has[MII_BODY_BONES];
} mii_anim;

typedef struct {
	uint8_t *data;         /* the file, kept: animation keys point into it */
	uint32_t size;
	int nbones;
	mii_bone bone[MII_BODY_BONES];
	int head, chest;       /* bone indexes */
	int nverts;            /* skin vertices: a bone, a position and a normal in its space */
	int vcap;
	uint8_t *vbone;
	float *vpos, *vnrm;
	int ntris[2];          /* index triples into the skin vertices: shirt, trousers */
	uint16_t *tris[2];
	int nanims;
	mii_anim anim[MII_BODY_ANIMS];
} mii_body;

/* Checks and parses a copy in memory. On success it is taken over and
 * mii_body_free free()s it, so it must come from malloc or memalign; on
 * failure the caller still owns it. */
int mii_body_attach(mii_body *body, void *data, size_t size);
/* Reads one (path NULL: the male body). MII_E_NO_FILE without it. */
int mii_body_load(mii_body *body, const char *path);
void mii_body_free(mii_body *body);
/* The animation named so ("bodyWait"), or -1. */
int mii_body_anim(const mii_body *body, const char *name);

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
	float arm_len;         /* shoulder to the hand's centre, unscaled */
	float shoe[2];         /* a shoe's centre: height and how far forward */
	/* With a Wii body (mii_model_set_body): */
	const mii_body *body;
	float bone_scale[MII_BODY_BONES][3];   /* each bone's own scale, from height and build */
	float ground;          /* lifts the bind pose's feet to Y = 0 */
	float *skin_pos, *skin_nrm;   /* the posed vertices, rewritten each draw */
	int body_ready;        /* skin_pos and skin_nrm hold this draw's pose */
	uint32_t shirt, pants; /* colours, 0xRRGGBBAA */
} mii_model;

/* Builds a Mii. mask_size is the face texture's side (64, 128 or 256; 0:
 * 128). Free it with mii_model_free. */
int mii_model_make(const mii_res *res, const mii *m, int mask_size, mii_model *out);
void mii_model_free(mii_model *model);

/* Uses a loaded body (male or female, to match the Mii) for this model in
 * place of the built one; the body must outlive the model. 0 or MII_E_*. */
int mii_model_set_body(mii_model *model, const mii_body *body);

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
	/* With a Wii body: 1 + one of its animations (mii_body_anim), or 0 to
	 * stand still (Wait's first frame); at this frame, counted at 60 a
	 * second and wrapping round. The arm, leg and breathing fields above
	 * are for the built body. */
	int anim;
	float frame;
} mii_pose;

/* The pose's bones in the world, for a Wii body (portable: tests use it):
 * each bone's rotation and position, the body scale applied. */
void mii_body_pose(const mii_model *model, const mii_pose *pose, float rot[][3][3], float pos[][3]);
/* The posed vertices into model->skin_pos and skin_nrm (3 floats each). */
void mii_body_skin(mii_model *model, const mii_pose *pose);

/* The bounds of a standing Mii (feet at 0), for framing a camera. */
void mii_model_bounds(const mii_model *model, int head_only, float *top, float *half_width);

#ifndef MII_NO_OGC

/* ---- Drawing ----------------------------------------------------------------- */

/* Draws the Mii with your projection already loaded and `view` the camera
 * (the Mii stands at the world's origin; move it with `place`, or NULL).
 * Lit by one white light from the front-left-above, in view space.
 *
 * It changes, so set yours again after: the vertex descriptor and the
 * formats of GX_VTXFMT0 (position, normal, texture coordinate, F32) and,
 * with a Wii body, GX_VTXFMT1 (position, normal); GX_LIGHT0; one colour
 * channel (COLOR0 lit by GX_LIGHT0, ALPHA0 unlit) and its ambient and
 * material colours; one texture coordinate generation (TEXCOORD0 from TEX0,
 * identity); one TEV stage, its order and operation; GX_TEXMAP0; GX_PNMTX0's
 * position and normal matrices and the current matrix (GX_PNMTX0); Z (on,
 * LEQUAL, update) and its compare after texturing (GX_SetZCompLoc false);
 * blending (source alpha) and the alpha compare (alpha > 0); culling.
 * It assumes the default TEV swap tables, no indirect stages and no fog.
 *
 * It writes the model's skinning arrays: draw one model from one thread at
 * a time. */
void mii_draw(const mii_model *model, const mii_pose *pose, Mtx view, Mtx place);

/* A camera for drawing the Mii straight into your frame: the projection and
 * view that frame the whole Mii (or the head, for head_only) in a viewport
 * whose width over height on the TV is `aspect`, with `margin` around it.
 * Set your viewport (and a depth range of your choosing) to that area, load
 * `proj` with GX_PERSPECTIVE, then mii_draw(model, pose, view, NULL). This is
 * how to draw a Mii each frame: no copies, no pixel-format change. */
int mii_view(const mii_model *model, const mii_pose *pose, float aspect, float margin, Mtx44 proj,
			 Mtx view);

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
 * everything mii_draw changes: set yours again after (or at frame start).
 * For icons and stills; to show a Mii every frame, draw it in place with
 * mii_view and mii_draw: no copies, no pixel-format change. */
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
