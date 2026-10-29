/*
 * miidraw.c - drawing Miis (miidraw.h). Public domain.
 *
 * The head follows RFL, the Wii's face library, as the CC0 decompilations
 * show it (koopthekoopa/wii-ipl libs/RVLFaceLib: RFL_NANDLoader.c,
 * RFL_Model.c, RFL_MakeTex.c, RFL_Icon.c); the numbers below are RFL's,
 * the code is this library's own. The body is this library's own design.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miidraw.h"

#ifndef MII_NO_OGC
#include <malloc.h>
#include <ogc/cache.h>
/* libogc's paired-single ps_guMtxConcat loads a constant through r13 with a
 * psq_l, whose offset has 12 bits; with the stock linker scripts the offset
 * is out of range, gets cut short, and every product is wrong (README,
 * "Building"). The C one has no such load. */
#undef guMtxConcat
#define guMtxConcat c_guMtxConcat
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- RFL_Res.dat ------------------------------------------------------------ */

/* The archives, in RFL's order (RFLiArcID). */
enum {
	A_BEARD, A_EYE, A_EYEBROW, A_FACELINE, A_FACETEX, A_FOREHEAD, A_GLASS, A_GLASSTEX,
	A_HAIR, A_MASK, A_MOLE, A_MOUTH, A_MUSTACHE, A_NOSE, A_NLINE, A_NLINETEX, A_CAP,
	A_CAPTEX, A_COUNT
};

static uint32_t be32(const uint8_t *p) {
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p) {
	return (uint16_t) (p[0] << 8 | p[1]);
}

static int16_t bes16(const uint8_t *p) {
	return (int16_t) be16(p);
}

static float bef32(const uint8_t *p) {
	uint32_t u = be32(p);
	float f;

	memcpy(&f, &u, 4);
	return f;
}

int mii_res_attach(mii_res *res, const void *data, size_t size) {
	const uint8_t *d = data;
	int i;

	if (!res || !data)
		return MII_E_ARG;
	memset(res, 0, sizeof(*res));
	if (size < 4 + 4 * A_COUNT || size > 0x1000000 || be16(d) != A_COUNT || be16(d + 2) < 0x39D)
		return MII_E_FORMAT;
	for (i = 0; i < A_COUNT; ++i) {
		uint32_t at = be32(d + 4 + 4 * i), n, base, k, prev = 0;

		if (at > size - 4)
			return MII_E_FORMAT;
		n = be16(d + at);
		base = at + 4 + 4 * (n + 1);
		if (base > size)
			return MII_E_FORMAT;
		for (k = 0; k <= n; ++k) {
			uint32_t off = be32(d + at + 4 + 4 * k);

			if (off < prev || off > size - base)
				return MII_E_FORMAT;
			prev = off;
		}
		res->arc[i] = at;
		res->count[i] = (uint16_t) n;
	}
	res->data = d;
	res->size = (uint32_t) size;
	res->version = be16(d + 2);
	return MII_OK;
}

int mii_res_load(mii_res *res, const char *path) {
	FILE *f;
	long size;
	void *buf;
	int r;

	if (!res)
		return MII_E_ARG;
	memset(res, 0, sizeof(*res));
	f = fopen(path ? path : MII_RES_PATH, "rb");
	if (!f)
		return errno == ENOENT || errno == ENOTDIR ? MII_E_NO_FILE : MII_E_IO;
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 || size > 0x1000000 ||
			fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return MII_E_FORMAT;
	}
	buf = malloc(size);
	if (!buf) {
		fclose(f);
		return MII_E_MEM;
	}
	if (fread(buf, 1, size, f) != (size_t) size) {
		fclose(f);
		free(buf);
		return MII_E_IO;
	}
	fclose(f);
	r = mii_res_attach(res, buf, size);
	if (r < 0) {
		free(buf);
		return r;
	}
	res->owned = buf;
	return MII_OK;
}

void mii_res_free(mii_res *res) {
	if (!res)
		return;
	free(res->owned);
	memset(res, 0, sizeof(*res));
}

static int res_file(const mii_res *res, int arc, int idx, const uint8_t **p, uint32_t *len) {
	const uint8_t *d = res->data;
	uint32_t at = res->arc[arc], n = res->count[arc], a, b;

	if (idx < 0 || (uint32_t) idx >= n)
		return MII_E_INVALID;
	a = be32(d + at + 4 + 4 * idx);
	b = be32(d + at + 4 + 4 * (idx + 1));
	*p = d + at + 4 + 4 * (n + 1) + a;
	*len = b - a;
	return MII_OK;
}

/* ---- Textures ------------------------------------------------------------- */

static uint8_t x5(unsigned v) { return (uint8_t) (v << 3 | v >> 2); }
static uint8_t x3(unsigned v) { return (uint8_t) (v << 5 | v << 2 | v >> 1); }

/* An RFL texture (a BTI-like 0x20-byte header) as linear RGBA. Formats RFL
 * uses: I4, I8, IA4, IA8, RGB5A3. */
static int tex_decode(const uint8_t *f, uint32_t len, mii_tex *out) {
	int fmt, w, h, bw, bh, bits, bx, by, k;
	uint32_t at, image, need;
	const uint8_t *d;

	memset(out, 0, sizeof(*out));
	if (len < 0x20)
		return MII_E_FORMAT;
	fmt = f[0];
	w = be16(f + 2);
	h = be16(f + 4);
	image = be32(f + 0x1C);
	switch (fmt) {
	case 0: bw = 8; bh = 8; bits = 4; break;
	case 1: bw = 8; bh = 4; bits = 8; break;
	case 2: bw = 8; bh = 4; bits = 8; break;
	case 3: bw = 4; bh = 4; bits = 16; break;
	case 5: bw = 4; bh = 4; bits = 16; break;
	default: return MII_E_FORMAT;
	}
	if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
		return MII_E_FORMAT;
	need = (uint32_t) ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * (bw * bh * bits / 8);
	if (image > len || need > len - image)
		return MII_E_FORMAT;
	out->rgba = calloc((size_t) w * h, 4);
	if (!out->rgba)
		return MII_E_MEM;
	out->w = w;
	out->h = h;
	/* The shape textures' own wrap (RFLiInitTexRes): the glasses are one lens
	 * mirrored across, the face and nose line mirrored too. The face parts
	 * RFL clamps (RFLiInitRFLTexture), and composite() never reads past an
	 * edge. */
	out->wrap_s = f[6] <= 2 ? f[6] : 0;
	out->wrap_t = f[7] <= 2 ? f[7] : 0;
	d = f + image;
	at = 0;
	for (by = 0; by < h; by += bh)
		for (bx = 0; bx < w; bx += bw) {
			for (k = 0; k < bw * bh; ++k) {
				int x = bx + k % bw, y = by + k / bw;
				uint8_t p[4];
				unsigned v;

				switch (fmt) {
				case 0:
					v = d[at + k / 2] >> (k % 2 ? 0 : 4) & 15;
					p[0] = p[1] = p[2] = p[3] = (uint8_t) (v * 17);
					break;
				case 1:
					p[0] = p[1] = p[2] = p[3] = d[at + k];
					break;
				case 2:
					v = d[at + k];
					p[0] = p[1] = p[2] = (uint8_t) ((v & 15) * 17);
					p[3] = (uint8_t) ((v >> 4) * 17);
					break;
				case 3:
					p[0] = p[1] = p[2] = d[at + 2 * k + 1];
					p[3] = d[at + 2 * k];
					break;
				default:
					v = be16(d + at + 2 * k);
					if (v & 0x8000) {
						p[0] = x5(v >> 10 & 31);
						p[1] = x5(v >> 5 & 31);
						p[2] = x5(v & 31);
						p[3] = 255;
					} else {
						p[0] = (uint8_t) ((v >> 8 & 15) * 17);
						p[1] = (uint8_t) ((v >> 4 & 15) * 17);
						p[2] = (uint8_t) ((v & 15) * 17);
						p[3] = x3(v >> 12 & 7);
					}
				}
				if (x < w && y < h)
					memcpy(out->rgba + 4 * ((size_t) y * w + x), p, 4);
			}
			at += bw * bh * bits / 8;
		}
	return MII_OK;
}

/* ---- The cache: parts shared by content ---------------------------------- */

typedef struct share {
	struct share *next;
	mii_cache *cache;
	uint32_t hash, size[2];
	void *buf[2];
	int refs;
	/* A mesh's: its whole list's hash and corners, and its levels, so the
	 * next Mii with the same part takes them without simplifying again. */
	int mesh, count, lods;
	uint32_t whole;
	int lod_count[MII_HEAD_LODS];
	uint32_t lod_first[MII_HEAD_LODS], lod_at[MII_HEAD_LODS], lod_size[MII_HEAD_LODS];
} share;

struct mii_cache {
	share *head;
};

mii_cache *mii_cache_new(void) {
	return calloc(1, sizeof(mii_cache));
}

void mii_cache_free(mii_cache *cache) {
	share *e, *next;

	if (!cache)
		return;
	for (e = cache->head; e; e = next) {   /* all of them: the models were freed first */
		next = e->next;
		free(e->buf[0]);
		free(e->buf[1]);
		free(e);
	}
	free(cache);
}

void mii_cache_stats(const mii_cache *cache, int *entries, uint32_t *bytes, int *users) {
	const share *e;

	*entries = *users = 0;
	*bytes = 0;
	for (e = cache ? cache->head : NULL; e; e = e->next) {
		++*entries;
		*bytes += e->size[0] + e->size[1];
		*users += e->refs;
	}
}

static uint32_t hash_bytes(uint32_t h, const void *p, uint32_t n) {
	const uint8_t *b = p;

	while (n--)
		h = (h ^ *b++) * 16777619u;
	return h;
}

/* Two buffers (the second may be NULL) handed to the cache: if it holds the
 * same bytes, ours are freed and its used; else it keeps ours. The entry
 * goes in *owner; NULL cache: nothing changes. */
static int share_bufs(mii_cache *cache, void **a, uint32_t sa, void **b, uint32_t sb, void **owner) {
	uint32_t h;
	share *e;

	*owner = NULL;
	if (!cache || !*a)
		return MII_OK;
	if (!b || !*b)
		sb = 0;
	h = hash_bytes(hash_bytes(2166136261u, *a, sa), sb ? *b : NULL, sb);
	for (e = cache->head; e; e = e->next)
		if (e->hash == h && e->size[0] == sa && e->size[1] == sb && !memcmp(e->buf[0], *a, sa) &&
				(!sb || !memcmp(e->buf[1], *b, sb))) {
			free(*a);
			*a = e->buf[0];
			if (sb) {
				free(*b);
				*b = e->buf[1];
			}
			++e->refs;
			*owner = e;
			return MII_OK;
		}
	e = calloc(1, sizeof(*e));
	if (!e)
		return MII_E_MEM;
	e->cache = cache;
	e->hash = h;
	e->size[0] = sa;
	e->size[1] = sb;
	e->buf[0] = *a;
	e->buf[1] = sb ? *b : NULL;
	e->refs = 1;
	e->next = cache->head;
	cache->head = e;
	*owner = e;
	return MII_OK;
}

/* A model lets go of a shared entry: the last one frees it. */
static void unshare(void *owner) {
	share *e = owner, **p;

	if (--e->refs)
		return;
	for (p = &e->cache->head; *p; p = &(*p)->next)
		if (*p == e) {
			*p = e->next;
			break;
		}
	free(e->buf[0]);
	free(e->buf[1]);
	free(e);
}

#ifndef MII_NO_OGC
/* A texture was freed: a new one may come at its address, and the GPU's
 * texture cache still hold the old texels. The next draw invalidates it. */
static int tex_freed;
#endif

static void tex_free(mii_tex *t) {
#ifndef MII_NO_OGC
	tex_freed |= t->gx != NULL;
#endif
	if (t->shared) {
		unshare(t->shared);
#ifndef MII_NO_OGC
		t->gx = NULL;
#else
		t->rgba = NULL;
#endif
	}
	free(t->rgba);
#ifndef MII_NO_OGC
	free(t->gx);
#endif
	memset(t, 0, sizeof(*t));
}

uint32_t mii_tex_rgb5a3_size(int w, int h) {
	return (uint32_t) ((w + 3) / 4) * ((h + 3) / 4) * 32;
}

void mii_tex_rgb5a3(const mii_tex *t, uint8_t *out) {
	int tw = (t->w + 3) / 4, th = (t->h + 3) / 4, tx, ty, k;

	for (ty = 0; ty < th; ++ty)
		for (tx = 0; tx < tw; ++tx, out += 32)
			for (k = 0; k < 16; ++k) {
				int x = tx * 4 + k % 4, y = ty * 4 + k / 4;
				const uint8_t *p = x < t->w && y < t->h ? t->rgba + 4 * (y * t->w + x) : (const uint8_t *) "\0\0\0\0";
				int a = (p[3] * 7 + 127) / 255;
				uint16_t v;

				if (a == 7)   /* opaque: 5 bits a colour */
					v = (uint16_t) (0x8000 | (p[0] * 31 + 127) / 255 << 10 | (p[1] * 31 + 127) / 255 << 5 |
									(p[2] * 31 + 127) / 255);
				else
					v = (uint16_t) (a << 12 | (p[0] * 15 + 127) / 255 << 8 | (p[1] * 15 + 127) / 255 << 4 |
									(p[2] * 15 + 127) / 255);
				out[2 * k] = (uint8_t) (v >> 8);
				out[2 * k + 1] = (uint8_t) v;
			}
}

static int res_tex(const mii_res *res, int arc, int idx, mii_tex *out) {
	const uint8_t *p;
	uint32_t len;
	int r = res_file(res, arc, idx, &p, &len);

	return r < 0 ? r : tex_decode(p, len, out);
}

/* Bilinear, clamped at the edges, in [0,1] texture space; out in 0..1. */
static void sample(const mii_tex *t, float s, float u, float out[4]) {
	float x = s * t->w - 0.5f, y = u * t->h - 0.5f, fx, fy;
	int x0, y0, x1, y1, c;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > t->w - 1) x = (float) (t->w - 1);
	if (y > t->h - 1) y = (float) (t->h - 1);
	x0 = (int) x;
	y0 = (int) y;
	x1 = x0 + 1 < t->w ? x0 + 1 : x0;
	y1 = y0 + 1 < t->h ? y0 + 1 : y0;
	fx = x - x0;
	fy = y - y0;
	for (c = 0; c < 4; ++c) {
		float a = t->rgba[4 * (y0 * t->w + x0) + c], b = t->rgba[4 * (y0 * t->w + x1) + c];
		float e = t->rgba[4 * (y1 * t->w + x0) + c], g = t->rgba[4 * (y1 * t->w + x1) + c];

		out[c] = ((a + (b - a) * fx) * (1 - fy) + (e + (g - e) * fx) * fy) / 255.0f;
	}
}

/* ---- RFL's colours (RFL_Model.c, RFLi_FaceConfig/colors.h) ------------------ */

static const uint8_t skin_colors[6][3] = {
	{ 240, 216, 196 }, { 255, 188, 128 }, { 216, 136, 80 },
	{ 255, 176, 144 }, { 152, 80, 48 }, { 82, 46, 28 }
};
static const uint8_t hair_colors[8][3] = {   /* also the beard's, eyebrows' and mustache's */
	{ 30, 26, 24 }, { 56, 32, 21 }, { 85, 38, 23 }, { 112, 64, 36 },
	{ 114, 114, 120 }, { 73, 54, 26 }, { 122, 89, 40 }, { 193, 159, 100 }
};
static const uint8_t glass_colors[6][3] = {
	{ 16, 16, 16 }, { 96, 56, 16 }, { 152, 24, 16 },
	{ 32, 48, 96 }, { 144, 88, 0 }, { 96, 88, 80 }
};
static const uint8_t fav_colors[12][3] = {
	{ 184, 64, 48 }, { 240, 120, 40 }, { 248, 216, 32 }, { 128, 200, 40 },
	{ 0, 116, 40 }, { 32, 72, 152 }, { 64, 160, 216 }, { 232, 96, 120 },
	{ 112, 44, 168 }, { 72, 56, 24 }, { 224, 224, 224 }, { 24, 24, 20 }
};
static const uint8_t eye_colors[6][3] = {
	{ 0, 0, 0 }, { 124, 128, 128 }, { 112, 80, 64 },
	{ 112, 110, 64 }, { 88, 104, 184 }, { 72, 128, 104 }
};
static const uint8_t mouth_color0[3][3] = { { 190, 78, 38 }, { 216, 48, 40 }, { 207, 68, 71 } };
static const uint8_t mouth_color1[3][3] = { { 113, 42, 4 }, { 120, 21, 16 }, { 126, 37, 40 } };
static const uint8_t mole_color[3] = { 18, 15, 15 };

/* RFLi_FaceConfig/offsets.h: each eye and eyebrow type's base angle, in
 * 32nds of a turn. */
