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
 * Many Miis (a crowd, a plaza): build them with mii_model_build and
 * mii_model_opts: one mii_cache for all of them (identical parts are kept
 * once), only the expressions you draw, levels of detail (opts.lods with
 * mii_body_make_lods; each frame pose.lod = mii_body_lod_for), and the
 * faces drawn by the GPU (opts.gpu_faces, then mii_model_make_faces between
 * frames). sdk/mii/README.md, "Many Miis", has the numbers.
 *
 * How it draws (docs/research/mii-rendering.md has the sources):
 *   - The head is RFL's (the Wii's face library): each part's shape from
 *     RFL_Res.dat, placed by the faceline's nose, beard and hair anchors and
 *     the Mii's sizes and positions, coloured from RFL's tables.
 *   - The face (eyes, eyebrows, mouth, mustache, mole) is one texture per
 *     expression, laid out as RFL lays them out: composited on the CPU, or
 *     drawn by the GPU (gpu_faces) as RFL does. The parts RFL colours with
 *     TEV tricks (skin under make-up, the cap) are baked into plain
 *     textures. So drawing needs only "texture times light": no EFB format
 *     change, no TEV state beyond one stage.
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
 *   - mii_model_make takes about 0.1 s and, with all seven expressions at
 *     128 px, about 280 KB (one expression: about 70 KB; with a cache
 *     shared by many Miis, 12 to 31 KB each). Call it outside the frame
 *     loop, from any thread (it writes no GX commands; Miis sharing a
 *     cache are built and freed on one thread at a time). Drawing replays
 *     display lists: a whole Mii puts a few KB into the GX FIFO, not its
 *     vertices, and costs the CPU about 16 us (a frame's commands should
 *     stay well under the FIFO).
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

#define MIIDRAW_VERSION "1.2.1"   /* 1.1.0 (HBC 1.11.0): the Wii's body, mii_view; 1.1.1: its depth; 1.2.0 (HBC 1.12.0): crowds; 1.2.1: levels by eye, hair whole */
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
#define MII_PALETTES 4
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

/* One MDL0 object as the GPU draws it: its own display list (the primitives'
 * vertex format rewritten to GX_VTXFMT1, so the head's VTXFMT0 is left
 * alone), with indexed matrix loads that take each bone's matrices from the
 * model's palette (GX_POSMTXARRAY, GX_NRMMTXARRAY) by matrix id. */
typedef struct {
	uint8_t *dl;           /* 32-byte aligned, padded with GX_NOPs */
	uint32_t dl_size;
	uint32_t vcd;          /* the MDL0 vertex descriptor (CP 0x50) */
	int mesh;              /* 0 the shirt (with the hands), 1 the trousers */
	int pos, nrm;          /* which of the vertex arrays */
} mii_body_obj;

/* A vertex array as the GPU reads it: a 32-byte aligned copy. */
typedef struct {
	uint8_t *gx;
	uint32_t stride;
	uint8_t fmt, frac;     /* GX_U8 .. GX_F32, and the fixed-point shift */
} mii_body_array;

#define MII_BODY_OBJS 8
#define MII_BODY_MATRICES 64   /* matrix ids a body may use */
#define MII_POSE_MEMO 16       /* poses a body remembers */
#define MII_BODY_LODS 4        /* levels of detail mii_body_make_lods can add */

/* A coarser body (mii_body_make_lods): each object's own display list, its
 * triangles a subset of the body's vertices (no new vertex data). */
typedef struct {
	uint8_t *dl[MII_BODY_OBJS];
	uint32_t dl_size[MII_BODY_OBJS];
	int tris;              /* triangles, all objects */
	float err;             /* the largest fold this level made, in body units */
} mii_body_lod;

/* A pose's bones, local (each one's rotation and translation from its
 * parent, before any Mii's scale): what the animation keys give, worked out
 * once for every Mii at that frame. */
typedef struct {
	const void *a, *a2;    /* the animations (NULL: none), */
	float f, f2, w;        /* their frames and the fade */
	float r[MII_BODY_BONES][3][3], t[MII_BODY_BONES][3];
} mii_pose_memo;

