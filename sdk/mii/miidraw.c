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
	for (e = cache->head; e; e = next) {   /* any a model still holds: its model is gone */
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

static void tex_free(mii_tex *t) {
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

/* The mask texture for one expression (RFLiSetFaceParts, RFLiCapture). RFL
 * draws front to back (mustache, mouth, eyebrows, eyes, mole) with the
 * destination alpha as coverage; this composites the same parts back to
 * front. */
static int make_mask(const mii_res *res, const mii *m, int e, int size, mii_tex *out) {
	static const float ux = 0.88961464f, uy = 1.1600001f * 0.9276675f;
	expr_face f = expression(m, e);
	float unit = size / 64.0f, *buf;
	mii_tex eye, brow, mouth, mustache, mole;
	face_part p[8];
	int n = 0, i, r;
	float eye_x, eye_y, eye_w, eye_h, eye_a, brow_x, brow_y, brow_w, brow_h, brow_a;

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

	memset(p, 0, sizeof(p));
	if (m->mole) {
		p[n].x = (17.766165f + 2 * ux * m->mole_x) * unit;
		p[n].y = (17.95986f + uy * m->mole_y) * unit;
		p[n].w = p[n].h = (1 + 0.4f * m->mole_scale) * unit;
		p[n].origin = O_CENTER;
		p[n].kind = K_FLAT;
		p[n].tex = &mole;
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
		p[n].tex = &eye;
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
		p[n].tex = &brow;
		color3(p[n].c0, hair_colors[pick(m->brow_color, 8)]);
	}
	p[n].x = 32 * unit;
	p[n].y = (29.25885f + uy * m->mouth_y) * unit;
	p[n].w = 396 / 64.0f * (1 + 0.4f * m->mouth_scale) * unit;
	p[n].h = 288 / 64.0f * (1 + 0.4f * m->mouth_scale) * unit;
	p[n].origin = O_CENTER;
	p[n].kind = K_MOUTH;
	p[n].tex = &mouth;
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
			p[n].tex = &mustache;
			color3(p[n].c0, hair_colors[pick(m->beard_color, 8)]);
		}
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

#ifndef MII_NO_OGC
/* A packed mesh as a GX display list, written as bytes (so any thread can
 * build it): GX_TRIANGLES on VTXFMT0, a big-endian u16 corner count, then
 * each corner's index into the position, normal and coordinate arrays (8
 * bits each where there are under 256 vertices, else 16), padded with
 * GX_NOPs to 32 bytes. One draw command holds at most 65535 corners. The
 * floats and the corner list then go: the list and the arrays are all the
 * GPU reads. */
static int mesh_list(mii_mesh *mesh) {
	uint32_t size = 0, done = 0, w = mesh->nverts < 256 ? 1 : 2;
	uint8_t *o;

	if (!mesh->count)
		return MII_OK;
	size = 3 * ((mesh->count + 65534) / 65535) + (uint32_t) mesh->count * 3 * w;
	size = (size + 31) & ~31u;
	mesh->dl = memalign(32, size);
	if (!mesh->dl)
		return MII_E_MEM;
	memset(mesh->dl, 0, size);   /* 0: GX_NOP */
	o = mesh->dl;
	while (done < (uint32_t) mesh->count) {
		uint32_t n = mesh->count - done > 65535 ? 65535 : mesh->count - done, i, k;

		*o++ = GX_TRIANGLES | GX_VTXFMT0;
		*o++ = (uint8_t) (n >> 8);
		*o++ = (uint8_t) n;
		for (i = 0; i < n; ++i)
			for (k = 0; k < 3; ++k) {
				uint16_t x = mesh->idx[done + i];

				if (w == 2)
					*o++ = (uint8_t) (x >> 8);
				*o++ = (uint8_t) x;
			}
		done += n;
	}
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

static int mesh_share(mii_cache *cache, mii_mesh *mesh) {
	uint32_t pack = mesh->pack_st + (((uint32_t) mesh->nverts * 4 + 31) & ~31u);

	if (!mesh->count)
		return MII_OK;
#ifndef MII_NO_OGC
	return share_bufs(cache, (void **) &mesh->pack, pack, &mesh->dl, mesh->dl_size, &mesh->shared);
#else
	return share_bufs(cache, (void **) &mesh->pack, pack, (void **) &mesh->idx,
					  sizeof(uint16_t) * mesh->count, &mesh->shared);
#endif
}

/* The faces of the expressions asked for, at that size, uploaded and shared. */
static int make_masks(const mii_res *res, mii_model *model, int size, unsigned exprs, mii_tex *out) {
	int e, r;

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
	mii_model_opts o = { mask_size, 0, NULL };

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
	out->part[MII_PART_FOREHEAD].color = rgba(skin_colors[pick(m->face_color, 6)]);
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
	out->part[MII_PART_NOSE].color = rgba(skin_colors[pick(m->face_color, 6)]);
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
	free(model->last_pm);
	free(model->last_nm);
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

static void load(Mtx view, Mtx model) {
	Mtx mv, inv, nrm;

	guMtxConcat(view, model, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	if (guMtxInverse(mv, inv))
		guMtxTranspose(inv, nrm);
	else
		guMtxCopy(mv, nrm);
	GX_LoadNrmMtxImm(nrm, GX_PNMTX0);
}

static void draw_mesh(const mii_mesh *mesh, const GXTexObj *tex) {
	int w = mesh->nverts < 256 ? GX_INDEX8 : GX_INDEX16;

	if (!mesh->count || !mesh->dl)
		return;
	if (tex) {
		GX_LoadTexObj((GXTexObj *) tex, GX_TEXMAP0);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor) { 255, 255, 255, 255 });
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
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
	GX_CallDispList(mesh->dl, mesh->dl_size);
}

static void setup(Mtx view) {
	GXLightObj light;
	guVector pos = { -900, 1100, 1400 }, vpos;

	GX_InvVtxCache();   /* the arrays may be new at an address a freed model used */

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

static void draw_head(const mii_model *model, int expression, Mtx view, Mtx head) {
	static const int opaque[] = { MII_PART_BEARD, MII_PART_NOSE, MII_PART_FOREHEAD, MII_PART_HAIR,
								  MII_PART_CAP, MII_PART_FACELINE };
	static const int translucent[] = { MII_PART_MASK, MII_PART_NOSELINE, MII_PART_GLASS };
	unsigned i;

	load(view, head);
	for (i = 0; i < sizeof(opaque) / sizeof(opaque[0]); ++i) {
		const mii_mesh *p = &model->part[opaque[i]];

		draw_mesh(p, p->tex >= 0 && p->tex < MII_TEX_COUNT ? &model->tex[p->tex].obj : NULL);
	}
	/* The layers over the face test Z but write none: drawn just in front of
	 * the faceline, they must not fight it or each other. */
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	for (i = 0; i < sizeof(translucent) / sizeof(translucent[0]); ++i) {
		const mii_mesh *p = &model->part[translucent[i]];

		if (!p->count)
			continue;
		if (translucent[i] == MII_PART_GLASS)
			GX_SetCullMode(GX_CULL_NONE);
		if (expression < 0 || expression >= MII_EXPR_COUNT || !model->mask[expression].gx)
			expression = MII_EXPR_NORMAL;   /* not built (mii_model_opts.expressions) */
		draw_mesh(p, translucent[i] == MII_PART_MASK ? &model->mask[expression].obj :
				  &model->tex[p->tex].obj);
	}
	GX_SetCullMode(GX_CULL_BACK);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
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
	float (*bp)[3][4] = mm->last_pm, (*bn)[3][3] = mm->last_nm;
	float (*pp)[3][4] = mm->pal_pos + mm->pal_size * mm->pal_next;
	float (*pn)[3][3] = mm->pal_nrm + mm->pal_size * mm->pal_next;
	Mtx mv, inv, nmv, head, t;
	int i, j, k, id;

	mm->pal_next = (mm->pal_next + 1) % MII_PALETTES;
	if (!pose->reuse || !mm->posed) {   /* pose it: once, for the body and the head */
		float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3];
		const float (*b)[3] = body->bone[body->head].bind_rot;

		mii_body_pose(model, pose, rot, pos);
		mii_body_matrices_posed(model, rot, pos, bp, bn);
		for (i = 0; i < 3; ++i) {
			for (j = 0; j < 3; ++j) {
				float v = 0;

				for (k = 0; k < 3; ++k)
					v += rot[body->head][i][k] * b[j][k];   /* R_head times the bind's transpose */
				mm->last_head[i][j] = v;
			}
			mm->last_head[i][3] = pos[body->head][i];
		}
		mm->posed = 1;
	}
	guMtxConcat(view, root, mv);
	if (guMtxInverse(mv, inv))
		guMtxTranspose(inv, nmv);
	else
		guMtxCopy(mv, nmv);
	for (id = 0; id < body->nmatrices; ++id) {
		int bone = body->matrix_bone[id];

		if (bone < 0) {   /* an id no bone has: never drawn with, kept harmless */
			guMtxCopy(mv, pp[id]);
			for (i = 0; i < 3; ++i)
				for (j = 0; j < 3; ++j)
					pn[id][i][j] = nmv[i][j];
			continue;
		}
		guMtxConcat(mv, bp[bone], pp[id]);
		for (i = 0; i < 3; ++i)
			for (j = 0; j < 3; ++j)
				pn[id][i][j] = nmv[i][0] * bn[bone][0][j] + nmv[i][1] * bn[bone][1][j] +
							   nmv[i][2] * bn[bone][2][j];
	}
	DCFlushRange(pp, sizeof(float) * 12 * mm->pal_size);
	DCFlushRange(pn, sizeof(float) * 9 * mm->pal_size);
	GX_SetArray(GX_POSMTXARRAY, pp, sizeof(float) * 12);
	GX_SetArray(GX_NRMMTXARRAY, pn, sizeof(float) * 9);
	/* The lists load texture matrices by the same ids; nothing uses them
	 * (no texgen here), but the GPU still reads them: from the palette. */
	GX_SetArray(GX_TEXMTXARRAY, pp, sizeof(float) * 12);
	GX_InvVtxCache();   /* matrices and vertices come through the vertex cache */

	/* No texture: no texture coordinate generated either (the vertices
	 * carry none; a texgen reading an input the vertex lacks is not a
	 * valid setup, though Dolphin quietly reads a default). */
	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	for (k = 0; k < body->nobjs; ++k) {
		const mii_body_obj *o = &body->obj[k];
		const mii_body_array *pa = &body->pos_array[o->pos], *na = &body->nrm_array[o->nrm];
		uint32_t col = o->mesh ? model->pants : model->shirt;

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
		GX_CallDispList(o->dl, o->dl_size);
	}
	GX_SetNumTexGens(1);
	GX_SetCurrentMtx(GX_PNMTX0);

	guMtxCopy(mm->last_head, head);
	mtx_mul(head, root, head);
	mtx_rot(t, 'y', pose->head_yaw);
	mtx_mul(head, head, t);
	mtx_rot(t, 'x', -pose->head_pitch);
	mtx_mul(head, head, t);
	mtx_rot(t, 'z', pose->head_roll);
	mtx_mul(head, head, t);
	draw_head(model, pose->expression, view, head);
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
	setup(view);
	if (place)
		guMtxCopy(place, root);
	else
		guMtxIdentity(root);
	mtx_rot(m, 'y', pose->yaw);
	mtx_mul(root, root, m);
	mtx_trans(m, 0, pose->bob, 0);
	mtx_mul(root, root, m);

	if (pose->head_only) {
		draw_head(model, pose->expression, view, root);
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
	draw_head(model, pose->expression, view, m);
}

/* ---- Capture ------------------------------------------------------------------ */

static int capture(const mii_model *model, const mii_pose *pose, int w, int h, float fovy,
				   guVector eye, guVector target, float stretch_x, GXColor bg, GXColor clear,
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
	 * the real copy overwrites below). */
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
	return capture(model, pose, w, h, 20, eye, target, stretch_x,
				   (GXColor) { 0, 0, 0, 0 }, clear, dst);
}

int mii_icon(const mii_model *model, int expression, int size, GXColor background, void *dst) {
	/* RFLiMakeIcon: fovy 2 atan2(43.2, 500), the camera at (0, 34.5, 600)
	 * looking at (0, 34.5, 0). */
	mii_pose pose;
	guVector eye = { 0, 34.5f, 600 }, target = { 0, 34.5f, 0 };

	memset(&pose, 0, sizeof(pose));
	pose.head_only = 1;
	pose.expression = expression;
	return capture(model, &pose, size, size, 2 * atan2f(43.2f, 500) * 180 / (float) M_PI, eye, target,
				   1, background, (GXColor) { 0, 0, 0, 255 }, dst);
}

#endif /* MII_NO_OGC */