static const uint8_t eye_rot_offset[50] = {
	29, 28, 28, 28, 29, 28, 28, 28, 29, 28, 28, 28, 28, 29, 29, 28,
	28, 28, 29, 29, 28, 29, 28, 29, 29, 28, 29, 28, 28, 29, 28, 28,
	28, 29, 29, 29, 28, 28, 29, 29, 29, 28, 28, 29, 29, 29, 29, 29,
	28, 28
};
static const uint8_t brow_rot_offset[24] = {
	26, 26, 27, 25, 26, 25, 26, 25, 28, 25, 26, 24, 27, 27, 26, 26,
	25, 25, 26, 26, 27, 26, 25, 27
};

static uint32_t rgba(const uint8_t c[3]) {
	return (uint32_t) c[0] << 24 | (uint32_t) c[1] << 16 | (uint32_t) c[2] << 8 | 0xFF;
}

/* A colour as RGB5A3 keeps an opaque one: 5 bits a channel. The skin's
 * plain parts (forehead, nose) take it so, the same as the face texture
 * beside them: at full precision the darkest skin, (82, 46, 28), stood
 * redder than the face's (82, 49, 24). */
static uint32_t rgba_as5(const uint8_t c[3]) {
	uint8_t q[3];
	int k;

	for (k = 0; k < 3; ++k)
		q[k] = x5((unsigned) (c[k] * 31 + 127) / 255);
	return rgba(q);
}

static int pick(int v, int n) {
	return v < 0 ? 0 : v >= n ? n - 1 : v;
}

/* ---- The face mask (RFL_MakeTex.c) ------------------------------------------- */

enum { K_EYE, K_MOUTH, K_FLAT };
enum { O_CENTER, O_RIGHT, O_LEFT };

typedef struct {
	float x, y, w, h, angle;   /* in mask pixels; angle in degrees */
	int origin, kind;
	const mii_tex *tex;
	float c0[3], c1[3];
} face_part;

/* The face's Mii fields for one expression (RFLiMakeTexture). */
typedef struct {
	int eye_type, eye_rot, brow_rot, brow_y, mouth_type;
} expr_face;