/* A loaded body. Read-only to the app once loaded: drawing writes only its
 * pose memo (below). */
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
	/* For the GPU: the objects, their arrays, and matrix id -> bone. */
	int nobjs;
	mii_body_obj obj[MII_BODY_OBJS];
	mii_body_array pos_array[8], nrm_array[8];
	int nmatrices;         /* matrix ids 0 .. nmatrices - 1 */
	int8_t matrix_bone[MII_BODY_MATRICES];   /* the bone with that id, or -1 */
	/* The last MII_POSE_MEMO poses drawn: Miis at the same frame of the same
	 * animation pose once between them. Scratch, written by drawing: draw a
	 * body's models from one thread. */
	mii_pose_memo *memo;
	int memo_next;
	int wait_anim;         /* bodyWait's index (or -1): a pose's anim 0 */
	uint32_t memo_hits, memo_misses;
	/* Levels of detail (mii_body_make_lods): lod[0] is level 1. */
	int full_tris;         /* the whole body's triangles */
	int nlods;
	mii_body_lod lod[MII_BODY_LODS];
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

/* Makes coarser levels of the body, as games draw a crowd's far Miis (Wii
 * Party's bodies go 2,092, 1,296, 776, 448, 240 triangles), up to
 * MII_BODY_LODS: each level folds while a fold moves the surface by no
 * more than its tolerance (body units, finest first). NULL: 0.57, 0.94,
 * 1.7 and 3.4, half a pixel at a Mii 150, 90, 50 and 25 pixels tall,
 * finer than each level needs where mii_body_lod_for draws it; n 0: all
 * four. Built here from the user's own body, so nothing Nintendo made is
 * shipped: each step folds a vertex into a neighbour on the same bone, the
 * fold that moves the surface least (quadric error over the area behind
 * it), keeping open edges and turning no triangle more than 60 degrees.
 * It frees the levels made before: not while the GPU may still draw them.
 * With your own tolerances, n is how many (1 to MII_BODY_LODS).
 * 0 or MII_E_*. */
int mii_body_make_lods(mii_body *body, const float *tolerances, int n);
/* The level for a Mii this many pixels tall on screen (mii_pixels_tall):
 * 0 (the whole body) from 90, then one level more under each of 90, 39,
 * 30.5 and 25, as far as made. Chosen by eye on a real Wii, Miis walking
 * through the levels (tests/mii_app parade). */
int mii_body_lod_for(const mii_body *body, float pixels_tall);

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

typedef struct mii_cache mii_cache;   /* below: parts shared between Miis */

#define MII_HEAD_LODS 4        /* coarser heads mii_model_opts.lods can ask for */

/* One part's triangles, each vertex x y z, nx ny nz, s t. */
typedef struct {
	int count;             /* vertices, three to a triangle */
	float *v;              /* as floats (on the Wii, freed once packed) */
	uint32_t color;        /* 0xRRGGBBAA, for a part without a texture */
	int tex;               /* MII_TEX_*, or -1 */
	int flip;              /* mirrored: its triangles wind the other way */
	/* Packed, as the GPU draws it (RFL keeps its shapes as s16 too): the
	 * distinct vertices, each array 32-byte aligned in one block: positions
	 * s16 x 3 with pos_frac fraction bits, normals s8 x 3 (6), texture
	 * coordinates s16 x 2 (st_frac); and each corner's vertex. */
	int nverts;
	int pos_frac, st_frac;
	uint8_t *pack;
	uint32_t pack_nrm, pack_st;   /* where the normals and coordinates start */
	uint16_t *idx;         /* count corners (on the Wii, in the display list instead) */
	void *dl;              /* on the Wii: the triangles as a GX display list */
	uint32_t dl_size;
	/* Coarser levels (mii_model_opts.lods): fewer triangles over the same
	 * vertices. On a PC each level's corners follow the full list in idx
	 * (lod_first); on the Wii its own list follows in dl (lod_at, lod_size). */
	int lods;
	int lod_count[MII_HEAD_LODS];
	uint32_t lod_first[MII_HEAD_LODS];
	uint32_t lod_at[MII_HEAD_LODS], lod_size[MII_HEAD_LODS];
	void *shared;          /* a mii_cache entry holding pack and dl (on a PC, idx), or NULL */
	uint32_t whole;        /* with a cache: the hash it finds this part again by */
} mii_mesh;

enum { MII_TEX_FACE, MII_TEX_CAP, MII_TEX_NOSELINE, MII_TEX_GLASS, MII_TEX_COUNT };

/* A texture: on the Wii, GX's RGB5A3 tiles (rgba NULL once built); on a PC,
 * linear RGBA bytes. */
typedef struct {
	int w, h;
	int wrap_s, wrap_t;    /* 0 clamp, 1 repeat, 2 mirror (GX's values) */
	uint8_t *rgba;
#ifndef MII_NO_OGC
	void *gx;              /* GX_TF_RGB5A3, 32-byte aligned */
	GXTexObj obj;
#endif
	void *shared;          /* a mii_cache entry holding gx (on a PC, rgba), or NULL */
} mii_tex;

/* The texture as GX's RGB5A3 (RFL's format for its faces): 4 x 4 tiles of
 * big-endian 16-bit texels, opaque ones 1RRRRRGGGGGBBBBB, the rest
 * 0AAARRRRGGGGBBBB; mii_tex_rgb5a3_size bytes. What the Wii uploads;
 * portable, for tests. */
uint32_t mii_tex_rgb5a3_size(int w, int h);
void mii_tex_rgb5a3(const mii_tex *t, uint8_t *out);

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

/* A built Mii. Read what you need (m, the bounds below); the rest is the
 * library's own, written by building and drawing. */
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
	float *skin_pos, *skin_nrm;   /* posed by mii_body_skin (the CPU reference; made by its first call) */
	/* The GPU's palette: each matrix id's position (3 x 4) and normal (3 x 3)
	 * matrix, a ring of MII_PALETTES so a draw does not rewrite one the GPU
	 * may still be reading: up to that many draws of this model between two
	 * GX_DrawDone. */
	float (*pal_pos)[3][4];
	float (*pal_nrm)[3][3];
	/* The bones as the last draw posed them (mii_pose.reuse): each one's
	 * matrices, and the head's place. */
	float (*last_pm)[3][4];
	float (*last_w)[3][4];   /* unscaled: the normals' rotation */
	float last_head[3][4];
	int posed;
	int pal_next;
	int pal_size;          /* entries in each: the body's matrix ids, rounded up to 8 */
	uint32_t shirt, pants; /* colours, 0xRRGGBBAA */
	int mask_size;         /* as built (mii_model_opts) */
	unsigned expressions;
	mii_cache *cache;
	int gpu_faces, faces_pending;   /* mii_model_opts.gpu_faces; faces still to draw */
	/* On the Wii, each head level as two display lists (made by its first
	 * draw): the parts' state and triangles before the face, and after it,
	 * so a draw is a few matrix loads, the face and two calls. Scratch,
	 * written by drawing. */
	void *head_dl[MII_HEAD_LODS + 1][2];
	uint32_t head_dl_size[MII_HEAD_LODS + 1][2];
} mii_model;

/* Which expressions' faces to build: MII_EXPR_BIT(MII_EXPR_SMILE) | ...
 * Each is a texture of its own (128 px: 32 KB on the GPU), so build only
 * those you draw, as RFLInitCharModel's expression flags do. The normal
 * face is always built; drawing one not built draws it. */
#define MII_EXPR_BIT(e) (1u << (e))
#define MII_EXPRS_ALL ((1u << MII_EXPR_COUNT) - 1)

/* Miis built with one cache share the parts that come out the same: the
 * same hair, face shape, nose or glasses, the same textures, even the same
 * face. Free the cache after every model built with it; one thread at a
 * time. */
mii_cache *mii_cache_new(void);
void mii_cache_free(mii_cache *cache);
/* What it holds: distinct parts, their bytes, and how many models' parts
 * use them. */
void mii_cache_stats(const mii_cache *cache, int *entries, uint32_t *bytes, int *users);

typedef struct {
	int mask_size;         /* the face texture's side: 32, 64, 128 or 256 (0: 128) */
	unsigned expressions;  /* MII_EXPR_BIT()s (0: all of them) */
	mii_cache *cache;      /* share identical parts with other Miis built with it, or NULL */
	/* Coarser heads for Miis far off, drawn by mii_pose.lod: 0 to
	 * MII_HEAD_LODS levels, each changing the head by no more than half a
	 * pixel at a Mii 150, 90, 50 and 25 pixels tall (drawn smaller: from
	 * 90, 39, 30.5 and 25), the hair by a quarter.
	 * A head is about 480 triangles, more than a far body: in a crowd the
	 * GPU's time is mostly heads (REVIEW.md, 1.11.8). Some ms to build;
	 * Miis sharing a part through a cache share its levels. */
	int lods;
	/* 1: the faces drawn by the GPU, as RFL does (RFLiMakeTexture): about
	 * 0.08 ms each against 1 to 6 ms on the CPU. They need the EFB, so
	 * mii_model_build leaves them to mii_model_make_faces, which the thread
	 * that draws calls between frames (as mii_capture). Until then the
	 * model draws without a face. A PC build draws them on the CPU. */
	int gpu_faces;
} mii_model_opts;