static int clampi(int v, int lo, int hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static expr_face expression(const mii *m, int e) {
	expr_face f;
	int base = eye_rot_offset[pick(m->eye_type, 50)];

	f.eye_type = m->eye_type;
	f.eye_rot = m->eye_rotate;
	f.brow_rot = m->brow_rotate;
	f.brow_y = m->brow_y;
	f.mouth_type = m->mouth_type;
	switch (e) {
	case MII_EXPR_SMILE:
		f.eye_type = 48;
		f.eye_rot = clampi(f.eye_rot + base - eye_rot_offset[48], 0, 7);
		break;
	case MII_EXPR_ANGER:
		f.brow_rot = clampi(f.brow_rot + 2, 0, 11);
		f.eye_rot = clampi(f.eye_rot + 2, 0, 7);
		f.mouth_type = 10;
		break;
	case MII_EXPR_SORROW:
		f.brow_rot = clampi(f.brow_rot - 2, 0, 11);
		f.eye_rot = clampi(f.eye_rot - 2, 0, 7);
		f.mouth_type = 12;
		break;
	case MII_EXPR_SURPRISE:
		f.brow_y = f.brow_y - 2;
		f.eye_type = 49;
		f.eye_rot = clampi(f.eye_rot + base - eye_rot_offset[49], 0, 7);
		break;
	case MII_EXPR_BLINK:
		f.eye_type = 26;
		f.eye_rot = clampi(f.eye_rot + base - eye_rot_offset[26], 0, 7);
		break;
	case MII_EXPR_OPEN_MOUTH:
		f.mouth_type = 24;
		break;
	}
	return f;
}

static void color3(float out[3], const uint8_t c[3]) {
	out[0] = c[0] / 255.0f;
	out[1] = c[1] / 255.0f;
	out[2] = c[2] / 255.0f;
}

/* One part composited "over" what is already there, in a float RGBA buffer.
 * The quad: corners from (xo, -0.5) to (xo + 1, 0.5), scaled by w and h,
 * turned by angle, scaled by RFL's 0.8896 and 0.9277 (TRANS_X_BASE,
 * _Y_BASE), moved to (x, y), y down (RFLiDrawQuad). */
static void composite(float *buf, int size, const face_part *p) {
	static const float tx = 0.88961464f, ty = 0.9276675f;
	float a = p->angle * (float) M_PI / 180.0f, ca = cosf(a), sa = sinf(a);
	float xo = p->origin == O_CENTER ? -0.5f : p->origin == O_RIGHT ? -1.0f : 0.0f;
	float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
	int k, px, py;

	if (p->w <= 0 || p->h <= 0)
		return;
	for (k = 0; k < 4; ++k) {
		float u = (k & 1 ? 1.0f : 0.0f) + xo, v = k & 2 ? 0.5f : -0.5f;
		float lx = u * p->w, ly = v * p->h;
		float sx = (lx * ca - ly * sa) * tx + p->x, sy = (lx * sa + ly * ca) * ty + p->y;

		if (sx < minx) minx = sx;
		if (sx > maxx) maxx = sx;
		if (sy < miny) miny = sy;
		if (sy > maxy) maxy = sy;
	}
	for (py = (int) floorf(miny) < 0 ? 0 : (int) floorf(miny); py <= maxy && py < size; ++py)
		for (px = (int) floorf(minx) < 0 ? 0 : (int) floorf(minx); px <= maxx && px < size; ++px) {
			float dx = (px + 0.5f - p->x) / tx, dy = (py + 0.5f - p->y) / ty;
			float lx = dx * ca + dy * sa, ly = -dx * sa + dy * ca;
			float u = lx / p->w, v = ly / p->h, s, t, c[4], col[3], al, *o;

			if (u < xo || u > xo + 1 || v < -0.5f || v > 0.5f)
				continue;
			s = p->origin == O_LEFT ? 1.0f - u : p->origin == O_RIGHT ? u + 1.0f : u + 0.5f;
			t = v + 0.5f;
			sample(p->tex, s, t, c);
			switch (p->kind) {
			case K_EYE:     /* red: colour 0; blue: the eye's colour; green: white */
				for (k = 0; k < 3; ++k)
					col[k] = c[0] * p->c0[k] + c[2] * p->c1[k] + c[1];
				break;
			case K_MOUTH:   /* red: colour 0; green: colour 1; blue: white */
				for (k = 0; k < 3; ++k)
					col[k] = c[0] * p->c0[k] + c[1] * p->c1[k] + c[2];
				break;
			default:
				memcpy(col, p->c0, sizeof(col));
			}
			al = c[3];
			if (al <= 0)
				continue;
			o = buf + 4 * (py * size + px);
			for (k = 0; k < 3; ++k)
				o[k] = (col[k] > 1 ? 1 : col[k]) * al + o[k] * (1 - al);
			o[3] = al + o[3] * (1 - al);
		}
}

/* The face parts for one expression, in mask pixels (RFLiSetFaceParts):
 * the mole, the eyes, the eyebrows, the mouth and the mustache, back to
 * front, each pointing at its texture among those given (only their
 * pixels are read later). Returns how many. */
static int face_layout(const mii *m, int e, int size, face_part *p, const mii_tex *eye, const mii_tex *brow,
					   const mii_tex *mouth, const mii_tex *mustache, const mii_tex *mole) {
	static const float ux = 0.88961464f, uy = 1.1600001f * 0.9276675f;
	expr_face f = expression(m, e);
	float unit = size / 64.0f;
	int n = 0, i;
	float eye_x, eye_y, eye_w, eye_h, eye_a, brow_x, brow_y, brow_w, brow_h, brow_a;

	eye_x = ux * m->eye_x;
	eye_y = 18.451525f + uy * m->eye_y;
	eye_w = 342 / 64.0f * (1 + 0.4f * m->eye_scale);
	eye_h = 288 / 64.0f * (1 + 0.4f * m->eye_scale);
	eye_a = 11.25f * ((f.eye_rot + eye_rot_offset[pick(f.eye_type, 50)]) % 32);
	brow_x = ux * m->brow_x;
	brow_y = 16.549807f + uy * f.brow_y;
	brow_w = 324 / 64.0f * (1 + 0.4f * m->brow_scale);
	brow_h = 288 / 64.0f * (1 + 0.4f * m->brow_scale);
	brow_a = 11.25f * ((f.brow_rot + brow_rot_offset[pick(m->brow_type, 24)]) % 32);

	memset(p, 0, sizeof(face_part) * 8);
	if (m->mole) {
		p[n].x = (17.766165f + 2 * ux * m->mole_x) * unit;
		p[n].y = (17.95986f + uy * m->mole_y) * unit;
		p[n].w = p[n].h = (1 + 0.4f * m->mole_scale) * unit;
		p[n].origin = O_CENTER;
		p[n].kind = K_FLAT;
		p[n].tex = mole;
		color3(p[n].c0, mole_color);
		++n;
	}
	for (i = 0; i < 2; ++i, ++n) {   /* the eyes: right, then left (mirrored) */
		p[n].x = (32 + (i ? eye_x : -eye_x)) * unit;
		p[n].y = eye_y * unit;
		p[n].w = eye_w * unit;
		p[n].h = eye_h * unit;
		p[n].angle = i ? 360 - eye_a : eye_a;
		p[n].origin = i ? O_LEFT : O_RIGHT;
		p[n].kind = K_EYE;
		p[n].tex = eye;
		if (f.eye_type == 9) {
			p[n].c0[0] = 1; p[n].c0[1] = 130 / 255.0f;
		} else if (f.eye_type == 20) {
			p[n].c0[1] = 1; p[n].c0[2] = 1;
		}
		color3(p[n].c1, eye_colors[pick(m->eye_color, 6)]);
	}
	for (i = 0; i < 2; ++i, ++n) {
		p[n].x = (32 + (i ? brow_x : -brow_x)) * unit;
		p[n].y = brow_y * unit;
		p[n].w = brow_w * unit;
		p[n].h = brow_h * unit;
		p[n].angle = i ? 360 - brow_a : brow_a;
		p[n].origin = i ? O_LEFT : O_RIGHT;
		p[n].kind = K_FLAT;
		p[n].tex = brow;
		color3(p[n].c0, hair_colors[pick(m->brow_color, 8)]);
	}
	p[n].x = 32 * unit;
	p[n].y = (29.25885f + uy * m->mouth_y) * unit;
	p[n].w = 396 / 64.0f * (1 + 0.4f * m->mouth_scale) * unit;
	p[n].h = 288 / 64.0f * (1 + 0.4f * m->mouth_scale) * unit;
	p[n].origin = O_CENTER;
	p[n].kind = K_MOUTH;
	p[n].tex = mouth;
	color3(p[n].c0, mouth_color0[pick(m->mouth_color, 3)]);
	color3(p[n].c1, mouth_color1[pick(m->mouth_color, 3)]);
	++n;
	if (m->mustache)
		for (i = 0; i < 2; ++i, ++n) {
			p[n].x = 32 * unit;
			p[n].y = (31.763554f + uy * m->beard_y) * unit;
			p[n].w = 288 / 64.0f * (1 + 0.4f * m->beard_scale) * unit;
			p[n].h = 576 / 64.0f * (1 + 0.4f * m->beard_scale) * unit;
			p[n].origin = i ? O_LEFT : O_RIGHT;
			p[n].kind = K_FLAT;
			p[n].tex = mustache;
			color3(p[n].c0, hair_colors[pick(m->beard_color, 8)]);
		}
	return n;
}

/* The mask texture for one expression on the CPU (RFLiSetFaceParts,
 * RFLiCapture). RFL draws front to back (mustache, mouth, eyebrows, eyes,
 * mole) with the destination alpha as coverage; this composites the same
 * parts back to front, which comes to the same. */
static int make_mask(const mii_res *res, const mii *m, int e, int size, mii_tex *out) {
	expr_face f = expression(m, e);
	float *buf;
	mii_tex eye, brow, mouth, mustache, mole;
	face_part p[8];
	int n, i, r;

	memset(out, 0, sizeof(*out));
	memset(&eye, 0, sizeof(eye));
	memset(&brow, 0, sizeof(brow));
	memset(&mouth, 0, sizeof(mouth));
	memset(&mustache, 0, sizeof(mustache));
	memset(&mole, 0, sizeof(mole));
	if ((r = res_tex(res, A_EYE, f.eye_type, &eye)) < 0 ||
			(r = res_tex(res, A_EYEBROW, m->brow_type, &brow)) < 0 ||
			(r = res_tex(res, A_MOUTH, f.mouth_type, &mouth)) < 0 ||
			(m->mustache && (r = res_tex(res, A_MUSTACHE, m->mustache, &mustache)) < 0) ||
			(m->mole && (r = res_tex(res, A_MOLE, m->mole, &mole)) < 0))
		goto done;
	buf = calloc((size_t) size * size, 4 * sizeof(float));
	out->rgba = malloc((size_t) size * size * 4);
	if (!buf || !out->rgba) {
		free(buf);
		r = MII_E_MEM;
		goto done;
	}
	out->w = out->h = size;
	n = face_layout(m, e, size, p, &eye, &brow, &mouth, &mustache, &mole);
	for (i = 0; i < n; ++i)
		composite(buf, size, &p[i]);
	for (i = 0; i < size * size * 4; ++i) {
		float v = buf[i] * 255.0f + 0.5f;

		out->rgba[i] = (uint8_t) (v > 255 ? 255 : v < 0 ? 0 : v);
	}
	free(buf);
	r = MII_OK;
done:
	tex_free(&eye);
	tex_free(&brow);
	tex_free(&mouth);
	tex_free(&mustache);
	tex_free(&mole);
	if (r < 0)
		tex_free(out);
	return r;
}

/* ---- Shapes (RFL_Model.c, RFLiInitShapeRes) ------------------------------------ */

typedef struct {
	float x, y, z;
} vec3;

/* Reads one shape into a triangle list, scaled, moved and maybe mirrored as
 * RFL places it. anchors (faceline only): the nose, beard and hair. */
static int shape(const mii_res *res, int arc, int idx, const char *tag, int texcoords, float scale,
				 vec3 at, int flip, mii_mesh *mesh, vec3 anchors[3]) {
	const uint8_t *f, *end, *pos, *nrm, *txc = NULL;
	uint32_t len;
	unsigned npos, nnrm, ntxc = 0, nprim, i, j, per = texcoords ? 3 : 2, total = 0;
	const uint8_t *prims;
	int r = res_file(res, arc, idx, &f, &len);
	float *v;

	memset(mesh, 0, sizeof(*mesh));
	mesh->tex = -1;
	if (r < 0)
		return r;
	end = f + len;
	if (len < 6 || memcmp(f, tag, 4))
		return MII_E_FORMAT;
	f += 4;
	if (anchors) {
		if (end - f < 36)
			return MII_E_FORMAT;
		for (i = 0; i < 3; ++i) {
			anchors[i].x = bef32(f + 12 * i);
			anchors[i].y = bef32(f + 12 * i + 4);
			anchors[i].z = bef32(f + 12 * i + 8);
		}
		f += 36;
	}
	if (end - f < 2)
		return MII_E_FORMAT;
	npos = be16(f);
	if (!npos)
		return MII_OK;   /* an empty part: no beard, no cap */
	pos = f + 2;
	f = pos + 6 * npos;
	if (end - f < 2)
		return MII_E_FORMAT;
	nnrm = be16(f);
	nrm = f + 2;
	f = nrm + 6 * nnrm;
	if (texcoords) {
		if (end - f < 2)
			return MII_E_FORMAT;
		ntxc = be16(f);
		txc = f + 2;
		f = txc + 4 * ntxc;
	}
	if (end - f < 1)
		return MII_E_FORMAT;
	nprim = *f++;
	prims = f;
	/* First pass: check the primitives and count triangle vertices. */
	for (i = 0; i < nprim; ++i) {
		unsigned n, kind;

		if (end - f < 2)
			return MII_E_FORMAT;
		n = f[0];
		kind = f[1];
		f += 2;
		if ((unsigned) (end - f) < n * per)
			return MII_E_FORMAT;
		for (j = 0; j < n; ++j)
			if (f[per * j] >= npos || f[per * j + 1] >= nnrm || (texcoords && f[per * j + 2] >= ntxc))
				return MII_E_FORMAT;
		f += n * per;
		if (n < 3)
			continue;
		switch (kind) {
		case 0x80: total += n / 4 * 6; break;           /* quads */
		case 0x90: total += n / 3 * 3; break;           /* triangles */
		case 0x98: case 0xA0: total += (n - 2) * 3; break;  /* strip, fan */
		default: return MII_E_FORMAT;
		}
	}
	mesh->v = malloc(sizeof(float) * 8 * (total ? total : 1));
	if (!mesh->v)
		return MII_E_MEM;
	v = mesh->v;
	f = prims;
	for (i = 0; i < nprim; ++i) {
		unsigned n = f[0], kind = f[1], t, tri[3];
		const uint8_t *vx = f + 2;

		f += 2 + n * per;
		if (n < 3)
			continue;
		for (t = 0; ; ++t) {
			unsigned k;

			switch (kind) {
			case 0x80:
				if (t >= n / 4 * 2)
					goto next;
				tri[0] = t / 2 * 4;
				tri[1] = t / 2 * 4 + (t % 2 ? 2 : 1);
				tri[2] = t / 2 * 4 + (t % 2 ? 3 : 2);
				break;
			case 0x90:
				if (t >= n / 3)
					goto next;
				tri[0] = 3 * t; tri[1] = 3 * t + 1; tri[2] = 3 * t + 2;
				break;
			case 0x98:
				if (t >= n - 2)
					goto next;
				tri[0] = t % 2 ? t + 1 : t;
				tri[1] = t % 2 ? t : t + 1;
				tri[2] = t + 2;
				break;
			default:
				if (t >= n - 2)
					goto next;
				tri[0] = 0; tri[1] = t + 1; tri[2] = t + 2;
			}
			if (flip) {   /* mirrored: keep the winding the camera sees */
				unsigned s = tri[1];

				tri[1] = tri[2];
				tri[2] = s;
			}
			for (k = 0; k < 3; ++k) {
				const uint8_t *e = vx + per * tri[k];
				const uint8_t *pp = pos + 6 * e[0], *nn = nrm + 6 * e[1];
				float x = bes16(pp) / 256.0f, nx = bes16(nn) / 16384.0f;

				*v++ = at.x + (flip ? -x : x) * scale;
				*v++ = at.y + bes16(pp + 2) / 256.0f * scale;
				*v++ = at.z + bes16(pp + 4) / 256.0f * scale;
				*v++ = flip ? -nx : nx;
				*v++ = bes16(nn + 2) / 16384.0f;
				*v++ = bes16(nn + 4) / 16384.0f;
				if (texcoords) {
					const uint8_t *tt = txc + 4 * e[2];

					*v++ = bes16(tt) / 8192.0f;
					*v++ = bes16(tt + 2) / 8192.0f;
				} else {
					*v++ = 0;
					*v++ = 0;
				}
			}
		}
next:;
	}
	mesh->count = (int) ((v - mesh->v) / 8);
	return MII_OK;
}

/* ---- The body: this library's own ------------------------------------------- */

/* A surface of revolution about Y: profile points (y, r) from bottom to
 * top; its cross-section an ellipse, z = zscale * x. */
static int lathe(mii_mesh *mesh, const float (*prof)[2], int n, int seg, float zscale, uint32_t color) {
	float *v;
	int i, j, k;

	memset(mesh, 0, sizeof(*mesh));
	mesh->tex = -1;
	mesh->color = color;
	mesh->v = malloc(sizeof(float) * 8 * 6 * seg * (n - 1));
	if (!mesh->v)
		return MII_E_MEM;
	v = mesh->v;
	for (i = 0; i + 1 < n; ++i)
		for (j = 0; j < seg; ++j) {
			/* two triangles: (i, j) (i, j+1) (i+1, j+1) and (i, j) (i+1, j+1) (i+1, j) */
			static const int quad[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };

			for (k = 0; k < 6; ++k) {
				int ring = i + quad[k][1], s = j + quad[k][0];
				float th = 2 * (float) M_PI * s / seg, ct = cosf(th), st = sinf(th);
				int lo = ring > 0 ? ring - 1 : ring, hi = ring + 1 < n ? ring + 1 : ring;
				float dy = prof[hi][0] - prof[lo][0], dr = prof[hi][1] - prof[lo][1];
				float nr = dy, ny = -dr, l, nx, nz;

				*v++ = prof[ring][1] * ct;
				*v++ = prof[ring][0];
				*v++ = prof[ring][1] * st * zscale;
				nx = nr * ct;
				nz = nr * st / zscale;
				l = sqrtf(nx * nx + ny * ny + nz * nz);
				if (l < 1e-6f)
					l = 1;
				*v++ = nx / l;
				*v++ = ny / l;
				*v++ = nz / l;
				*v++ = 0;
				*v++ = 0;
			}
		}
	mesh->count = 6 * seg * (n - 1);
	return MII_OK;
}

/* An ellipsoid's profile: centre y0, half-height ry, radius rx, n points. */
static void ellipse_profile(float (*prof)[2], int n, float y0, float ry, float rx) {
	int i;

	for (i = 0; i < n; ++i) {
		float a = (float) M_PI * (-0.5f + (float) i / (n - 1));

		prof[i][0] = y0 + ry * sinf(a);
		prof[i][1] = rx * cosf(a);
	}
}

/* Body proportions, in head units, at the middle height and build, after
 * the Mii Channel's plaza: below the chin the body is about 2.3 heads; the
 * shirt is a near-straight column from the shoulders to the waist, about
 * 0.8 of the head's width; the trousers start at that same width; the legs
 * are as long as the torso; thin arms reach mid-thigh; small round hands
 * and feet. Feet on Y = 0. */
#define BODY_WAIST_Y 94.0f        /* where shirt and trousers meet */
#define BODY_WAIST_R 18.5f        /* the waist's half width, shirt and trousers alike */
#define BODY_TORSO_TOP 158.0f
#define BODY_CROTCH_Y 80.0f
#define BODY_LEG_X 9.6f
#define BODY_ARM_Y 150.0f
#define BODY_ARM_LEN 72.0f

static int make_body(mii_model *model) {
	const mii *m = &model->m;
	int female = m->sex == MII_FEMALE;
	float prof[12][2];
	uint32_t shirt = rgba(fav_colors[pick(m->fav_color, 12)]);
	uint32_t skin = rgba(skin_colors[pick(m->face_color, 6)]);
	/* Trousers: grey, gold for a special Mii (an ID without MII_ID_NORMAL),
	 * as the Wii. A Mii with no ID yet is an ordinary one. */
	uint32_t pants = !mii_id_empty(m->id) && !(mii_id_flags(m->id) & MII_ID_NORMAL) ?
					 0xC09838FF : 0x5c5c66FF;
	uint32_t shoe = 0x34343aFF;
	float h = m->height, b = m->build, sx, sy;
	/* Women: shoulders a little narrower, legs and arms a little slimmer. */
	float sh = female ? 18.2f : 20.0f, leg_r = female ? 6.4f : 7.0f, arm_r = female ? 3.9f : 4.3f;
	int r;

	model->shirt = shirt;
	model->pants = pants;

	/* Width and height from the Mii's height and build, as later Mii
	 * libraries scale bodies, over their value at the middle (64, 64). */
	sx = (b * (h * 0.47f / 128 + 0.4f)) / 128 + h * 0.23f / 128 + 0.4f;
	sy = h * 0.77f / 128 + 0.5f;
	/* The width's range is narrowed (x^0.6: 0.64 to 1.42 of the middle):
	 * the formula's own made a tiny Mii a stick and a large one a box. */
	model->body_scale[0] = powf(sx / 0.8325f, 0.6f);
	model->body_scale[1] = sy / 0.885f;

	{   /* the shirt: waist to shoulders, its bottom inside the trousers' top */
		const float t[][2] = {
			{ BODY_WAIST_Y - 2, 0 }, { BODY_WAIST_Y - 2, BODY_WAIST_R },
			{ 110, BODY_WAIST_R + 0.3f }, { 128, (BODY_WAIST_R + sh) / 2 + 0.3f },
			{ 146, sh }, { 152, sh - 0.6f }, { 155.5f, sh - 2.6f }, { 157.5f, sh - 7 },
			{ BODY_TORSO_TOP, 0 }
		};
		if ((r = lathe(&model->part[MII_BODY_TORSO], t, 9, 22, 0.66f, shirt)) < 0)
			return r;
	}
	{   /* the trousers' top: the waist, as wide as the shirt, down to the legs */
		const float t[][2] = {
			{ BODY_CROTCH_Y - 4, 0 }, { BODY_CROTCH_Y - 4, BODY_LEG_X + leg_r - 1.5f },
			{ BODY_CROTCH_Y, BODY_LEG_X + leg_r - 0.4f }, { BODY_CROTCH_Y + 6, BODY_WAIST_R - 0.6f },
			{ BODY_WAIST_Y, BODY_WAIST_R }, { BODY_WAIST_Y + 1, BODY_WAIST_R - 0.2f },
			{ BODY_WAIST_Y + 1.2f, 0 }
		};
		if ((r = lathe(&model->part[MII_BODY_HIPS], t, 7, 22, 0.66f, pants)) < 0)
			return r;
	}
	{   /* one leg, from the crotch down to the ankle, at the origin */
		const float t[][2] = {
			{ -BODY_CROTCH_Y + 3, 0 }, { -BODY_CROTCH_Y + 3, leg_r - 0.6f },
			{ -BODY_CROTCH_Y + 6, leg_r }, { -2, leg_r + 0.2f }, { 2, 0 }
		};
		if ((r = lathe(&model->part[MII_BODY_LEG], t, 5, 14, 0.95f, pants)) < 0)
			return r;
	}
	ellipse_profile(prof, 8, 0, 3.4f, leg_r);   /* a shoe: small, a little long */
	if ((r = lathe(&model->part[MII_BODY_SHOE], (const float (*)[2]) prof, 8, 14, 1.45f, shoe)) < 0)
		return r;
	{   /* one arm, hanging from the shoulder at the origin */
		const float t[][2] = {
			{ -BODY_ARM_LEN, 0 }, { -BODY_ARM_LEN, arm_r - 0.6f }, { -BODY_ARM_LEN + 2, arm_r - 0.3f },
			{ -20, arm_r }, { -3, arm_r + 0.4f }, { 1.5f, arm_r }, { 4, 0 }
		};
		if ((r = lathe(&model->part[MII_BODY_ARM], t, 7, 14, 1.0f, shirt)) < 0)
			return r;
	}
	ellipse_profile(prof, 9, 0, arm_r + 1.8f, arm_r + 1.8f);   /* a hand: a small ball */
	if ((r = lathe(&model->part[MII_BODY_HAND], (const float (*)[2]) prof, 9, 14, 0.9f, skin)) < 0)
		return r;
	{
		static const float t[][2] = { { 0, 6.0f }, { 14, 5.6f } };
		if ((r = lathe(&model->part[MII_BODY_NECK], t, 2, 12, 1.0f, skin)) < 0)
			return r;
	}
	model->head_y = (BODY_TORSO_TOP + 2) * model->body_scale[1];
	model->shoulder[0] = (sh + arm_r + 0.8f) * model->body_scale[0];
	model->shoulder[1] = BODY_ARM_Y * model->body_scale[1];
	model->shoulder[2] = 0;
	model->hip[0] = BODY_LEG_X * model->body_scale[0];
	model->hip[1] = BODY_CROTCH_Y * model->body_scale[1];
	model->hip[2] = 0;
	model->arm_len = BODY_ARM_LEN;
	model->shoe[0] = 3.0f;
	model->shoe[1] = 2.5f;
	return MII_OK;
}

/* ---- A Mii, built ------------------------------------------------------------ */

/* Bakes the parts RFL colours with TEV tricks into plain RGBA: the face
 * (skin under make-up, swap table R A B G: out = skin (1 - g) + (r, a, b) g),
 * the cap (favourite colour (1 + i) / 2), the nose line (black, alpha i) and
 * the glasses (their colour times i, alpha a). */
static void bake(mii_tex *t, int kind, const uint8_t c[3]) {
	int i;

	for (i = 0; i < t->w * t->h; ++i) {
		uint8_t *p = t->rgba + 4 * i, r = p[0], g = p[1], b = p[2], a = p[3];
		int k;

		switch (kind) {
		case MII_TEX_FACE: {
			uint8_t src[3] = { r, a, b };

			for (k = 0; k < 3; ++k)
				p[k] = (uint8_t) ((c[k] * (255 - g) + src[k] * g) / 255);
			p[3] = 255;
			break;
		}
		case MII_TEX_CAP:
			for (k = 0; k < 3; ++k)
				p[k] = (uint8_t) (c[k] * (255 + r) / 510);
			p[3] = 255;
			break;
		case MII_TEX_NOSELINE:
			p[0] = p[1] = p[2] = 0;
			p[3] = r;
			break;
		default:
			for (k = 0; k < 3; ++k)
				p[k] = (uint8_t) (c[k] * r / 255);
			break;
		}
	}
}

/* The fraction bits that fit the largest magnitude in a signed 16 bits. */
static int frac_for(float most) {
	int f = 14;

	while (f > 0 && most * (float) (1 << f) > 32767)
		--f;
	return f;
}

static int16_t fixed16(float v, int frac) {
	float q = v * (float) (1 << frac);

	return (int16_t) (q > 32767 ? 32767 : q < -32768 ? -32768 : (q < 0 ? q - 0.5f : q + 0.5f));
}

/* The mesh's distinct vertices, packed (mii_mesh.pack), and each corner's
 * index: a head's 1,438 corners have about 330 distinct vertices. A hash of
 * each vertex's 32 bytes finds the repeats. */
static int pack_mesh(mii_mesh *mesh) {
	int n = mesh->count, cap = 64, i, k, nv = 0, *slot, *first;
	float pmost = 0, smost = 0;
	uint32_t at;
	int16_t *pos, *st;
	int8_t *nrm;

	if (!n)
		return MII_OK;
	while (cap < 2 * n)
		cap *= 2;
	slot = malloc(sizeof(int) * cap);
	first = malloc(sizeof(int) * n);   /* each distinct vertex's first corner */
	mesh->idx = malloc(sizeof(uint16_t) * n);
	if (!slot || !first || !mesh->idx) {
		free(slot);
		free(first);
		return MII_E_MEM;
	}
	for (i = 0; i < cap; ++i)
		slot[i] = -1;
	for (i = 0; i < n; ++i) {
		const uint8_t *b = (const uint8_t *) (mesh->v + 8 * i);
		uint32_t h = 2166136261u;

		for (k = 0; k < 32; ++k)
			h = (h ^ b[k]) * 16777619u;
		for (h &= cap - 1; slot[h] >= 0; h = (h + 1) & (cap - 1))
			if (!memcmp(mesh->v + 8 * first[slot[h]], b, 32))
				break;
		if (slot[h] < 0) {
			if (nv >= 65535) {
				free(slot);
				free(first);
				return MII_E_FORMAT;
			}
			slot[h] = nv;
			first[nv++] = i;
		}
		mesh->idx[i] = (uint16_t) slot[h];
	}
	free(slot);
	for (i = 0; i < nv; ++i) {
		const float *v = mesh->v + 8 * first[i];

		for (k = 0; k < 3; ++k)
			if (fabsf(v[k]) > pmost)
				pmost = fabsf(v[k]);
		for (k = 6; k < 8; ++k)
			if (fabsf(v[k]) > smost)
				smost = fabsf(v[k]);
	}
	mesh->pos_frac = frac_for(pmost);
	mesh->st_frac = frac_for(smost);
	mesh->pack_nrm = ((uint32_t) nv * 6 + 31) & ~31u;
	mesh->pack_st = mesh->pack_nrm + (((uint32_t) nv * 3 + 31) & ~31u);
	at = mesh->pack_st + (((uint32_t) nv * 4 + 31) & ~31u);
#ifndef MII_NO_OGC
	mesh->pack = memalign(32, at);
#else
	mesh->pack = malloc(at);
#endif
	if (!mesh->pack) {
		free(first);
		return MII_E_MEM;
	}
	memset(mesh->pack, 0, at);
	pos = (int16_t *) mesh->pack;
	nrm = (int8_t *) (mesh->pack + mesh->pack_nrm);
	st = (int16_t *) (mesh->pack + mesh->pack_st);
	for (i = 0; i < nv; ++i) {
		const float *v = mesh->v + 8 * first[i];

		for (k = 0; k < 3; ++k) {
			float c = v[3 + k] * 64;

			pos[3 * i + k] = fixed16(v[k], mesh->pos_frac);
			nrm[3 * i + k] = (int8_t) (c > 127 ? 127 : c < -128 ? -128 : (c < 0 ? c - 0.5f : c + 0.5f));
		}
		st[2 * i] = fixed16(v[6], mesh->st_frac);
		st[2 * i + 1] = fixed16(v[7], mesh->st_frac);
	}
	free(first);
	mesh->nverts = nv;
	return MII_OK;
}

/* ---- Strips -------------------------------------------------------------------- */

/* Is (a, b, c) the triangle t, the same way round? */
static int same_turn(const uint16_t *t, int a, int b, int c) {
	return (t[0] == a && t[1] == b && t[2] == c) || (t[1] == a && t[2] == b && t[0] == c) ||
		   (t[2] == a && t[0] == b && t[1] == c);
}

int mii_strips(const uint16_t *tri, int ntri, uint16_t *out, uint16_t *lens) {
	int *first = NULL, *next = NULL, i, k, nv = 0, nstrips = 0, at = 0, left = ntri;
	uint8_t *used = calloc(ntri ? ntri : 1, 1);

	for (i = 0; i < 3 * ntri; ++i)
		if (tri[i] >= nv)
			nv = tri[i] + 1;
	/* Each vertex's triangles: first[v], then next[] through them. */
	first = malloc(sizeof(int) * (nv ? nv : 1));
	next = malloc(sizeof(int) * 3 * (ntri ? ntri : 1));
	if (!used || !first || !next) {
		free(used);
		free(first);
		free(next);
		return -1;
	}
	for (i = 0; i < nv; ++i)
		first[i] = -1;
	for (i = 0; i < 3 * ntri; ++i) {   /* corner i of triangle i / 3 */
		next[i] = first[tri[i]];
		first[tri[i]] = i;
	}
	while (left > 0) {
		int start = -1, fewest = 4, len, a, b;

		/* A free triangle with the fewest free neighbours (a strip from an
		 * edge of the mesh leaves fewer islands). */
		for (i = 0; i < ntri && fewest > 0; ++i) {
			int n = 0, e;

			if (used[i])
				continue;
			for (e = 0; e < 3; ++e) {
				int u = tri[3 * i + e], v = tri[3 * i + (e + 1) % 3], c;

				for (c = first[u]; c >= 0; c = next[c]) {
					int o = c / 3;

					if (o != i && !used[o] && (tri[3 * o] == v || tri[3 * o + 1] == v || tri[3 * o + 2] == v)) {
						++n;
						break;
					}
				}
			}
			if (n < fewest) {
				fewest = n;
				start = i;
			}
		}
		used[start] = 1;
		--left;
		out[at] = tri[3 * start];
		out[at + 1] = tri[3 * start + 1];
		out[at + 2] = tri[3 * start + 2];
		len = 3;
		/* On across the last edge: the k-th triangle of a strip is (s[k],
		 * s[k+1], s[k+2]), every other one turned (s[k+1], s[k], s[k+2]). */
		for (;;) {
			int found = -1, x = -1, c;

			a = out[at + len - 2];
			b = out[at + len - 1];
			for (c = first[a]; c >= 0 && found < 0; c = next[c]) {
				const uint16_t *t = tri + 3 * (c / 3);
				int third;

				if (used[c / 3])
					continue;
				for (k = 0; k < 3; ++k)
					if (t[k] != a && t[k] != b)
						break;
				if (k == 3)
					continue;
				third = t[k];
				if (!(t[0] == b || t[1] == b || t[2] == b))
					continue;
				/* triangle number len - 2 of the strip: even (a, b, x), odd (b, a, x) */
				if ((len - 2) % 2 == 0 ? same_turn(t, a, b, third) : same_turn(t, b, a, third)) {
					found = c / 3;
					x = third;
				}
			}
			if (found < 0 || len == 0xFFFF)   /* a GX count is 16 bits */
				break;
			used[found] = 1;
			--left;
			out[at + len++] = (uint16_t) x;
		}
		lens[nstrips++] = (uint16_t) len;
		at += len;
	}
	free(used);
	free(first);
	free(next);
	return nstrips;
}

/* ---- Coarser heads ------------------------------------------------------------ */

/* How far each coarser level may change a head part, in head units: half
 * a pixel at a Mii 150, 90, 50 and 25 pixels tall (about 170 units). The
 * levels are drawn smaller than that (mii_body_lod_for: from 90, 39, 30.5
 * and 25 pixels), so each is a little finer than it need be. */
static const float head_tol[MII_HEAD_LODS] = { 0.57f, 0.94f, 1.7f, 3.4f };

static void quad_add_plane(double *q, const double n[3], double d, double w) {
	q[0] += w * n[0] * n[0]; q[1] += w * n[0] * n[1]; q[2] += w * n[0] * n[2]; q[3] += w * n[0] * d;
	q[4] += w * n[1] * n[1]; q[5] += w * n[1] * n[2]; q[6] += w * n[1] * d;
	q[7] += w * n[2] * n[2]; q[8] += w * n[2] * d;
	q[9] += w * d * d;
}

static double quad_sum_at(const double *a, const double *b, const float *p) {
	double c[10], x = p[0], y = p[1], z = p[2];
	int k;

	for (k = 0; k < 10; ++k)
		c[k] = a[k] + b[k];
	return c[0] * x * x + 2 * c[1] * x * y + 2 * c[2] * x * z + 2 * c[3] * x + c[4] * y * y + 2 * c[5] * y * z +
		   2 * c[6] * y + c[7] * z * z + 2 * c[8] * z + c[9];
}

static void cross3(const float *a, const float *b, const float *c, double n[3]) {
	double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };

	n[0] = u[1] * v[2] - u[2] * v[1];
	n[1] = u[2] * v[0] - u[0] * v[2];
	n[2] = u[0] * v[1] - u[1] * v[0];
}