/* Builds a Mii (opts NULL: 128 px, every expression). Free it with
 * mii_model_free. On the Wii only the GPU's copies are kept: the mii_tex
 * rgba pointers are NULL after it (on a PC, which has no GPU copy, they
 * stay). */
int mii_model_build(const mii_res *res, const mii *m, const mii_model_opts *opts, mii_model *out);
/* mii_model_build with that mask size and every expression. */
int mii_model_make(const mii_res *res, const mii *m, int mask_size, mii_model *out);
/* Rebuilds the model's faces (the expressions it was built with) at
 * another size: 32 px for a Mii far off, 128 near (the faces are most of a
 * Mii's memory). About as long as building them did. With gpu_faces it
 * only asks: mii_model_make_faces draws them (faces_pending), and the old
 * ones draw until then. On failure the model keeps its faces. Do not call
 * it while the GPU may still draw the model. */
int mii_model_set_mask_size(mii_model *model, const mii_res *res, int mask_size);
void mii_model_free(mii_model *model);

/* Uses a loaded body (male or female, to match the Mii) for this model in
 * place of the built one, whose parts it frees; the body must outlive the
 * model. 0 or MII_E_*. */
int mii_model_set_body(mii_model *model, const mii_body *body);
/* Frees a part's triangles, leaving it empty. */
void mii_mesh_free(mii_mesh *mesh);

/* Triangles (ntri index triples) as strips, the way the GPU takes them
 * cheapest: a strip of n vertices draws n - 2 triangles, and the GPU
 * transforms each vertex it is sent (its vertex cache holds what it reads,
 * not what it works out). Greedy: from the triangle with the fewest free
 * neighbours, on through the neighbour across the strip's last edge, the
 * winding kept. out gets the strips' vertices, lens each strip's length;
 * returns how many strips (each at most 65535 vertices), or -1 (no
 * memory). out needs 3 * ntri entries, lens ntri. */
int mii_strips(const uint16_t *tri, int ntri, uint16_t *out, uint16_t *lens);

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
	/* A cross-fade from the animation before, so a change of animation does
	 * not snap: from_anim and from_frame as anim and frame were, and
	 * from_weight its share, 1 at the change down to 0 (0.3 to 0.4 s is
	 * usual; mii_fade_weight). Each bone's rotation is blended as a
	 * quaternion (the short way), its position linearly. 0: no fade.
	 * Frames wrap: let a looping animation (bodyWait) play on as it fades,
	 * but hold a finished one-shot (bodyGreeting) at its last frame, or the
	 * fade blends toward it starting over. */
	int from_anim;
	float from_frame;
	float from_weight;
	/* 1: the bones as this model's last draw posed them, the fields above
	 * unread but the head's angles and the expression: a Mii far off posed
	 * every second or fourth frame costs about a third. */
	int reuse;
	/* 0 the whole Mii; 1 to 4 coarser levels for a Mii far off
	 * (mii_body_lod_for): the Wii body's (mii_body_make_lods) and the
	 * head's (mii_model_opts.lods), each as far as it has levels. */
	int lod;
} mii_pose;

/* The weight for a cross-fade `seconds` after a change of animation that
 * takes `length` seconds: 1 at the change, eased to 0 (a smoothstep). */
float mii_fade_weight(float seconds, float length);

/* The pose's bones in the world, for a Wii body (portable: tests use it):
 * each bone's rotation and position, the body scale applied. This and the
 * two below write the body's pose memo, as drawing does: one thread per
 * body. */
void mii_body_pose(const mii_model *model, const mii_pose *pose, float rot[][3][3], float pos[][3]);
/* The posed vertices into model->skin_pos and skin_nrm (3 floats each): the
 * CPU reference the GPU's palette is tested against. Without the memory
 * for them they stay NULL. */
void mii_body_skin(mii_model *model, const mii_pose *pose);
/* Each bone's matrices in the body's space for the pose: position = rotation
 * times the bone's own scale, then its position; normal = the rotation (unit
 * length kept: the GPU does not renormalise). What the GPU draws with,
 * before the camera and the model's placement. */
void mii_body_matrices(const mii_model *model, const mii_pose *pose, float pos[][3][4], float nrm[][3][3]);
/* The same, from a pose mii_body_pose worked out (model, its body and the
 * arrays must be valid: no checks). */
void mii_body_matrices_posed(const mii_model *model, float rot[][3][3], float at[][3], float pos[][3][4],
							 float nrm[][3][3]);
#ifndef MII_NO_OGC
/* Each bone in the body's space as a matrix (rotation and position, the
 * scale not applied): mii_body_pose, on the Wii's paired-single unit (each
 * bone its parent's matrix times its own, ps_guMtxConcat). What mii_draw
 * poses with. */
void mii_body_world(const mii_model *model, const mii_pose *pose, Mtx world[]);
#endif

/* The bounds of a standing Mii (feet at 0), for framing a camera. */
void mii_model_bounds(const mii_model *model, int head_only, float *top, float *half_width);

#ifndef MII_NO_OGC

/* ---- Drawing ----------------------------------------------------------------- */

/* Draws the Mii with your projection already loaded and `view` the camera
 * (the Mii stands at the world's origin; move it with `place`, or NULL).
 * Lit by one white light from the front-left-above, in view space.
 *
 * It changes, so set yours again after: the vertex descriptor, the
 * position, normal and texture coordinate arrays, and the formats of
 * GX_VTXFMT0 (indexed s16 positions, s8 normals, s16 coordinates) and, with
 * a Wii body, GX_VTXFMT1 (its own); GX_LIGHT0; one colour
 * channel (COLOR0 lit by GX_LIGHT0, ALPHA0 unlit) and its ambient and
 * material colours; one texture coordinate generation (TEXCOORD0 from TEX0,
 * identity); one TEV stage, its order and operation; GX_TEXMAP0; the
 * position and normal matrices of GX_PNMTX0 and, for a coarser head
 * (pose.lod), GX_PNMTX1 and GX_PNMTX2, and the current matrix (GX_PNMTX0);
 * with a Wii body, the GX_POSMTXARRAY, GX_NRMMTXARRAY and GX_TEXMTXARRAY
 * arrays; Z (on, LEQUAL, update) and its compare after texturing
 * (GX_SetZCompLoc false);
 * blending (source alpha) and the alpha compare (alpha > 0); culling.
 * It assumes the default TEV swap tables, no indirect stages and no fog.
 *
 * It writes the model's skinning arrays: draw one model from one thread at
 * a time. */
void mii_draw(const mii_model *model, const mii_pose *pose, Mtx view, Mtx place);

#ifdef MII_PROFILE
/* Built with -DMII_PROFILE: where mii_draw's CPU time goes, in time base
 * ticks, summed over draws (zero it to start). */
typedef struct {
	uint64_t setup, pose, palette, body, head;
	uint32_t draws;
} mii_profile_ticks;
extern mii_profile_ticks mii_profile;
#endif

/* A camera for drawing the Mii straight into your frame: the projection and
 * view that frame the whole Mii (or the head, for head_only) in a viewport
 * whose width over height on the TV is `aspect`, with `margin` around it.
 * Its near and far planes fit the Mii, so all of the viewport's depth range
 * goes to it. Give it a wide range: the face's layers (mask, nose line,
 * glasses) sit just in front of the faceline, and with a sliver of the Z
 * buffer they break up on a real Wii (Dolphin hides it; HBC 1.11.1). Set
 * your viewport to that area, load `proj` with GX_PERSPECTIVE, then
 * mii_draw(model, pose, view, NULL). This is how to draw a Mii each frame:
 * no copies, no pixel-format change. */
int mii_view(const mii_model *model, const mii_pose *pose, float aspect, float margin, Mtx44 proj,
			 Mtx view);

/* How many pixels tall the Mii stands on screen, drawn with this camera
 * (`view`), placing (`place`, or NULL) and perspective projection, in a
 * viewport `viewport_h` pixels tall: what mii_body_lod_for and
 * mii_model_set_mask_size go by. Measured at the Mii's middle; a Mii at or
 * behind the camera is as near as can be. */
float mii_pixels_tall(const mii_model *model, Mtx view, Mtx place, Mtx44 proj, float viewport_h);

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
 * it, on `background` (alpha 0 for none). Same rules as mii_capture; the
 * corner is cleared to opaque black after. */
int mii_icon(const mii_model *model, int expression, int size, GXColor background, void *dst);

/* Draws a model's faces with the GPU (mii_model_opts.gpu_faces), as RFL's
 * RFLiMakeTexture: the parts as quads into the EFB's corner, coloured by
 * TEV, copied out as RGB5A3. Call it between frames, on the thread that
 * draws: after your last copy, before you draw the next frame. It leaves
 * the corner in `clear` (your clear colour), the pixel format RGB8_Z24,
 * and changes the viewport, scissor, projection, copy filter, vertex
 * format, channels, TEV, blending, Z and culling: set yours again. */
int mii_model_make_faces(mii_model *model, const mii_res *res, GXColor clear);

#endif /* MII_NO_OGC */

#ifdef __cplusplus
}
#endif

#endif