/* How far the texture moves where u was, if u folds into v: each of u's
 * triangles that lives on, its corner at u taking v's vertex nearest in
 * texture coordinate, interpolates a coordinate at u's old place; the
 * triangle that holds that place says how far it is from u's own. A flat,
 * evenly mapped part (the face's mask) folds without the eyes moving; one
 * whose mapping bends does not. With no triangle holding the place, the
 * farthest a corner's coordinate jumps. */
static double tex_shift(const int *t, int ntri, const uint8_t *dead, const int *node, const float *pos,
						const float *uv, const int *vfirst, const int *vlist, const int *cfirst, const int *clist,
						int u, int v) {
	double best = 1e30, jump = 0;
	int k, x, j;

	(void) ntri;
	for (k = cfirst[u]; k < cfirst[u + 1]; ++k) {
		int c = clist[k], tri = c / 3, h = c % 3, has_v = 0, r = -1;
		float bd = 1e30f;
		const float *p[3], *q[3];
		double e1[3], e2[3], w[3], d00 = 0, d01 = 0, d11 = 0, d20 = 0, d21 = 0, den, b1, b2, iu, iv;

		if (dead[tri])
			continue;
		for (x = vfirst[v]; x < vfirst[v + 1]; ++x) {   /* v's vertex nearest this corner's coordinate */
			float du = uv[2 * vlist[x]] - uv[2 * t[c]], dv = uv[2 * vlist[x] + 1] - uv[2 * t[c] + 1];

			if (du * du + dv * dv < bd) {
				bd = du * du + dv * dv;
				r = vlist[x];
			}
		}
		if (sqrt(bd) > jump)
			jump = sqrt(bd);
		for (j = 0; j < 3; ++j)
			has_v |= node[t[3 * tri + j]] == v;
		if (has_v || r < 0)
			continue;   /* it goes with the fold */
		for (j = 0; j < 3; ++j) {
			p[j] = j == h ? pos + 3 * v : pos + 3 * node[t[3 * tri + j]];
			q[j] = j == h ? uv + 2 * r : uv + 2 * t[3 * tri + j];
		}
		for (j = 0; j < 3; ++j) {   /* u's place in the new triangle (barycentric, on its plane) */
			e1[j] = p[1][j] - p[0][j];
			e2[j] = p[2][j] - p[0][j];
			w[j] = pos[3 * u + j] - p[0][j];
			d00 += e1[j] * e1[j];
			d01 += e1[j] * e2[j];
			d11 += e2[j] * e2[j];
			d20 += w[j] * e1[j];
			d21 += w[j] * e2[j];
		}
		den = d00 * d11 - d01 * d01;
		if (den <= 1e-12)
			continue;
		b1 = (d11 * d20 - d01 * d21) / den;
		b2 = (d00 * d21 - d01 * d20) / den;
		if (b1 < -0.05 || b2 < -0.05 || b1 + b2 > 1.05)
			continue;   /* not here */
		iu = q[0][0] + b1 * (q[1][0] - q[0][0]) + b2 * (q[2][0] - q[0][0]);
		iv = q[0][1] + b1 * (q[1][1] - q[0][1]) + b2 * (q[2][1] - q[0][1]);
		iu -= uv[2 * t[c]];
		iv -= uv[2 * t[c] + 1];
		iu = sqrt(iu * iu + iv * iv);
		if (iu < best)
			best = iu;
	}
	return best < 1e29 ? best : jump;
}

/* A packed mesh's triangles (in, ntri) into out, simpler while a fold
 * changes it by no more than `tol` head units: each step folds a node (the
 * vertices at one position) into a neighbour, the fold that changes it
 * least. A fold's change is the larger of how far it moves the surface
 * (its quadric error over the area behind it: a distance) and, on a
 * textured part, how far it moves the texture (so the eyes and mouth stay
 * put). Open edges (a faceline's outline) weigh heavily; no triangle turns
 * more than 60 degrees. A corner that moves takes the vertex at its new
 * node nearest in texture coordinate and normal. Returns the triangles
 * left. The levels of a part so depend on the part alone: a flat one (the
 * face's mask) folds far, the outline and the curves keep theirs. */
static int simplify(const mii_mesh *m, int textured, const uint16_t *in, int ntri, double tol, uint16_t *out) {
	int nv = m->nverts, i, j, e, alive = ntri, tries = 0;
	const int16_t *pp = (const int16_t *) m->pack, *st = (const int16_t *) (m->pack + m->pack_st);
	const int8_t *nr = (const int8_t *) (m->pack + m->pack_nrm);
	float *pos = malloc(sizeof(float) * 3 * nv), *uv = malloc(sizeof(float) * 2 * nv);
	int *node = malloc(sizeof(int) * nv), *t = malloc(sizeof(int) * 3 * ntri);
	/* Each node's vertices, and (each step) its live corners: vfirst and
	 * cfirst index vlist and clist by node. */
	int *vfirst = malloc(sizeof(int) * (nv + 1)), *vlist = malloc(sizeof(int) * nv);
	int *cfirst = malloc(sizeof(int) * (nv + 1)), *clist = malloc(sizeof(int) * 3 * ntri);
	double *q = calloc((size_t) nv * 10, sizeof(double)), *area = calloc(nv ? nv : 1, sizeof(double));
	uint8_t *dead = calloc(ntri ? ntri : 1, 1);
	float scale = 1;

	if (!pos || !uv || !node || !t || !q || !dead || !vfirst || !vlist || !cfirst || !clist || !area) {
		alive = -1;
		goto done;
	}
	for (i = 0; i < nv; ++i) {
		for (j = 0; j < 3; ++j)
			pos[3 * i + j] = pp[3 * i + j] / (float) (1 << m->pos_frac);
		uv[2 * i] = st[2 * i] / (float) (1 << m->st_frac);
		uv[2 * i + 1] = st[2 * i + 1] / (float) (1 << m->st_frac);
		node[i] = i;
		for (j = 0; j < i; ++j)   /* the first vertex at this position names the node */
			if (node[j] == j && !memcmp(pp + 3 * j, pp + 3 * i, 6)) {
				node[i] = j;
				break;
			}
		for (j = 0; j < 3; ++j)
			if (fabsf(pos[3 * i + j]) > scale)
				scale = fabsf(pos[3 * i + j]);
	}
	for (i = 0; i < 3 * ntri; ++i)
		t[i] = in[i];
	memset(vfirst, 0, sizeof(int) * (nv + 1));   /* the vertices by node (a counting sort) */
	for (i = 0; i < nv; ++i)
		++vfirst[node[i] + 1];
	for (i = 0; i < nv; ++i)
		vfirst[i + 1] += vfirst[i];
	{
		int *fill = cfirst;   /* scratch here */

		memcpy(fill, vfirst, sizeof(int) * (nv + 1));
		for (i = 0; i < nv; ++i)
			vlist[fill[node[i]]++] = i;
	}
	/* Quadrics on the nodes: their triangles' planes by area, and a plane
	 * square to each open edge, weighted heavily. */
	for (i = 0; i < ntri; ++i) {
		double n[3], l;

		cross3(pos + 3 * node[t[3 * i]], pos + 3 * node[t[3 * i + 1]], pos + 3 * node[t[3 * i + 2]], n);
		l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (l <= 0)
			continue;
		for (j = 0; j < 3; ++j)
			n[j] /= l;
		for (j = 0; j < 3; ++j) {
			const float *p0 = pos + 3 * node[t[3 * i]];

			quad_add_plane(q + 10 * node[t[3 * i + j]], n, -(n[0] * p0[0] + n[1] * p0[1] + n[2] * p0[2]), l / 2);
			area[node[t[3 * i + j]]] += l / 2;
		}
		for (e = 0; e < 3; ++e) {
			int a = node[t[3 * i + e]], b = node[t[3 * i + (e + 1) % 3]], k, shared = 0;
			double d[3], s[3], sl;

			for (k = 0; k < ntri && !shared; ++k) {
				int c0 = node[t[3 * k]], c1 = node[t[3 * k + 1]], c2 = node[t[3 * k + 2]];

				shared = k != i && (c0 == a || c1 == a || c2 == a) && (c0 == b || c1 == b || c2 == b);
			}
			if (shared)
				continue;
			for (j = 0; j < 3; ++j)
				d[j] = pos[3 * b + j] - pos[3 * a + j];
			s[0] = d[1] * n[2] - d[2] * n[1];
			s[1] = d[2] * n[0] - d[0] * n[2];
			s[2] = d[0] * n[1] - d[1] * n[0];
			sl = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
			if (sl <= 0)
				continue;
			for (j = 0; j < 3; ++j)
				s[j] /= sl;
			sl = -(s[0] * pos[3 * a] + s[1] * pos[3 * a + 1] + s[2] * pos[3 * a + 2]);
			quad_add_plane(q + 10 * a, s, sl, 100 * (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
			quad_add_plane(q + 10 * b, s, sl, 100 * (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
		}
	}
	while (alive > 2) {
		double best = 1e300;
		int bu = -1, bv = -1, ok = 1;

		if (textured) {   /* the live corners by node, for the texture term */
			memset(cfirst, 0, sizeof(int) * (nv + 1));
			for (i = 0; i < 3 * ntri; ++i)
				if (!dead[i / 3])
					++cfirst[node[t[i]] + 1];
			for (i = 0; i < nv; ++i)
				cfirst[i + 1] += cfirst[i];
			for (i = 0; i < 3 * ntri; ++i)
				if (!dead[i / 3])
					clist[cfirst[node[t[i]]]++] = i;
			for (i = nv; i > 0; --i)   /* the fill moved each start to the next: back */
				cfirst[i] = cfirst[i - 1];
			cfirst[0] = 0;
		}
		for (i = 0; i < ntri; ++i) {
			if (dead[i])
				continue;
			for (e = 0; e < 6; ++e) {
				int u = node[t[3 * i + e % 3]], v = node[t[3 * i + (e % 3 + (e < 3 ? 1 : 2)) % 3]];
				double qc = quad_sum_at(q + 10 * u, q + 10 * v, pos + 3 * v);
				double c = sqrt((qc > 0 ? qc : 0) / (area[u] + area[v] + 1e-6));   /* a distance */

				if (textured && c < best) {   /* how far the texture moves */
					double tc = tex_shift(t, ntri, dead, node, pos, uv, vfirst, vlist, cfirst, clist, u, v);

					if (tc * scale > c)   /* the texture's width is about the part's */
						c = tc * scale;
				}
				if (c < best) {
					best = c;
					bu = u;
					bv = v;
				}
			}
		}
		if (bu < 0 || best > tol)
			break;
		for (i = 0; i < ntri && ok; ++i) {   /* no triangle round u turned over 60 degrees */
			int h = -1, has_v = 0;
			double n0[3], n1[3];
			const float *p[3];

			if (dead[i])
				continue;
			for (j = 0; j < 3; ++j) {
				has_v |= node[t[3 * i + j]] == bv;
				if (node[t[3 * i + j]] == bu)
					h = j;
			}
			if (h < 0 || has_v)
				continue;
			for (j = 0; j < 3; ++j)
				p[j] = pos + 3 * node[t[3 * i + j]];
			cross3(p[0], p[1], p[2], n0);
			p[h] = pos + 3 * bv;
			cross3(p[0], p[1], p[2], n1);
			if (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] <= 0.5 * sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]) *
					sqrt(n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2]))
				ok = 0;
		}
		if (!ok) {
			double none[3] = { 0, 0, 0 };

			if (++tries > 4 * ntri)
				break;
			quad_add_plane(q + 10 * bu, none, 1, 1e9);   /* dear to fold from now on */
			continue;
		}
		for (j = 0; j < 10; ++j)
			q[10 * bv + j] += q[10 * bu + j];
		area[bv] += area[bu];
		for (i = 0; i < ntri; ++i) {
			int has_v = 0, has_u = 0;

			if (dead[i])
				continue;
			for (j = 0; j < 3; ++j) {
				has_v |= node[t[3 * i + j]] == bv;
				has_u |= node[t[3 * i + j]] == bu;
			}
			if (!has_u)
				continue;
			if (has_v) {
				dead[i] = 1;
				--alive;
				continue;
			}
			for (j = 0; j < 3; ++j) {
				int c = t[3 * i + j], x, r = bv;
				float bd = 1e30f;

				if (node[c] != bu)
					continue;
				for (x = vfirst[bv]; x < vfirst[bv + 1]; ++x) {   /* the vertex at bv nearest this corner's */
					int y = vlist[x], k;
					float du = uv[2 * y] - uv[2 * c], dv = uv[2 * y + 1] - uv[2 * c + 1], dn = 0;

					for (k = 0; k < 3; ++k)
						dn += (float) (nr[3 * y + k] - nr[3 * c + k]) * (nr[3 * y + k] - nr[3 * c + k]) / 4096;
					if (du * du + dv * dv + 0.1f * dn < bd) {
						bd = du * du + dv * dv + 0.1f * dn;
						r = y;
					}
				}
				t[3 * i + j] = r;
			}
		}
	}
	for (i = 0, j = 0; i < ntri; ++i)
		if (!dead[i]) {
			out[3 * j] = (uint16_t) t[3 * i];
			out[3 * j + 1] = (uint16_t) t[3 * i + 1];
			out[3 * j + 2] = (uint16_t) t[3 * i + 2];
			++j;
		}
	alive = j;
done:
	free(pos);
	free(uv);
	free(node);
	free(t);
	free(q);
	free(dead);
	free(vfirst);
	free(vlist);
	free(cfirst);
	free(clist);
	free(area);
	return alive;
}

/* A part's coarser levels, each from the last (head_tol times `scale`):
 * their corners after the full list in idx. */
static int head_lods(mii_mesh *mesh, int textured, int n, float scale) {
	const float *tol = head_tol;
	uint16_t *all;
	uint32_t at = mesh->count;
	int l, prev = mesh->count / 3, from = 0;

	if (!mesh->count || n <= 0)
		return MII_OK;
	all = realloc(mesh->idx, sizeof(uint16_t) * (mesh->count * (n + 1)));
	if (!all)
		return MII_E_MEM;
	mesh->idx = all;
	for (l = 0; l < n; ++l) {
		int got;

		if (prev <= 4) {   /* already as small as it goes: the level before */
			memmove(all + at, all + from, sizeof(uint16_t) * 3 * prev);
			got = prev;
		} else {
			got = simplify(mesh, textured, all + from, prev, tol[l] * scale, all + at);
			if (got < 0)
				return MII_E_MEM;
		}
		mesh->lod_first[l] = at;
		mesh->lod_count[l] = 3 * got;
		from = (int) at;
		at += 3 * got;
		prev = got;
	}
	mesh->lods = n;
	return MII_OK;
}

#ifdef MII_NO_OGC
/* The corners in idx: the full list and its levels (a PC keeps them). */
static uint32_t idx_total(const mii_mesh *mesh) {
	return mesh->lods ? mesh->lod_first[mesh->lods - 1] + mesh->lod_count[mesh->lods - 1] : (uint32_t) mesh->count;
}
#endif

#ifndef MII_NO_OGC
/* A packed mesh's triangles (count corners) as GX display list bytes (so
 * any thread can build them): triangle strips (mii_strips: the GPU works
 * out each vertex it is sent, and a strip sends about one a triangle),
 * each GX_TRIANGLESTRIP on VTXFMT0, a big-endian u16 count, then each
 * vertex's index into the position, normal and coordinate arrays (8 bits
 * each where there are under 256 vertices, else 16), padded with GX_NOPs to
 * 32 bytes. malloc'd; *size its bytes, padded. */
static uint8_t *list_bytes(const uint16_t *idx, uint32_t count, uint32_t w, uint32_t *size) {
	uint32_t n = count / 3, at = 0, need = 0, k;
	uint16_t *v = malloc(sizeof(uint16_t) * 3 * (n + 1)), *lens = malloc(sizeof(uint16_t) * (n + 1));
	uint8_t *o = NULL, *p;
	int ns = v && lens ? mii_strips(idx, (int) n, v, lens) : -1, s;

	if (ns < 0)
		goto done;
	for (s = 0; s < ns; ++s)
		need += 3 + (uint32_t) lens[s] * 3 * w;
	need = (need + 31) & ~31u;
	o = calloc(1, need ? need : 32);   /* 0: GX_NOP */
	if (!o)
		goto done;
	p = o;
	for (s = 0; s < ns; ++s) {
		*p++ = GX_TRIANGLESTRIP | GX_VTXFMT0;
		*p++ = (uint8_t) (lens[s] >> 8);
		*p++ = (uint8_t) lens[s];
		for (k = 0; k < lens[s]; ++k, ++at) {
			int c;

			for (c = 0; c < 3; ++c) {
				if (w == 2)
					*p++ = (uint8_t) (v[at] >> 8);
				*p++ = (uint8_t) v[at];
			}
		}
	}
	*size = need ? need : 32;
done:
	free(v);
	free(lens);
	return o;
}

static int mesh_list(mii_mesh *mesh) {
	uint32_t size, w = mesh->nverts < 256 ? 1 : 2, got[MII_HEAD_LODS + 1];
	uint8_t *part[MII_HEAD_LODS + 1] = { NULL };
	int l, r = MII_OK;

	if (!mesh->count || mesh->shared)
		return MII_OK;
	/* The whole list, then the coarser levels' after it, each 32-byte aligned. */
	for (l = 0; l <= mesh->lods && r == MII_OK; ++l)
		if (!(part[l] = l ? list_bytes(mesh->idx + mesh->lod_first[l - 1], mesh->lod_count[l - 1], w, &got[l]) :
				  list_bytes(mesh->idx, mesh->count, w, &got[l])))
			r = MII_E_MEM;
	size = 0;
	for (l = 0; l <= mesh->lods && r == MII_OK; ++l) {
		if (l) {
			mesh->lod_at[l - 1] = size;
			mesh->lod_size[l - 1] = got[l];
		}
		size += got[l];
	}
	mesh->dl = r == MII_OK ? memalign(32, size) : NULL;
	if (mesh->dl) {
		uint32_t at = 0;

		for (l = 0; l <= mesh->lods; ++l) {
			memcpy((uint8_t *) mesh->dl + at, part[l], got[l]);
			at += got[l];
		}
	}
	for (l = 0; l <= MII_HEAD_LODS; ++l)
		free(part[l]);
	if (!mesh->dl)
		return MII_E_MEM;
	mesh->dl_size = size;
	DCFlushRange(mesh->dl, size);
	DCFlushRange(mesh->pack, mesh->pack_st + (((uint32_t) mesh->nverts * 4 + 31) & ~31u));
	free(mesh->v);
	free(mesh->idx);
	mesh->v = NULL;
	mesh->idx = NULL;
	return MII_OK;
}

/* Linear RGBA to GX's RGB5A3, as RFL keeps its faces: half of RGBA8. */
static int tex_upload(mii_tex *t) {
	uint32_t size = mii_tex_rgb5a3_size(t->w, t->h);

	if (!t->rgba)
		return MII_OK;
	t->gx = memalign(32, size);
	if (!t->gx)
		return MII_E_MEM;
	mii_tex_rgb5a3(t, t->gx);
	DCFlushRange(t->gx, size);
	free(t->rgba);   /* the GPU's copy is all a Wii draws from */
	t->rgba = NULL;
	return MII_OK;
}

static void tex_obj(mii_tex *t) {
	GX_InitTexObj(&t->obj, t->gx, t->w, t->h, GX_TF_RGB5A3, t->wrap_s, t->wrap_t, GX_FALSE);
	GX_InitTexObjFilterMode(&t->obj, GX_LINEAR, GX_LINEAR);
}
/* ---- Faces drawn by the GPU (RFLiMakeTexture) ----------------------------------- */

static int tex_share(mii_cache *cache, mii_tex *t);

/* A face part's texture from RFL_Res.dat, for the GPU: its image in RFL's
 * own GX format (I4, I8, IA4, IA8, RGB5A3), copied 32-byte aligned. */
typedef struct {
	void *data;
	GXTexObj obj;
} gx_part;

static int gx_part_load(const mii_res *res, int arc, int idx, gx_part *out) {
	static const uint8_t gx_fmt[6] = { GX_TF_I4, GX_TF_I8, GX_TF_IA4, GX_TF_IA8, 0xFF, GX_TF_RGB5A3 };
	static const uint8_t bw[6] = { 8, 8, 8, 4, 0, 4 }, bh[6] = { 8, 4, 4, 4, 0, 4 }, bits[6] = { 4, 8, 8, 16, 0, 16 };
	const uint8_t *f;
	uint32_t len, image, need;
	int r = res_file(res, arc, idx, &f, &len), fmt, w, h;

	memset(out, 0, sizeof(*out));
	if (r < 0)
		return r;
	if (len < 0x20 || f[0] > 5 || gx_fmt[f[0]] == 0xFF)
		return MII_E_FORMAT;
	fmt = f[0];
	w = be16(f + 2);
	h = be16(f + 4);
	image = be32(f + 0x1C);
	if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
		return MII_E_FORMAT;
	need = (uint32_t) ((w + bw[fmt] - 1) / bw[fmt]) * ((h + bh[fmt] - 1) / bh[fmt]) * (bw[fmt] * bh[fmt] * bits[fmt] / 8);
	if (image > len || need > len - image)
		return MII_E_FORMAT;
	out->data = memalign(32, (need + 31) & ~31u);
	if (!out->data)
		return MII_E_MEM;
	memcpy(out->data, f + image, need);
	DCFlushRange(out->data, (need + 31) & ~31u);
	/* Clamped and bilinear, as composite() samples (RFLiInitRFLTexture). */
	GX_InitTexObj(&out->obj, out->data, w, h, gx_fmt[fmt], GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&out->obj, GX_LINEAR, GX_LINEAR);
	return MII_OK;
}

/* One TEV stage: colour d + (1 - c) a + c b, alpha from ins, clamped. */
static void tev(int stage, int swap, int ca, int cb, int cc, int cd, int ad, int k) {
	GX_SetTevOrder(stage, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevSwapMode(stage, GX_TEV_SWAP0, swap);
	GX_SetTevKColorSel(stage, k);
	GX_SetTevColorIn(stage, ca, cb, cc, cd);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, ad);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static GXColor kcolor(const float c[3]) {
	return (GXColor) { (u8) (c[0] * 255 + 0.5f), (u8) (c[1] * 255 + 0.5f), (u8) (c[2] * 255 + 0.5f), 255 };
}

/* Draws one expression's face into the EFB's top-left size x size and
 * copies it out to dst (RGB5A3) as RFL does: the parts front to back, each
 * coloured by TEV from its texture's channels (the eye's red, blue and
 * green: colour 0, the eye's colour, white; the mouth's red, green, blue:
 * its two colours, white; the rest one colour), premultiplied by its alpha
 * and added under what is there (GX_BL_INVDSTALPHA, GX_BL_ONE), which is
 * the CPU's back-to-front "over". */
static void gpu_face(const mii *m, int e, int size, gx_part *tx, void *dst) {
	static const mii_tex ids[5];   /* tokens: which of tx a part uses */
	face_part p[8];
	Mtx44 proj;
	Mtx id;
	int n = face_layout(m, e, size, p, ids + 0, ids + 1, ids + 2, ids + 3, ids + 4), i, k;

	guOrtho(proj, 0, (f32) size, 0, (f32) size, 0, 1);   /* pixels, y down */
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	guMtxIdentity(id);
	GX_LoadPosMtxImm(id, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_SetViewport(0, 0, (f32) size, (f32) size, 0, 1);
	GX_SetScissor(0, 0, size, size);
	for (i = n - 1; i >= 0; --i) {   /* front to back */
		const face_part *q = &p[i];
		gx_part *g = &tx[q->tex - ids];
		float a = q->angle * (float) M_PI / 180.0f, ca = cosf(a), sa = sinf(a);
		float xo = q->origin == O_CENTER ? -0.5f : q->origin == O_RIGHT ? -1.0f : 0.0f;
		static const float tx_ = 0.88961464f, ty_ = 0.9276675f;

		if (!g->data || q->w <= 0 || q->h <= 0)
			continue;
		GX_LoadTexObj(&g->obj, GX_TEXMAP0);
		GX_SetTevKColor(GX_KCOLOR0, kcolor(q->c0));
		GX_SetTevKColor(GX_KCOLOR1, kcolor(q->c1));
		if (q->kind == K_FLAT) {   /* colour 0 times the texture's alpha */
			GX_SetNumTevStages(1);
			tev(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXA, GX_CC_ZERO, GX_CA_TEXA,
				GX_TEV_KCSEL_K0);
		} else {
			int second = q->kind == K_EYE ? GX_TEV_SWAP3 : GX_TEV_SWAP2;   /* the eye's blue, the mouth's green */
			int third = q->kind == K_EYE ? GX_TEV_SWAP2 : GX_TEV_SWAP3;

			GX_SetNumTevStages(4);
			tev(GX_TEVSTAGE0, GX_TEV_SWAP1, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_ZERO, GX_CA_TEXA,
				GX_TEV_KCSEL_K0);
			tev(GX_TEVSTAGE1, second, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV, GX_CA_APREV,
				GX_TEV_KCSEL_K1);
			tev(GX_TEVSTAGE2, third, GX_CC_ZERO, GX_CC_ONE, GX_CC_TEXC, GX_CC_CPREV, GX_CA_APREV,
				GX_TEV_KCSEL_K0);
			tev(GX_TEVSTAGE3, GX_TEV_SWAP0, GX_CC_ZERO, GX_CC_CPREV, GX_CC_APREV, GX_CC_ZERO, GX_CA_APREV,
				GX_TEV_KCSEL_K0);   /* premultiplied */
		}
		GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		for (k = 0; k < 4; ++k) {
			int c = k == 0 ? 0 : k == 1 ? 1 : k == 2 ? 3 : 2;   /* round the quad */
			float u = (c & 1 ? 1.0f : 0.0f) + xo, v = c & 2 ? 0.5f : -0.5f;
			float lx = u * q->w, ly = v * q->h;
			float s = q->origin == O_LEFT ? 1.0f - u : q->origin == O_RIGHT ? u + 1.0f : u + 0.5f;

			GX_Position3f32((lx * ca - ly * sa) * tx_ + q->x, (lx * sa + ly * ca) * ty_ + q->y, 0);
			GX_TexCoord2f32(s, v + 0.5f);
		}
		GX_End();
	}
	GX_SetTexCopySrc(0, 0, size, size);
	GX_SetTexCopyDst(size, size, GX_TF_RGB5A3, GX_FALSE);
	GX_CopyTex(dst, GX_TRUE);   /* and clears the corner for the next */
}

int mii_model_make_faces(mii_model *model, const mii_res *res, GXColor clear) {
	gx_part tx[5];
	int e, r = MII_OK, k, made = 0;
	uint32_t size;
	mii_tex fresh[MII_EXPR_COUNT];

	if (!model || !res || !res->data)
		return MII_E_ARG;
	size = mii_tex_rgb5a3_size(model->mask_size, model->mask_size);
	memset(fresh, 0, sizeof(fresh));
	/* GX: the EFB with alpha, no Z, no culling, quads with positions and
	 * texture coordinates, one texture coordinate, no lighting. */
	GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
	GX_PixModeSync();
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_INVDSTALPHA, GX_BL_ONE, GX_LO_CLEAR);
	GX_SetDstAlpha(GX_FALSE, 0);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
	GX_SetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
	GX_SetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	/* The corner starts clear (a copy that clears it, into a face's buffer,
	 * which its own copy then overwrites). */
	GX_SetCopyClear((GXColor) { 0, 0, 0, 0 }, 0x00FFFFFF);
	for (e = 0; e < MII_EXPR_COUNT && r == MII_OK; ++e) {
		expr_face f = expression(&model->m, e);

		if (!(model->expressions >> e & 1))
			continue;
		memset(tx, 0, sizeof(tx));
		fresh[e].w = fresh[e].h = model->mask_size;
		fresh[e].gx = memalign(32, size);
		if (!fresh[e].gx ||
				(r = gx_part_load(res, A_EYE, f.eye_type, &tx[0])) < 0 ||
				(r = gx_part_load(res, A_EYEBROW, model->m.brow_type, &tx[1])) < 0 ||
				(r = gx_part_load(res, A_MOUTH, f.mouth_type, &tx[2])) < 0 ||
				(model->m.mustache && (r = gx_part_load(res, A_MUSTACHE, model->m.mustache, &tx[3])) < 0) ||
				(model->m.mole && (r = gx_part_load(res, A_MOLE, model->m.mole, &tx[4])) < 0)) {
			if (r == MII_OK)
				r = MII_E_MEM;
		} else {
			if (!made++) {
				GX_SetTexCopySrc(0, 0, model->mask_size, model->mask_size);
				GX_SetTexCopyDst(model->mask_size, model->mask_size, GX_TF_RGB5A3, GX_FALSE);
				GX_CopyTex(fresh[e].gx, GX_TRUE);
			}
			DCInvalidateRange(fresh[e].gx, size);
			gpu_face(&model->m, e, model->mask_size, tx, fresh[e].gx);
		}
		GX_DrawDone();   /* the parts' textures are read until here */
		for (k = 0; k < 5; ++k)
			free(tx[k].data);
	}
	/* Back to the defaults mii_draw and most apps expect, RGB8 and Z writes
	 * on (a copy clears Z only while they are on), and the corner in the
	 * caller's clear colour (a clearing copy into a scratch buffer). */
	for (k = 0; k < 4; ++k)
		GX_SetTevSwapMode(GX_TEVSTAGE0 + k, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_PixModeSync();
	{
		void *scratch = memalign(32, size);

		if (scratch) {
			GX_SetCopyClear(clear, 0x00FFFFFF);
			GX_SetTexCopySrc(0, 0, model->mask_size, model->mask_size);
			GX_SetTexCopyDst(model->mask_size, model->mask_size, GX_TF_RGB5A3, GX_FALSE);
			GX_CopyTex(scratch, GX_TRUE);
			GX_DrawDone();
			free(scratch);
		}
	}
	GX_InvalidateTexAll();
	if (r < 0) {
		for (e = 0; e < MII_EXPR_COUNT; ++e)
			free(fresh[e].gx);
		return r;
	}
	for (e = 0; e < MII_EXPR_COUNT; ++e) {
		if (!fresh[e].gx)
			continue;
		fresh[e].wrap_s = fresh[e].wrap_t = 0;
		tex_free(&model->mask[e]);
		model->mask[e] = fresh[e];
		tex_share(model->cache, &model->mask[e]);
	}
	model->faces_pending = 0;
	return MII_OK;
}

#endif

/* A texture into the cache (and, on the Wii, its texture object made). */
static int tex_share(mii_cache *cache, mii_tex *t) {
	int r;

#ifndef MII_NO_OGC
	if (!t->gx)
		return MII_OK;
	r = share_bufs(cache, &t->gx, mii_tex_rgb5a3_size(t->w, t->h), NULL, 0, &t->shared);
	tex_obj(t);
#else
	if (!t->rgba)
		return MII_OK;
	r = share_bufs(cache, (void **) &t->rgba, (uint32_t) t->w * t->h * 4, NULL, 0, &t->shared);
#endif
	return r;
}

static uint32_t pack_bytes(const mii_mesh *mesh) {
	return mesh->pack_st + (((uint32_t) mesh->nverts * 4 + 31) & ~31u);
}

/* The hash of a packed mesh's vertices and whole list: what a twin in the
 * cache must match before its bytes are compared. */
static uint32_t whole_hash(const mii_mesh *mesh) {
	return hash_bytes(hash_bytes(2166136261u, mesh->pack, pack_bytes(mesh)), mesh->idx,
					  sizeof(uint16_t) * mesh->count);
}

/* Does the cached part's whole list hold these corners? (A PC keeps the
 * corners; a Wii the display list made from them.) */
static int whole_matches(const share *e, const mii_mesh *mesh) {
#ifndef MII_NO_OGC
	uint32_t size, whole = e->lods ? e->lod_at[0] : e->size[1];
	uint8_t *b = list_bytes(mesh->idx, mesh->count, mesh->nverts < 256 ? 1 : 2, &size);
	int same = b && size == whole && !memcmp(b, e->buf[1], size);   /* the list it would make */

	free(b);
	return same;
#else
	return !memcmp(e->buf[1], mesh->idx, sizeof(uint16_t) * mesh->count);
#endif
}

/* A part the cache already holds, with the same levels asked for: take it,
 * levels and all (mesh_list and mesh_share then leave it). */
static int mesh_twin(mii_cache *cache, mii_mesh *mesh, int lods) {
	uint32_t h;
	share *e;

	if (!cache || !mesh->count)
		return 0;
	h = whole_hash(mesh);
	mesh->whole = h;
	for (e = cache->head; e; e = e->next)
		if (e->mesh && e->whole == h && e->count == mesh->count && e->lods == lods &&
				e->size[0] == pack_bytes(mesh) && !memcmp(e->buf[0], mesh->pack, e->size[0]) &&
				whole_matches(e, mesh)) {
			free(mesh->pack);
			free(mesh->idx);
			mesh->pack = e->buf[0];
#ifndef MII_NO_OGC
			mesh->idx = NULL;
			free(mesh->v);
			mesh->v = NULL;
			mesh->dl = e->buf[1];
			mesh->dl_size = e->size[1];
#else
			mesh->idx = e->buf[1];
#endif
			mesh->lods = e->lods;
			memcpy(mesh->lod_count, e->lod_count, sizeof(e->lod_count));
			memcpy(mesh->lod_first, e->lod_first, sizeof(e->lod_first));
			memcpy(mesh->lod_at, e->lod_at, sizeof(e->lod_at));
			memcpy(mesh->lod_size, e->lod_size, sizeof(e->lod_size));
			++e->refs;
			mesh->shared = e;
			return 1;
		}
	return 0;
}

static int mesh_share(mii_cache *cache, mii_mesh *mesh) {
	share *e;
	int r;

	if (!mesh->count || mesh->shared)   /* empty, or taken whole from the cache */
		return MII_OK;
#ifndef MII_NO_OGC
	r = share_bufs(cache, (void **) &mesh->pack, pack_bytes(mesh), &mesh->dl, mesh->dl_size, &mesh->shared);
#else
	r = share_bufs(cache, (void **) &mesh->pack, pack_bytes(mesh), (void **) &mesh->idx,
				   sizeof(uint16_t) * idx_total(mesh), &mesh->shared);
#endif
	e = mesh->shared;
	if (r == MII_OK && e && e->refs == 1 && mesh->whole) {   /* new: it can be found whole */
		e->mesh = 1;
		e->whole = mesh->whole;
		e->count = mesh->count;
		e->lods = mesh->lods;
		memcpy(e->lod_count, mesh->lod_count, sizeof(e->lod_count));
		memcpy(e->lod_first, mesh->lod_first, sizeof(e->lod_first));
		memcpy(e->lod_at, mesh->lod_at, sizeof(e->lod_at));
		memcpy(e->lod_size, mesh->lod_size, sizeof(e->lod_size));
	}
	return r;
}

/* The faces of the expressions asked for, at that size, uploaded and shared. */
static int make_masks(const mii_res *res, mii_model *model, int size, unsigned exprs, mii_tex *out) {
	int e, r;

#ifndef MII_NO_OGC
	if (model->gpu_faces) {   /* mii_model_make_faces draws them, between frames */
		model->faces_pending = 1;
		return MII_OK;
	}
#endif
	for (e = 0; e < MII_EXPR_COUNT; ++e) {
		if (!(exprs >> e & 1))
			continue;
		if ((r = make_mask(res, &model->m, e, size, &out[e])) < 0)
			return r;
#ifndef MII_NO_OGC
		if ((r = tex_upload(&out[e])) < 0)
			return r;
#endif
		if ((r = tex_share(model->cache, &out[e])) < 0)
			return r;
	}
	return MII_OK;
}

int mii_model_set_mask_size(mii_model *model, const mii_res *res, int mask_size) {
	mii_tex fresh[MII_EXPR_COUNT];
	int e, r;

	if (!model || !res || !res->data ||
			(mask_size != 32 && mask_size != 64 && mask_size != 128 && mask_size != 256))
		return MII_E_ARG;
	if (mask_size == model->mask_size)
		return MII_OK;
#ifndef MII_NO_OGC
	if (model->gpu_faces) {   /* the old faces draw until mii_model_make_faces replaces them */
		model->mask_size = mask_size;
		model->faces_pending = 1;
		return MII_OK;
	}
#endif
	memset(fresh, 0, sizeof(fresh));
	r = make_masks(res, model, mask_size, model->expressions, fresh);
	for (e = 0; e < MII_EXPR_COUNT; ++e)
		tex_free(r < 0 ? &fresh[e] : &model->mask[e]);
	if (r < 0)
		return r;
	memcpy(model->mask, fresh, sizeof(fresh));
	model->mask_size = mask_size;
	return MII_OK;
}

int mii_model_make(const mii_res *res, const mii *m, int mask_size, mii_model *out) {
	mii_model_opts o = { mask_size, 0, NULL, 0, 0 };

	return mii_model_build(res, m, &o, out);
}

int mii_model_build(const mii_res *res, const mii *m, const mii_model_opts *opts, mii_model *out) {
	vec3 anchors[3] = { { 0, 0, 0 } }, at, zero = { 0, 0, 0 };
	int mask_size = opts && opts->mask_size ? opts->mask_size : 128;
	unsigned exprs = (opts && opts->expressions ? opts->expressions : MII_EXPRS_ALL) | MII_EXPR_BIT(MII_EXPR_NORMAL);
	float scale;
	int r, i;

	if (!res || !res->data || !m || !out)
		return MII_E_ARG;
	memset(out, 0, sizeof(*out));
	for (i = 0; i < MII_PARTS; ++i)
		out->part[i].tex = -1;
	out->m = *m;
	if (mask_size != 32 && mask_size != 64 && mask_size != 128 && mask_size != 256)
		return MII_E_ARG;
	out->mask_size = mask_size;
	out->expressions = exprs;
	out->cache = opts ? opts->cache : NULL;
	out->gpu_faces = opts && opts->gpu_faces;

	/* The faceline first: its anchors place the rest (RFLiInitCharModelRes). */
	if ((r = shape(res, A_FACELINE, m->face_type, "face", 1, 1, zero, 0,
				   &out->part[MII_PART_FACELINE], anchors)) < 0)
		goto fail;
	out->part[MII_PART_FACELINE].tex = MII_TEX_FACE;
	if ((r = shape(res, A_MASK, m->face_type, "mask", 1, 1, zero, 0, &out->part[MII_PART_MASK], NULL)) < 0)
		goto fail;
	out->part[MII_PART_MASK].tex = MII_TEX_COUNT;   /* the expression's mask */
	if ((r = shape(res, A_HAIR, m->hair_type, "hair", 0, 1, anchors[2], m->hair_flip,
				   &out->part[MII_PART_HAIR], NULL)) < 0 ||
			(r = shape(res, A_FOREHEAD, m->hair_type, "frhd", 0, 1, anchors[2], m->hair_flip,
					   &out->part[MII_PART_FOREHEAD], NULL)) < 0 ||
			(r = shape(res, A_CAP, m->hair_type, "cap_", 1, 1, anchors[2], m->hair_flip,
					   &out->part[MII_PART_CAP], NULL)) < 0)
		goto fail;
	out->part[MII_PART_HAIR].color = rgba(hair_colors[pick(m->hair_color, 8)]);
	out->part[MII_PART_FOREHEAD].color = rgba_as5(skin_colors[pick(m->face_color, 6)]);
	out->part[MII_PART_CAP].tex = MII_TEX_CAP;
	if (m->beard_type) {
		if ((r = shape(res, A_BEARD, m->beard_type, "berd", 0, 1, anchors[1], 0,
					   &out->part[MII_PART_BEARD], NULL)) < 0)
			goto fail;
		out->part[MII_PART_BEARD].color = rgba(hair_colors[pick(m->beard_color, 8)]);
	}
	scale = 0.4f + 0.175f * m->nose_scale;
	at = anchors[0];
	at.y += -1.5f * (m->nose_y - 8);
	if ((r = shape(res, A_NOSE, m->nose_type, "nose", 0, scale, at, 0, &out->part[MII_PART_NOSE], NULL)) < 0 ||
			(r = shape(res, A_NLINE, m->nose_type, "nsln", 1, scale, at, 0,
					   &out->part[MII_PART_NOSELINE], NULL)) < 0)
		goto fail;
	out->part[MII_PART_NOSE].color = rgba_as5(skin_colors[pick(m->face_color, 6)]);
	out->part[MII_PART_NOSELINE].tex = MII_TEX_NOSELINE;
	if (m->glass_type) {
		scale = 0.4f + 0.15f * m->glass_scale;
		at = anchors[0];
		at.y += 5 - 1.5f * (m->glass_y - 11);
		at.z += 2;
		if ((r = shape(res, A_GLASS, 0, "glas", 1, scale, at, 0, &out->part[MII_PART_GLASS], NULL)) < 0)
			goto fail;
		out->part[MII_PART_GLASS].tex = MII_TEX_GLASS;
	}

	/* Textures. */
	if ((r = res_tex(res, A_FACETEX, m->face_tex, &out->tex[MII_TEX_FACE])) < 0 ||
			(r = res_tex(res, A_CAPTEX, m->hair_type, &out->tex[MII_TEX_CAP])) < 0 ||
			(r = res_tex(res, A_NLINETEX, m->nose_type, &out->tex[MII_TEX_NOSELINE])) < 0 ||
			(m->glass_type && (r = res_tex(res, A_GLASSTEX, m->glass_type, &out->tex[MII_TEX_GLASS])) < 0))
		goto fail;
	bake(&out->tex[MII_TEX_FACE], MII_TEX_FACE, skin_colors[pick(m->face_color, 6)]);
	bake(&out->tex[MII_TEX_CAP], MII_TEX_CAP, fav_colors[pick(m->fav_color, 12)]);
	bake(&out->tex[MII_TEX_NOSELINE], MII_TEX_NOSELINE, NULL);
	if (m->glass_type)
		bake(&out->tex[MII_TEX_GLASS], MII_TEX_GLASS, glass_colors[pick(m->glass_color, 6)]);

	if ((r = make_body(out)) < 0)
		goto fail;
	for (i = 0; i < MII_PARTS; ++i)
		if ((r = pack_mesh(&out->part[i])) < 0)
			goto fail;
	for (i = 0; i < MII_PARTS; ++i) {   /* the cache may hold it already, levels and all */
		int lods = opts && opts->lods > 0 && i < MII_HEAD_PARTS ?
				   (opts->lods < MII_HEAD_LODS ? opts->lods : MII_HEAD_LODS) : 0;

		if (mesh_twin(out->cache, &out->part[i], lods))
			continue;
		/* The hair at half the tolerance: its thin pieces (bangs,
		 * sideburns) folded at the whole of it parted, and the sky showed
		 * through the cracks, sparkling as a Mii walked (level 2, short
		 * styles; 72 styles in three views: 47 crack pixels against 6). */
		if (lods && (r = head_lods(&out->part[i], out->part[i].tex >= 0, lods,
								   i == MII_PART_HAIR ? 0.5f : 1)) < 0)   /* coarser heads */
			goto fail;
	}

#ifndef MII_NO_OGC
	for (i = 0; i < MII_TEX_COUNT; ++i)
		if ((r = tex_upload(&out->tex[i])) < 0)
			goto fail;
	for (i = 0; i < MII_PARTS; ++i)
		if ((r = mesh_list(&out->part[i])) < 0)
			goto fail;
#endif
	for (i = 0; i < MII_TEX_COUNT; ++i)
		if ((r = tex_share(out->cache, &out->tex[i])) < 0)
			goto fail;
	for (i = 0; i < MII_PARTS; ++i)
		if ((r = mesh_share(out->cache, &out->part[i])) < 0)
			goto fail;
	if ((r = make_masks(res, out, mask_size, exprs, out->mask)) < 0)
		goto fail;
	return MII_OK;
fail:
	mii_model_free(out);
	return r;
}

void mii_mesh_free(mii_mesh *mesh) {
	if (mesh->shared) {
		unshare(mesh->shared);
		mesh->shared = NULL;
		mesh->pack = NULL;
#ifndef MII_NO_OGC
		mesh->dl = NULL;
#else
		mesh->idx = NULL;
#endif
	}
	free(mesh->v);
	free(mesh->idx);
	free(mesh->pack);
#ifndef MII_NO_OGC
	free(mesh->dl);
#endif
	mesh->v = NULL;
	mesh->idx = NULL;
	mesh->pack = NULL;
	mesh->dl = NULL;
	mesh->count = mesh->nverts = 0;
	mesh->dl_size = 0;
}

void mii_model_free(mii_model *model) {
	int i;

	if (!model)
		return;
	for (i = 0; i < MII_PARTS; ++i)
		mii_mesh_free(&model->part[i]);
	for (i = 0; i < MII_TEX_COUNT; ++i)
		tex_free(&model->tex[i]);
	for (i = 0; i < MII_EXPR_COUNT; ++i)
		tex_free(&model->mask[i]);
	free(model->skin_pos);
	free(model->skin_nrm);
	free(model->pal_pos);
	free(model->pal_nrm);
	free(model->pal_env);
	free(model->last_pm);
	free(model->last_w);
#ifndef MII_NO_OGC
	for (i = 0; i <= MII_HEAD_LODS; ++i) {
		free(model->head_dl[i][0]);
		free(model->head_dl[i][1]);
	}
#endif
	memset(model, 0, sizeof(*model));
}

void mii_model_bounds(const mii_model *model, int head_only, float *top, float *half_width) {
	/* The head: a faceline is about 50 tall, the tallest hair about 70. */
	float head_top = 72, head_half = 36;

	if (head_only) {
		*top = head_top;
		*half_width = head_half;
		return;
	}
	*top = model->head_y + head_top;
	/* Room for an arm raised out to the side (the built body's wave); the
	 * Wii body's bodyGreeting raises the hand beside the head. */
	if (model->body)
		*half_width = model->shoulder[0] + 10;
	else
		*half_width = model->shoulder[0] + 0.8f * model->arm_len * model->body_scale[1] + 6;
	if (*half_width < head_half)
		*half_width = head_half;
}

#ifndef MII_NO_OGC

/* ---- Drawing (RFL_Model.c: RFLDrawOpaCore, RFLDrawXluCore) ------------------- */

#ifdef MII_PROFILE
#include <ogc/lwp_watchdog.h>
mii_profile_ticks mii_profile;
#define PROF_T(v) uint64_t v = gettime()
#define PROF_ADD(field, from) (mii_profile.field += gettime() - (from))
#else
#define PROF_T(v)
#define PROF_ADD(field, from)
#endif

static void mtx_mul(Mtx out, Mtx a, Mtx b) {
	Mtx t;

	guMtxConcat(a, b, t);
	guMtxCopy(t, out);
}

static void mtx_trans(Mtx m, float x, float y, float z) {
	guMtxIdentity(m);
	guMtxTrans(m, x, y, z);
}

static void mtx_rot(Mtx m, char axis, float rad) {
	guMtxRotRad(m, axis, rad);
}

static void mtx_scale(Mtx m, float x, float y, float z) {
	guMtxScale(m, x, y, z);
}

/* view times model into a matrix slot: positions, and normals by the
 * inverse transpose. */
static void load_to(Mtx view, Mtx model, int slot) {
	Mtx mv, inv, nrm;

	guMtxConcat(view, model, mv);
	GX_LoadPosMtxImm(mv, slot);
	if (guMtxInverse(mv, inv))
		guMtxTranspose(inv, nrm);
	else
		guMtxCopy(mv, nrm);
	GX_LoadNrmMtxImm(nrm, slot);
}

static void load(Mtx view, Mtx model) {
	load_to(view, model, GX_PNMTX0);
}

/* A part's state: its texture (on texmap; a NULL tex with texmap
 * GX_TEXMAP1 is the face, loaded there by the caller) or colour, and its
 * packed arrays and their formats. */
static void part_state(const mii_mesh *mesh, const GXTexObj *tex, int texmap) {
	int w = mesh->nverts < 256 ? GX_INDEX8 : GX_INDEX16;

	if (tex || texmap == GX_TEXMAP1) {
		if (tex)
			GX_LoadTexObj((GXTexObj *) tex, texmap);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor) { 255, 255, 255, 255 });
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, texmap, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	} else {
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor) { mesh->color >> 24, mesh->color >> 16,
													 mesh->color >> 8, 255 });
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	}
	/* The GPU fetches the list and the vertices from RAM: a few bytes in
	 * the FIFO. */
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, w);
	GX_SetVtxDesc(GX_VA_NRM, w);
	GX_SetVtxDesc(GX_VA_TEX0, w);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, mesh->pos_frac);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S16, mesh->st_frac);
	GX_SetArray(GX_VA_POS, mesh->pack, 6);
	GX_SetArray(GX_VA_NRM, mesh->pack + mesh->pack_nrm, 3);
	GX_SetArray(GX_VA_TEX0, mesh->pack + mesh->pack_st, 4);
}

/* A level's list in the part's display list. */
static const uint8_t *part_list(const mii_mesh *mesh, int lod, uint32_t *size) {
	if (lod > 0 && mesh->lods) {
		lod = lod < mesh->lods ? lod : mesh->lods;
		*size = mesh->lod_size[lod - 1];
		return (const uint8_t *) mesh->dl + mesh->lod_at[lod - 1];
	}
	*size = mesh->lods ? mesh->lod_at[0] : mesh->dl_size;   /* the levels follow the whole list */
	return mesh->dl;
}

static void draw_mesh_lod(const mii_mesh *mesh, const GXTexObj *tex, int lod) {
	const uint8_t *d;
	uint32_t size;

	if (!mesh->count || !mesh->dl)
		return;
	part_state(mesh, tex, GX_TEXMAP0);
	d = part_list(mesh, lod, &size);
	GX_CallDispList((void *) d, size);
}

static void draw_mesh(const mii_mesh *mesh, const GXTexObj *tex) {
	draw_mesh_lod(mesh, tex, 0);
}

static void setup(Mtx view) {
	GXLightObj light;
	guVector pos = { -900, 1100, 1400 }, vpos;

	GX_InvVtxCache();   /* the arrays may be new at an address a freed model used */
	if (tex_freed) {    /* and the textures */
		GX_InvalidateTexAll();
		tex_freed = 0;
	}

	/* One white light from the front, upper left, as RFL's icons light a
	 * head, in view space; a grey ambient. */
	guVecMultiply(view, &pos, &vpos);
	GX_InitLightPos(&light, vpos.x, vpos.y, vpos.z);
	GX_InitLightColor(&light, (GXColor) { 170, 170, 170, 255 });
	GX_InitLightAttn(&light, 1, 0, 0, 1, 0, 0);
	GX_LoadLightObj(&light, GX_LIGHT0);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
	GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor) { 112, 112, 112, 255 });

	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetZCompLoc(GX_FALSE);   /* after the alpha compare: clear texels write no Z */
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetCullMode(GX_CULL_BACK);
	GX_SetCurrentMtx(GX_PNMTX0);
}

/* A part into the display list being recorded: its state, then its
 * triangles inline (a list cannot call another), read back from its own
 * list: the same indexes. */
static void record_part(const mii_mesh *mesh, const GXTexObj *tex, int texmap, int lod) {
	uint32_t size, at = 0, w = mesh->nverts < 256 ? 1 : 2;
	const uint8_t *d;

	if (!mesh->count || !mesh->dl)
		return;
	part_state(mesh, tex, texmap);
	d = part_list(mesh, lod, &size);
	while (at + 3 <= size && (d[at] == (GX_TRIANGLESTRIP | GX_VTXFMT0) || d[at] == (GX_TRIANGLES | GX_VTXFMT0))) {
		uint32_t n = (uint32_t) (d[at + 1] << 8 | d[at + 2]), i;
		const uint8_t *c = d + at + 3;

		GX_Begin(d[at] & 0xF8, GX_VTXFMT0, (u16) n);
		for (i = 0; i < n; ++i, c += 3 * w)
			if (w == 1) {
				GX_Position1x8(c[0]);
				GX_Normal1x8(c[1]);
				GX_TexCoord1x8(c[2]);
			} else {
				GX_Position1x16((u16) (c[0] << 8 | c[1]));
				GX_Normal1x16((u16) (c[2] << 8 | c[3]));
				GX_TexCoord1x16((u16) (c[4] << 8 | c[5]));
			}
		GX_End();
		at += 3 + n * 3 * w;
	}
}

/* Records into a buffer of cap bytes, then keeps an exact copy. */
static void *record_begin(uint32_t cap) {
	uint8_t *buf = memalign(32, cap);

	if (buf) {
		DCInvalidateRange(buf, cap);   /* the list is written beneath the cache */
		GX_BeginDispList(buf, cap);
	}
	return buf;
}

static void *record_end(void *buf, uint32_t *size) {
	uint32_t got = GX_EndDispList();
	uint8_t *keep;

	*size = 0;
	if (!got) {   /* it did not fit */
		free(buf);
		return NULL;
	}
	keep = memalign(32, got);
	if (keep) {
		DCInvalidateRange(buf, got);
		memcpy(keep, buf, got);
		DCFlushRange(keep, got);
		*size = got;
	}
	free(buf);
	return keep;
}

/* The head's opaque parts, in drawing order. */
static const int head_opaque[] = { MII_PART_BEARD, MII_PART_NOSE, MII_PART_FOREHEAD, MII_PART_HAIR,
								   MII_PART_CAP, MII_PART_FACELINE };

/* A head level as two display lists: the opaque parts (the forehead on
 * PNMTX1); then, after the face's mask (drawn by each draw: expressions,
 * and faces drawn later, change it), the nose line and the glasses.
 *
 * What a list records is what libogc sends while it records: state it has
 * marked changed goes out only at the next primitive, and after
 * GX_EndDispList libogc puts back its own copy of the state from before.
 * So the first list sets, before its first triangles, all it relies on
 * (the current matrix too, which libogc already thought set), and nothing
 * at its end: what follows a call (the matrix, culling, Z) is set again by
 * the caller through libogc, which then sends it. The second list draws on
 * the matrix draw_head makes current for the face's layers (head_mtx). */
static int compile_head(mii_model *model, int lod) {
	uint32_t cap = 1024;
	uint8_t *buf;
	unsigned i;

	for (i = 0; i < MII_HEAD_PARTS; ++i)
		cap += 256 + (uint32_t) model->part[i].count * 6;
	cap = (cap + 31) & ~31u;
	if (!(buf = record_begin(cap)))
		return 0;
	GX_SetCurrentMtx(GX_PNMTX0);   /* sent with the first part */
	for (i = 0; i < sizeof(head_opaque) / sizeof(head_opaque[0]); ++i) {
		const mii_mesh *p = &model->part[head_opaque[i]];

		if (head_opaque[i] == MII_PART_FOREHEAD && lod > 0) {
			GX_SetCurrentMtx(GX_PNMTX1);
			record_part(p, NULL, GX_TEXMAP0, lod);
			GX_SetCurrentMtx(GX_PNMTX0);
			continue;
		}
		record_part(p, p->tex >= 0 && p->tex < MII_TEX_COUNT ? &model->tex[p->tex].obj : NULL, GX_TEXMAP0, lod);
	}
	model->head_dl[lod][0] = record_end(buf, &model->head_dl_size[lod][0]);
	if (!model->head_dl[lod][0])
		return 0;
	if (!(buf = record_begin(cap))) {
		free(model->head_dl[lod][0]);   /* both lists or neither */
		model->head_dl[lod][0] = NULL;
		return 0;
	}
	if (model->part[MII_PART_NOSELINE].count)
		record_part(&model->part[MII_PART_NOSELINE], &model->tex[MII_TEX_NOSELINE].obj, GX_TEXMAP0, lod);
	if (model->part[MII_PART_GLASS].count) {
		GX_SetCullMode(GX_CULL_NONE);
		record_part(&model->part[MII_PART_GLASS], &model->tex[MII_TEX_GLASS].obj, GX_TEXMAP0, lod);
	}
	model->head_dl[lod][1] = record_end(buf, &model->head_dl_size[lod][1]);
	if (!model->head_dl[lod][1]) {
		free(model->head_dl[lod][0]);
		model->head_dl[lod][0] = NULL;
		return 0;
	}
	return 1;
}

/* The head's matrices for a level: the head in GX_PNMTX0; at a coarser
 * level, the forehead in GX_PNMTX1 and the face's layers in GX_PNMTX2.
 * - A coarser head's hair may come in by its level's tolerance: the
 *   forehead (the scalp under it) is drawn that much smaller, about the
 *   head's middle (some 30 units out, at (0, 35, 0)), or it shows through.
 * - The layers over the face (mask, nose line, glasses) test Z but write
 *   none: drawn just in front of the faceline, they must not fight it or
 *   each other. A coarser faceline and mask each move by up to the level's
 *   tolerance, so the layers are drawn that much (and a fifth) nearer the
 *   camera. */
static void head_mtx(Mtx view, Mtx head, int level) {
	float tol, k;
	Mtx in, t, mv, inv, nrm;

	load_to(view, head, GX_PNMTX0);
	if (level <= 0)
		return;
	tol = head_tol[level - 1];
	k = 1 - tol / 30;
	guMtxScale(in, k, k, k);
	guMtxTransApply(in, in, 0, 35 * (1 - k), 0);
	guMtxConcat(head, in, t);
	load_to(view, t, GX_PNMTX1);
	guMtxConcat(view, head, mv);
	if (guMtxInverse(mv, inv))
		guMtxTranspose(inv, nrm);
	else
		guMtxCopy(mv, nrm);
	mv[2][3] += 1.2f * tol;
	GX_LoadPosMtxImm(mv, GX_PNMTX2);
	GX_LoadNrmMtxImm(nrm, GX_PNMTX2);
}

static void draw_head(const mii_model *model, int expression, Mtx view, Mtx head, int lod) {
	static const int translucent[] = { MII_PART_MASK, MII_PART_NOSELINE, MII_PART_GLASS };
	mii_model *mm = (mii_model *) model;   /* the lists are made on first use */
	int built = model->part[MII_PART_FACELINE].lods;
	int level = lod > 0 && built ? (lod < built ? lod : built) : 0;   /* as far as built */
	unsigned i;

	if (expression < 0 || expression >= MII_EXPR_COUNT || !model->mask[expression].gx)
		expression = MII_EXPR_NORMAL;   /* not built (mii_model_opts.expressions) */
	head_mtx(view, head, level);
	/* The display lists, once there is a face to put between them. */
	if (model->mask[expression].gx && (model->head_dl[level][0] || compile_head(mm, level))) {
		GX_CallDispList(model->head_dl[level][0], model->head_dl_size[level][0]);
		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);   /* the layers over the face */
		if (level > 0)
			GX_SetCurrentMtx(GX_PNMTX2);
		draw_mesh_lod(&model->part[MII_PART_MASK], &model->mask[expression].obj, level);
		GX_CallDispList(model->head_dl[level][1], model->head_dl_size[level][1]);
		/* What the lists changed, through libogc so it sends it (above). */
		GX_SetCullMode(GX_CULL_BACK);
		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		GX_SetCurrentMtx(GX_PNMTX0);
		GX_ClearVtxDesc();
		return;
	}

	/* Without lists (no face yet, or no memory for them): part by part. */
	for (i = 0; i < sizeof(head_opaque) / sizeof(head_opaque[0]); ++i) {
		const mii_mesh *p = &model->part[head_opaque[i]];

		if (head_opaque[i] == MII_PART_FOREHEAD && level > 0) {
			GX_SetCurrentMtx(GX_PNMTX1);
			draw_mesh_lod(p, NULL, level);
			GX_SetCurrentMtx(GX_PNMTX0);
			continue;
		}
		draw_mesh_lod(p, p->tex >= 0 && p->tex < MII_TEX_COUNT ? &model->tex[p->tex].obj : NULL, level);
	}
	if (level > 0)
		GX_SetCurrentMtx(GX_PNMTX2);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	for (i = 0; i < sizeof(translucent) / sizeof(translucent[0]); ++i) {
		const mii_mesh *p = &model->part[translucent[i]];

		if (!p->count)
			continue;
		if (translucent[i] == MII_PART_MASK && !model->mask[expression].gx)
			continue;   /* a GPU face not drawn yet (mii_model_make_faces) */
		if (translucent[i] == MII_PART_GLASS)
			GX_SetCullMode(GX_CULL_NONE);
		draw_mesh_lod(p, translucent[i] == MII_PART_MASK ? &model->mask[expression].obj :
					  &model->tex[p->tex].obj, level);
	}
	GX_SetCullMode(GX_CULL_BACK);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetCurrentMtx(GX_PNMTX0);
}

/* The Wii body's shadow (mii_body_shadow): the Mii Channel's mt_shadow, a
 * quad of its colour whose alpha is the texture's intensity, blended over
 * what is under it, testing Z but not writing it, after the Mii. */
static void draw_shadow(const mii_model *model, const mii_pose *pose, Mtx view, Mtx root) {
	const mii_body *body = model->body;
	static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	float sm[3][4];
	Mtx m;
	GXTexObj tex;
	int i;

	if (!mii_body_shadow(model, pose, sm))
		return;
	guMtxConcat(root, sm, m);
	guMtxConcat(view, m, m);
	GX_LoadPosMtxImm(m, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_InitTexObj(&tex, body->shadow_tex.gx, body->shadow_tex.w, body->shadow_tex.h, GX_TF_I8, GX_CLAMP,
				  GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&tex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GX_LoadTexObj(&tex, GX_TEXMAP0);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevColor(GX_TEVREG0, (GXColor) { body->shadow_rgb[0], body->shadow_rgb[1], body->shadow_rgb[2], 255 });
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
	for (i = 0; i < 4; ++i) {
		GX_Position3f32(corner[i][0], 0, corner[i][1]);
		GX_TexCoord2f32((corner[i][0] + 1) / 2, (corner[i][1] + 1) / 2);
	}
	GX_End();
	GX_SetCullMode(GX_CULL_BACK);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_ClearVtxDesc();
}

/* A Wii body (miibody.c), skinned on the GPU as NW4R draws a model: each
 * MDL0 object's own display list, whose indexed loads take each bone's
 * matrices from this frame's palette by matrix id (GX_POSMTXARRAY,
 * GX_NRMMTXARRAY), its vertices from the MDL0's own arrays. The CPU only
 * fills the palette: a matrix per bone, under 2 KB through the FIFO a frame
 * (CPU skinning sent 54 KB). The head on the head bone, its bind pose
 * upright, the pose's look on top. */
static void draw_body(const mii_model *model, const mii_pose *pose, Mtx view, Mtx root) {
	mii_model *mm = (mii_model *) model;   /* the palette ring and the last pose are scratch */
	const mii_body *body = model->body;
	float (*bp)[3][4] = mm->last_pm, (*bw)[3][4] = mm->last_w;
	float (*pp)[3][4] = mm->pal_pos + mm->pal_size * mm->pal_next;
	float (*pn)[3][3] = mm->pal_nrm + mm->pal_size * mm->pal_next;
	float (*pe)[3][4] = mm->pal_env + mm->pal_size * mm->pal_next;
	int env = body->env.gx != NULL;
	Mtx mv, inv, nmv, head, t;
	GXTexObj tex;
	int i, j, k, id;

	PROF_T(t0);

	mm->pal_next = (mm->pal_next + 1) % MII_PALETTES;
	if (!pose->reuse || !mm->posed) {   /* pose it: once, for the body and the head */
		const float (*b)[3] = body->bone[body->head].bind_rot;
		Mtx bt;

		/* On the paired-single unit: each bone a matrix concatenation, its
		 * scale a column scale (ps_guMtxApplyScale). */
		mii_body_world(model, pose, bw);
		for (i = 0; i < body->nbones; ++i)
			guMtxApplyScale(bw[i], bp[i], model->bone_scale[i][0], model->bone_scale[i][1],
							model->bone_scale[i][2]);
		for (i = 0; i < 3; ++i) {   /* the head: R_head times the bind's transpose */
			for (j = 0; j < 3; ++j)
				bt[i][j] = b[j][i];
			bt[i][3] = 0;
		}
		guMtxConcat(bw[body->head], bt, mm->last_head);
		mm->posed = 1;
	}
	PROF_ADD(pose, t0);
	PROF_T(t1);
	/* The palette is written whole, and the cache never holds it (a ring,
	 * a Mii's every frame): take its lines as zeros (dcbz) rather than read
	 * them from memory first. */
	DCZeroRange(pp, sizeof(float) * 12 * mm->pal_size);
	DCZeroRange(pn, sizeof(float) * 9 * mm->pal_size);
	if (env)
		DCZeroRange(pe, sizeof(float) * 12 * mm->pal_size);
	guMtxConcat(view, root, mv);
	if (guMtxInverse(mv, inv))
		guMtxTranspose(inv, nmv);
	else
		guMtxCopy(mv, nmv);
	for (id = 0; id < body->nmatrices; ++id) {
		int bone = body->matrix_bone[id];

		if (bone < 0) {   /* an id no bone has: never drawn with, kept harmless */
			guMtxCopy(mv, pp[id]);
			guMtxCopy(nmv, t);
		} else {
			guMtxConcat(mv, bp[bone], pp[id]);
			guMtxConcat(nmv, bw[bone], t);   /* the normals: the rotations only */
		}
		for (i = 0; i < 3; ++i)
			for (j = 0; j < 3; ++j)
				pn[id][i][j] = t[i][j];
		if (env)   /* the normal as the camera sees it, to the texture: s = (1 + x) / 2, t = (1 - y) / 2 */
			for (i = 0; i < 2; ++i) {
				float l = sqrtf(t[i][0] * t[i][0] + t[i][1] * t[i][1] + t[i][2] * t[i][2]);
				float h = l > 0 ? (i ? -0.5f : 0.5f) / l : 0;

				for (j = 0; j < 3; ++j)
					pe[id][i][j] = t[i][j] * h;
				pe[id][i][3] = 0.5f;
			}
	}
	DCFlushRange(pp, sizeof(float) * 12 * mm->pal_size);
	DCFlushRange(pn, sizeof(float) * 9 * mm->pal_size);
	if (env)
		DCFlushRange(pe, sizeof(float) * 12 * mm->pal_size);
	PROF_ADD(palette, t1);
	PROF_T(t2);
	GX_SetArray(GX_POSMTXARRAY, pp, sizeof(float) * 12);
	GX_SetArray(GX_NRMMTXARRAY, pn, sizeof(float) * 9);
	/* The lists load texture matrices by the same ids: each one's normal
	 * to the env texture (or, without one, nothing reads them: the
	 * palette). */
	GX_SetArray(GX_TEXMTXARRAY, env ? (void *) pe : (void *) pp, sizeof(float) * 12);
	GX_InvVtxCache();   /* matrices and vertices come through the vertex cache */

	if (env) {
		/* As the Mii Channel's outer and pants: no lamp, the colour (TEV
		 * register 0) plus the texel squared, the texel where the normal
		 * points as the camera sees it (the vertices' texture matrix
		 * indexes pick each bone's). */
		GX_InitTexObj(&tex, body->env.gx, body->env.w, body->env.h, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjLOD(&tex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
		GX_LoadTexObj(&tex, GX_TEXMAP0);
		GX_SetNumTexGens(1);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_NRM, GX_TEXMTX0);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_TEXC, GX_CC_C0);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	} else {
		/* No texture: no texture coordinate generated either (the vertices
		 * carry none; a texgen reading an input the vertex lacks is not a
		 * valid setup, though Dolphin quietly reads a default). */
		GX_SetNumTexGens(0);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	}
	for (k = 0; k < body->nobjs; ++k) {
		const mii_body_obj *o = &body->obj[k];
		const mii_body_array *pa = &body->pos_array[o->pos], *na = &body->nrm_array[o->nrm];
		uint32_t col = o->mesh ? model->pants : model->shirt;
		const mii_body_lod *l = pose->lod > 0 && pose->lod <= body->nlods ? &body->lod[pose->lod - 1] : NULL;

		if (env)
			GX_SetTevColor(GX_TEVREG0, (GXColor) { col >> 24, col >> 16, col >> 8, 255 });
		else
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor) { col >> 24, col >> 16, col >> 8, 255 });
		GX_ClearVtxDesc();
		GX_SetVtxDesc(GX_VA_PTNMTXIDX, GX_DIRECT);
		for (i = 0; i < 8; ++i)
			if (o->vcd >> (1 + i) & 1)
				GX_SetVtxDesc(GX_VA_TEX0MTXIDX + i, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_POS, (o->vcd >> 9 & 3) == 2 ? GX_INDEX8 : GX_INDEX16);
		GX_SetVtxDesc(GX_VA_NRM, (o->vcd >> 11 & 3) == 2 ? GX_INDEX8 : GX_INDEX16);
		GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, pa->fmt, pa->frac);
		GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_NRM, GX_NRM_XYZ, na->fmt, 0);
		GX_SetArray(GX_VA_POS, pa->gx, pa->stride);
		GX_SetArray(GX_VA_NRM, na->gx, na->stride);
		if (l && l->dl[k])
			GX_CallDispList(l->dl[k], l->dl_size[k]);
		else
			GX_CallDispList(o->dl, o->dl_size);
	}
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetCurrentMtx(GX_PNMTX0);

	PROF_ADD(body, t2);
	guMtxCopy(mm->last_head, head);
	mtx_mul(head, root, head);
	mtx_rot(t, 'y', pose->head_yaw);
	mtx_mul(head, head, t);
	mtx_rot(t, 'x', -pose->head_pitch);
	mtx_mul(head, head, t);
	mtx_rot(t, 'z', pose->head_roll);
	mtx_mul(head, head, t);
	{
		PROF_T(t3);
		draw_head(model, pose->expression, view, head, pose->lod);
		PROF_ADD(head, t3);
	}
	if (pose->shadow)
		draw_shadow(model, pose, view, root);
}

void mii_draw(const mii_model *model, const mii_pose *pose, Mtx view, Mtx place) {
	static const mii_pose still = { 0 };
	Mtx root, m, t, s, part;
	float sx, sy;
	int side;

	if (!model || !model->part[MII_PART_FACELINE].count)
		return;
	if (!pose)
		pose = &still;
#ifdef MII_PROFILE
	++mii_profile.draws;
	{
		PROF_T(ts);
		setup(view);
		PROF_ADD(setup, ts);
	}
#else
	setup(view);
#endif
	if (place)
		guMtxCopy(place, root);
	else
		guMtxIdentity(root);
	mtx_rot(m, 'y', pose->yaw);
	mtx_mul(root, root, m);
	mtx_trans(m, 0, pose->bob, 0);
	mtx_mul(root, root, m);

	if (pose->head_only) {
		draw_head(model, pose->expression, view, root, pose->lod);
		return;
	}
	if (model->body) {
		draw_body(model, pose, view, root);
		return;
	}
	sx = model->body_scale[0];
	sy = model->body_scale[1];

	/* Legs and shoes: from the hips, swinging about X. */
	for (side = 0; side < 2; ++side) {
		float x = side ? -model->hip[0] : model->hip[0];

		mtx_trans(t, x, model->hip[1], 0);
		mtx_mul(m, root, t);
		mtx_rot(t, 'x', pose->leg_swing[side]);
		mtx_mul(m, m, t);
		mtx_scale(s, sx, sy, sx);
		mtx_mul(part, m, s);
		load(view, part);
		draw_mesh(&model->part[MII_BODY_LEG], NULL);
		mtx_trans(t, 0, -model->hip[1] + model->shoe[0] * sy, model->shoe[1]);
		mtx_mul(part, m, t);
		mtx_scale(s, sx, sy, sx);
		mtx_mul(part, part, s);
		load(view, part);
		draw_mesh(&model->part[MII_BODY_SHOE], NULL);
	}
	/* Hips and torso; breathing fills the chest a little. */
	mtx_scale(s, sx, sy, sx);
	mtx_mul(part, root, s);
	load(view, part);
	draw_mesh(&model->part[MII_BODY_HIPS], NULL);
	mtx_scale(s, sx * (1 + 0.025f * pose->breathe), sy * (1 + 0.012f * pose->breathe),
			  sx * (1 + 0.04f * pose->breathe));
	mtx_mul(part, root, s);
	load(view, part);
	draw_mesh(&model->part[MII_BODY_TORSO], NULL);
	/* Arms: out to the side about Z, forward about X; a hand at the end. */
	for (side = 0; side < 2; ++side) {
		float x = side ? -model->shoulder[0] : model->shoulder[0];
		float raise = side ? -pose->arm_raise[side] : pose->arm_raise[side];

		mtx_trans(t, x, model->shoulder[1], model->shoulder[2]);
		mtx_mul(m, root, t);
		mtx_rot(t, 'z', raise + (side ? -0.12f : 0.12f));
		mtx_mul(m, m, t);
		mtx_rot(t, 'x', -pose->arm_swing[side]);
		mtx_mul(m, m, t);
		mtx_scale(s, sx, sy, sx);
		mtx_mul(part, m, s);
		load(view, part);
		draw_mesh(&model->part[MII_BODY_ARM], NULL);
		mtx_trans(t, 0, -model->arm_len * sy, 0);
		mtx_mul(part, m, t);
		mtx_scale(s, (sx + sy) / 2, (sx + sy) / 2, (sx + sy) / 2);
		mtx_mul(part, part, s);
		load(view, part);
		draw_mesh(&model->part[MII_BODY_HAND], NULL);
	}
	/* Neck and head. */
	mtx_trans(t, 0, model->head_y - 2, 0);
	mtx_mul(part, root, t);
	load(view, part);
	draw_mesh(&model->part[MII_BODY_NECK], NULL);
	mtx_trans(t, 0, model->head_y, 0);
	mtx_mul(m, root, t);
	mtx_rot(t, 'y', pose->head_yaw);
	mtx_mul(m, m, t);
	mtx_rot(t, 'x', -pose->head_pitch);
	mtx_mul(m, m, t);
	mtx_rot(t, 'z', pose->head_roll);
	mtx_mul(m, m, t);
	draw_head(model, pose->expression, view, m, pose->lod);
}

/* ---- Capture ------------------------------------------------------------------ */

static int capture(const mii_model *model, const mii_pose *pose, const mii_fx_state *fx, int w, int h,
				   float fovy, guVector eye, guVector target, float stretch_x, GXColor bg, GXColor clear,
				   void *dst) {
	int ss = 2 * w <= 640 && 2 * h <= 528 ? 2 : 1;   /* supersampled, box-filtered down */
	int ew = w * ss, eh = h * ss;
	Mtx44 proj;
	Mtx view;
	guVector up = { 0, 1, 0 };

	if (!model || !dst || w <= 0 || h <= 0 || w % 4 || h % 4 || ew > 640 || eh > 528 ||
			((uintptr_t) dst & 31))
		return MII_E_ARG;
	/* The capture needs alpha: RGBA6. A pixel-format change takes effect
	 * only after GX_PixModeSync(), so one follows each change. */
	GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
	GX_PixModeSync();
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
	GX_SetViewport(0, 0, ew, eh, 0, 1);
	GX_SetScissor(0, 0, ew, eh);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	/* Clear the area to the background: a copy that clears it (to dst, which
	 * the real copy overwrites below), Z too whatever the caller left (a
	 * copy clears Z only while Z writes are on). */
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetCopyClear(bg, 0x00FFFFFF);
	GX_SetTexCopySrc(0, 0, ew, eh);
	GX_SetTexCopyDst(w, h, GX_TF_RGBA8, ss > 1 ? GX_TRUE : GX_FALSE);
	GX_CopyTex(dst, GX_TRUE);
	GX_PixModeSync();

	guPerspective(proj, fovy, (float) w * stretch_x / h, 10, 4000);
	GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
	guLookAt(view, &eye, &up, &target);
	/* Every pixel the Mii covers is opaque in the copy. */
	GX_SetDstAlpha(GX_TRUE, 255);
	mii_draw(model, pose, view, NULL);
	if (fx)
		mii_fx_draw(fx, view);
	GX_SetDstAlpha(GX_FALSE, 0);

	GX_SetCopyClear(clear, 0x00FFFFFF);
	GX_SetTexCopySrc(0, 0, ew, eh);
	GX_SetTexCopyDst(w, h, GX_TF_RGBA8, ss > 1 ? GX_TRUE : GX_FALSE);
	DCInvalidateRange(dst, w * h * 4);
	GX_CopyTex(dst, GX_TRUE);
	GX_PixModeSync();
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_PixModeSync();   /* back to the caller's format before it draws again */
	GX_InvalidateTexAll();
	return MII_OK;
}

/* The whole Mii in view, from the front: fovy 20 degrees, the distance that
 * fits its height and width (with margin on each side) in aspect. */
static void frame(const mii_model *model, const mii_pose *pose, float aspect, float margin,
				  guVector *eye, guVector *target) {
	float top, half, t = tanf(20 * (float) M_PI / 360), need_h, need_w;

	mii_model_bounds(model, pose->head_only, &top, &half);
	need_h = top * (1 + 2 * margin) / 2;
	need_w = half * (1 + 2 * margin) / aspect;
	target->x = 0;
	target->y = top / 2;
	target->z = 0;
	*eye = *target;
	eye->z = (need_h > need_w ? need_h : need_w) / t;
}

int mii_view(const mii_model *model, const mii_pose *pose, float aspect, float margin, Mtx44 proj,
			 Mtx view) {
	static const mii_pose still = { 0 };
	guVector eye, target, up = { 0, 1, 0 };

	float top, half, r, near;

	if (!model || !proj || !view || !(aspect > 0))
		return MII_E_ARG;
	if (!pose)
		pose = &still;
	frame(model, pose, aspect, margin, &eye, &target);
	/* Near and far just around the Mii, so a depth range of any size spends
	 * its precision on the Mii: with 10 and 4000 a Mii 400 away had about
	 * 0.4% of the range, and a narrow viewport depth band left the face's
	 * layers (mask, nose line, glasses) a few dozen Z steps to fight over. */
	mii_model_bounds(model, pose->head_only, &top, &half);
	r = top + half;   /* more than the Mii reaches from its centre, any pose */
	near = eye.z - r > 1 ? eye.z - r : 1;
	guPerspective(proj, 20, aspect, near, eye.z + r);
	guLookAt(view, &eye, &up, &target);
	return MII_OK;
}

float mii_pixels_tall(const mii_model *model, Mtx view, Mtx place, Mtx44 proj, float viewport_h) {
	float top, half, at[3] = { 0, 0, 0 }, up = 1, z;
	int i;

	if (!model || !view || !proj)
		return 0;
	mii_model_bounds(model, 0, &top, &half);
	if (place) {
		/* The Mii's middle, and its height, as placed. */
		for (i = 0; i < 3; ++i)
			at[i] = place[i][1] * top / 2 + place[i][3];
		up = sqrtf(place[0][1] * place[0][1] + place[1][1] * place[1][1] + place[2][1] * place[2][1]);
	} else {
		at[1] = top / 2;
	}
	z = -(view[2][0] * at[0] + view[2][1] * at[1] + view[2][2] * at[2] + view[2][3]);
	if (z < 1)   /* at or behind the camera: as near as can be */
		return 1e9f;
	return top * up * proj[1][1] * viewport_h / 2 / z;
}

int mii_capture(const mii_model *model, const mii_pose *pose, int w, int h, float margin,
				float stretch_x, GXColor clear, void *dst) {
	static const mii_pose still = { 0 };
	guVector eye, target;

	if (!model || w <= 0 || h <= 0)
		return MII_E_ARG;
	if (!pose)
		pose = &still;
	if (stretch_x <= 0)
		stretch_x = 1;
	frame(model, pose, (float) w * stretch_x / h, margin, &eye, &target);
	return capture(model, pose, NULL, w, h, 20, eye, target, stretch_x,
				   (GXColor) { 0, 0, 0, 0 }, clear, dst);
}

int mii_icon(const mii_model *model, int expression, int size, GXColor background, void *dst) {
	return mii_icon_fx(model, expression, NULL, size, background, dst);
}

int mii_icon_fx(const mii_model *model, int expression, const mii_fx_state *fx, int size, GXColor background,
				void *dst) {
	/* RFLiMakeIcon: fovy 2 atan2(43.2, 500), the camera at (0, 34.5, 600)
	 * looking at (0, 34.5, 0). */
	mii_pose pose;
	guVector eye = { 0, 34.5f, 600 }, target = { 0, 34.5f, 0 };

	memset(&pose, 0, sizeof(pose));
	pose.head_only = 1;
	pose.expression = expression;
	return capture(model, &pose, fx, size, size, 2 * atan2f(43.2f, 500) * 180 / (float) M_PI, eye, target,
				   1, background, (GXColor) { 0, 0, 0, 255 }, dst);
}

#endif /* MII_NO_OGC */
