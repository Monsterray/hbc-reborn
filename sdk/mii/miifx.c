/*
 * miifx.c - the Mii Channel's particle effects (miidraw.h). Public domain.
 *
 * Reads effect_nigaoe.breff and effect_nigaoe.breft from the Mii Channel's
 * Character.szs (NW4R's EffectSystem files, REFF and REFT version 5), plays
 * what they use and draws the particles as billboards. The layouts were
 * worked out from the files' bytes, with the News Channel's and Wii Sports'
 * decompilations and BrawlCrate for the names and the maths. The files are
 * the user's own copies.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MII_NO_OGC
#include <malloc.h>
#include <ogc/cache.h>
#endif

#include "miidraw.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Bytes ---------------------------------------------------------------- */

typedef struct {
	const uint8_t *d;
	uint32_t size;
	int bad;               /* an offset fell outside the file */
} buf;

static int in(buf *b, uint32_t at, uint32_t n) {
	if (at > b->size || n > b->size - at) {
		b->bad = 1;
		return 0;
	}
	return 1;
}

static uint32_t rd32(buf *b, uint32_t at) {
	const uint8_t *p;

	if (!in(b, at, 4))
		return 0;
	p = b->d + at;
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static uint16_t rd16(buf *b, uint32_t at) {
	if (!in(b, at, 2))
		return 0;
	return (uint16_t) (b->d[at] << 8 | b->d[at + 1]);
}

static uint8_t rd8(buf *b, uint32_t at) {
	return in(b, at, 1) ? b->d[at] : 0;
}

/* A float, NaN and the absurd read as 0: nothing after it has to check. */
static float rdf(buf *b, uint32_t at) {
	uint32_t u = rd32(b, at);
	float f;

	memcpy(&f, &u, 4);
	return f == f && f > -1e6f && f < 1e6f ? f : 0;
}

/* A name of len bytes (with its NUL) at `at`, ending by `end`, into out
 * (cap bytes). 0, or -1 if it does not fit or is not terminated. */
static int rdname(buf *b, uint32_t at, uint32_t len, uint32_t end, char *out, uint32_t cap) {
	if (len < 1 || len > cap || end > b->size || at > end || len > end - at || b->d[at + len - 1])
		return -1;
	memcpy(out, b->d + at, len);
	return 0;
}

/* ---- The files' frame ------------------------------------------------------- */

/* A REFF or REFT (version 5, one block): its name table's place and end, and
 * how many entries it has, or -1. */
static int name_table(buf *b, const char *magic, uint32_t *table, uint32_t *end) {
	uint32_t hs, proj, phs, nt, ts;

	if (b->size < 0x20 || memcmp(b->d, magic, 4) || rd16(b, 4) != 0xFEFF || rd16(b, 6) != 5 ||
			rd32(b, 8) > b->size)
		return -1;
	hs = rd16(b, 0x0C);
	if (hs < 0x10 || !in(b, hs, 0x10) || memcmp(b->d + hs, magic, 4))
		return -1;
	proj = hs + 8;
	phs = rd32(b, proj);
	if (phs < 0x10 || phs > 0x400)
		return -1;
	nt = proj + phs;
	ts = rd32(b, nt);
	if (ts < 8 || !in(b, nt, ts))
		return -1;
	*table = nt;
	*end = nt + ts;
	return rd16(b, nt + 4);
}

/* The name table's next entry at *at (they are packed): its name and its
 * data, inside the file. 0 or -1. */
static int entry(buf *b, uint32_t nt, uint32_t end, uint32_t *at, char *name, uint32_t cap, uint32_t *data,
				 uint32_t *size) {
	uint32_t q = *at, len = rd16(b, q), off;

	if (rdname(b, q + 2, len, end, name, cap) || end - q - 2 - len < 8)
		return -1;
	off = rd32(b, q + 2 + len);
	*size = rd32(b, q + 6 + len);
	if (off > b->size - nt || !in(b, nt + off, *size))
		return -1;
	*data = nt + off;
	*at = q + 10 + len;
	return 0;
}

/* Room for one more of an array grown by doubling. 0 or -1. */
static int grow(void **p, int *cap, int need, size_t each) {
	void *n;
	int c;

	if (need <= *cap)
		return 0;
	if (need > 0x100000)
		return -1;
	for (c = *cap ? *cap : 16; c < need; c *= 2)
		;
	n = realloc(*p, (size_t) c * each);
	if (!n)
		return -1;
	*p = n;
	*cap = c;
	return 0;
}

/* ---- Textures (the BREFT) ---------------------------------------------------- */

/* Bytes in a texture's first level, or 0 for a format not drawn (the
 * palette ones: no palettes in these files). */
static uint32_t level_size(int fmt, uint32_t w, uint32_t h) {
	static const uint8_t bw[] = { 8, 8, 8, 4, 4, 4, 4 }, bh[] = { 8, 4, 4, 4, 4, 4, 4 },
		bits[] = { 4, 8, 8, 16, 16, 16, 32 };
	uint32_t x, y, p;

	if (fmt == 14) {   /* CMPR */
		x = 8; y = 8; p = 4;
	} else if (fmt >= 0 && fmt <= 6) {
		x = bw[fmt]; y = bh[fmt]; p = bits[fmt];
	} else {
		return 0;
	}
	return (w + x - 1) / x * x * ((h + y - 1) / y * y) * p / 8;
}

/* A copy the GPU reads: 32-byte aligned, out of the cache on a Wii. */
static void *gpu_copy(const uint8_t *src, uint32_t n) {
	uint32_t size = (n + 31) & ~31u;
	uint8_t *p;

#ifndef MII_NO_OGC
	p = memalign(32, size);
#else
	p = malloc(size);
#endif
	if (!p)
		return NULL;
	memset(p, 0, size);
	memcpy(p, src, n);
#ifndef MII_NO_OGC
	DCFlushRange(p, size);
#endif
	return p;
}

static int read_textures(mii_fx *fx, buf *b) {
	uint32_t nt, end, at, data, size;
	int n, i;

	n = name_table(b, "REFT", &nt, &end);
	if (n < 0 || n > MII_FX_TEXTURES)
		return MII_E_FORMAT;
	at = nt + 8;
	for (i = 0; i < n; ++i) {
		mii_fx_tex *t = &fx->tex[i];
		uint32_t w, h, lsz;

		if (entry(b, nt, end, &at, t->name, sizeof(t->name), &data, &size) || !in(b, data, 0x20))
			return MII_E_FORMAT;
		fx->ntex = i + 1;
		/* the header: name pointer, width, height, data size, format, palette, mipmaps */
		w = rd16(b, data + 4);
		h = rd16(b, data + 6);
		t->fmt = rd8(b, data + 0x0C);
		if (!w || !h || w > 1024 || h > 1024)
			return MII_E_FORMAT;
		t->w = (uint16_t) w;
		t->h = (uint16_t) h;
		lsz = level_size(t->fmt, w, h);
		if (!lsz)
			continue;   /* kept by name, not drawn */
		if (rd32(b, data + 8) < lsz || !in(b, data + 0x20, lsz))
			return MII_E_FORMAT;
		t->size = lsz;
		t->gx = gpu_copy(b->d + data + 0x20, lsz);
		if (!t->gx)
			return MII_E_MEM;
	}
	return 0;
}

static int find_tex(const mii_fx *fx, const char *name) {
	int i;

	for (i = 0; i < fx->ntex; ++i)
		if (!strcmp(fx->tex[i].name, name))
			return i;
	return -1;
}

/* ---- Curves --------------------------------------------------------------- */

enum {
	K_COLOR0 = 0, K_ALPHA0 = 1, K_COLOR0S = 2, K_ALPHA0S = 3, K_SIZE = 10, K_SCALE = 11, K_ROTATE = 12,
	K_TEXTURE = 14, K_CHILD = 26, K_GRAVITY = 32, K_SPEED = 33, K_VORTEX = 36, K_SPIN = 38,
	K_EMIT_SCALE = 65, K_EMIT_ROTATE = 66, K_EMIT_TRANSLATE = 67, K_EMIT_EMISSION = 73, K_OFF = 0xFF
};
#define C_LOOP 0x20
#define C_TURN 0x40
#define C_FIT 0x80

/* Whether a curve of this kind and type is one played here. */
static int kind_ok(int kind, int type) {
	switch (kind) {
	case K_COLOR0: case K_ALPHA0: case K_COLOR0S: case K_ALPHA0S:
		return type == 0;
	case K_ROTATE: case K_EMIT_ROTATE:
		return type == 3 || type == 6;
	case K_SIZE: case K_SCALE: case K_GRAVITY: case K_SPEED: case K_VORTEX: case K_SPIN:
	case K_EMIT_SCALE: case K_EMIT_TRANSLATE: case K_EMIT_EMISSION:
		return type == 3;
	case K_TEXTURE:
		return type == 4;
	case K_CHILD:
		return type == 5;
	}
	return 0;
}

static int bits6(unsigned mask) {
	int n = 0;

	for (mask &= 0x3F; mask; mask &= mask - 1)
		++n;
	return n;
}

/* One curve at c (size bytes): its header (0x20), then the key, range,
 * random, name and info tables. Appended to fx's curves. */
static int read_curve(mii_fx *fx, buf *b, uint32_t c, uint32_t size, int *ccap, int *kcap, int *rcap) {
	mii_fx_curve *cv;
	uint32_t t[5], sum = 0x20, at, vb, ksz, rsz = 0;
	int i, j, k, n, pc, type;

	if (size < 0x20 || grow((void **) &fx->curve, ccap, fx->ncurves + 1, sizeof(mii_fx_curve)))
		return size < 0x20 ? MII_E_FORMAT : MII_E_MEM;
	cv = &fx->curve[fx->ncurves++];
	memset(cv, 0, sizeof(*cv));
	for (i = 0; i < MII_FX_NAMES; ++i)
		cv->ref[i] = -1;
	for (i = 0; i < 5; ++i) {
		t[i] = rd32(b, c + 0x0C + 4 * i);
		if (t[i] > size - sum)
			return MII_E_FORMAT;
		sum += t[i];
	}
	type = rd8(b, c + 2);
	cv->kind = rd8(b, c + 1);
	cv->type = (uint8_t) type;
	cv->mask = rd8(b, c + 3) & 0x3F;
	cv->flags = rd8(b, c + 4);
	cv->loop = rd8(b, c + 5);
	cv->frames = rd16(b, c + 8);
	cv->key = (uint32_t) fx->nkeys;
	cv->range = (uint32_t) fx->nranges;
	pc = bits6(cv->mask);
	if (rd8(b, c) != 0xAC || !kind_ok(cv->kind, type) || (type == 0 && pc > 4)) {
		cv->kind = K_OFF;   /* baked, or not one played here */
		return 0;
	}
	vb = type == 0 ? (uint32_t) pc : type == 4 ? 4 : type == 5 ? 10 : 4 * (uint32_t) pc;
	ksz = (8 + vb + 1) & ~1u;
	at = c + 0x20;
	if (t[0] >= 4) {   /* keys: frame, interpolation, pad, random, pad, value */
		n = rd16(b, at);
		if ((uint32_t) n * ksz > t[0] - 4 || grow((void **) &fx->key, kcap, fx->nkeys + n, sizeof(mii_fx_key)))
			return (uint32_t) n * ksz > t[0] - 4 ? MII_E_FORMAT : MII_E_MEM;
		for (k = 0; k < n; ++k) {
			mii_fx_key *key = &fx->key[fx->nkeys + k];
			uint32_t q = at + 4 + (uint32_t) k * ksz, v = q + 8;

			memset(key, 0, sizeof(*key));
			key->frame = rd16(b, q);
			key->interp = rd16(b, q + 2) & 3;
			if (type == 4) {          /* wrap, reverse, the texture's name */
				key->ref = rd16(b, v + 2);
			} else if (type == 5) {   /* speed, scale, alpha, colour, weight, kind, flag; the emitter's name */
				key->child = rd8(b, v + 6);
				key->ref = rd16(b, v + 8);
			} else if (rd8(b, q + 6) & 1) {
				key->random = 1;
				key->ref = rd16(b, v);
			} else {
				for (i = j = 0; i < 6; ++i)
					if (cv->mask >> i & 1) {
						key->v[i] = type == 0 ? rd8(b, v + j) : rdf(b, v + 4 * j);
						++j;
					}
			}
		}
		fx->nkeys += n;
		cv->nkeys = (uint16_t) n;
	}
	at += t[0];
	if (type == 0)
		rsz = 2 * (uint32_t) pc;
	else if (type == 3)
		rsz = 8 * (uint32_t) pc;
	else if (type == 6)
		rsz = 8 * (uint32_t) pc + 4;
	if (t[1] >= 4 && rsz) {   /* ranges: base and range per component (a rotation's, then a sign) */
		n = rd16(b, at);
		if ((uint32_t) n * rsz > t[1] - 4 || grow((void **) &fx->range, rcap, fx->nranges + n, sizeof(mii_fx_range)))
			return (uint32_t) n * rsz > t[1] - 4 ? MII_E_FORMAT : MII_E_MEM;
		for (k = 0; k < n; ++k) {
			mii_fx_range *r = &fx->range[fx->nranges + k];
			uint32_t q = at + 4 + (uint32_t) k * rsz;

			memset(r, 0, sizeof(*r));
			for (i = j = 0; i < 6; ++i)
				if (cv->mask >> i & 1) {
					if (type == 0) {   /* base +- range */
						float base = rd8(b, q + 2 * j), range = rd8(b, q + 2 * j + 1);

						r->lo[i] = base - range;
						r->span[i] = 2 * range;
					} else {           /* base + range * a 16-bit random number */
						r->lo[i] = rdf(b, q + 8 * j);
						r->span[i] = rdf(b, q + 8 * j + 4) * 65536;
					}
					++j;
				}
			if (type == 6)
				r->sign = rd8(b, q + 8 * pc);
		}
		fx->nranges += n;
		cv->nranges = (uint16_t) n;
	}
	for (k = 0; k < cv->nkeys; ++k) {   /* a random key without its entry: no random */
		mii_fx_key *key = &fx->key[cv->key + k];

		if (key->random && key->ref >= cv->nranges)
			key->random = 0;
	}
	at += t[1] + t[2];   /* the random table: not used by these files */
	if (t[3] >= 4) {   /* names: runtime words, then each a u16 length and the name */
		uint32_t q;

		n = rd16(b, at);
		if ((uint32_t) n > (t[3] - 4) / 4)
			return MII_E_FORMAT;
		q = at + 4 + 4 * (uint32_t) n;
		for (k = 0; k < n; ++k) {
			char name[32];
			uint32_t len = rd16(b, q);

			if (rdname(b, q + 2, len, at + t[3], name, sizeof(name)))
				return MII_E_FORMAT;
			if (k < MII_FX_NAMES)
				cv->ref[k] = (int16_t) (type == 4 ? find_tex(fx, name) : mii_fx_find(fx, name));
			q += 2 + len;
		}
	}
	at += t[3];
	if (cv->kind == K_GRAVITY && t[4] >= 0x12) {   /* power, direction, coord, target */
		for (i = 0; i < 3; ++i)
			cv->dir[i] = rdf(b, at + 4 + 4 * i);
		cv->coord = rd8(b, at + 0x10);
		cv->target = rd8(b, at + 0x11);
	} else if (cv->kind == K_SPIN && t[4] >= 0x12) {   /* speed, rotation, coord, target */
		cv->coord = rd8(b, at + 0x10);
		cv->target = rd8(b, at + 0x11);
	} else if (cv->kind == K_VORTEX && t[4] >= 0x1A) {   /* inner, outer, distance, axis, coord, target */
		cv->coord = rd8(b, at + 0x18);
		cv->target = rd8(b, at + 0x19);
	}
	return b->bad ? MII_E_FORMAT : 0;
}

/* ---- Emitters (the BREFF) ---------------------------------------------------- */

/* One emitter's entry, b being just that entry: its header, EmitterDesc
 * (0x140 at 8), ParticleParameterDesc (its size at 0x148, then it), then the
 * curves. */
static int read_emitter(mii_fx *fx, buf *b, mii_fx_emitter *e, int *ccap, int *kcap, int *rcap) {
	const uint32_t d = 8, ds = d + 0xA0, p = 0x14C;
	uint32_t sizes[2][MII_FX_TRACKS], c, at, len, psz = rd32(b, 0x148);
	int n[2], init[2], l, i, r;
	char tex[32];

	/* the particle's size varies with its texture names' lengths */
	if (rd32(b, 4) != 0x140 || psz < 0x7C || psz > 0x400 || !in(b, 0, p + psz + 8))
		return MII_E_FORMAT;
	e->common = rd32(b, d);
	e->emit_flag = rd32(b, d + 4);
	e->emit_life = rd16(b, d + 8);
	e->ptcl_life = rd16(b, d + 0x0A);
	e->life_rnd = (int8_t) rd8(b, d + 0x0C);
	e->emit_rnd = (int8_t) rd8(b, d + 0x0F);
	e->emit = rdf(b, d + 0x10);
	e->start = rd16(b, d + 0x14);
	e->interval = rd16(b, d + 0x18);
	for (i = 0; i < 6; ++i)
		e->param[i] = rdf(b, d + 0x1C + 4 * i);
	e->div = rd16(b, d + 0x34);
	e->vel_rnd = (int8_t) rd8(b, d + 0x36);
	e->v_radial = rdf(b, d + 0x38);
	e->v_yaxis = rdf(b, d + 0x3C);
	e->v_random = rdf(b, d + 0x40);
	e->v_normal = rdf(b, d + 0x44);
	e->v_normal_cone = rdf(b, d + 0x48);
	e->v_spec = rdf(b, d + 0x4C);
	e->v_spec_cone = rdf(b, d + 0x50);
	for (i = 0; i < 3; ++i) {
		e->spec_dir[i] = rdf(b, d + 0x54 + 4 * i);
		e->scale[i] = rdf(b, d + 0x60 + 4 * i);
		e->rot[i] = rdf(b, d + 0x6C + 4 * i);
		e->trans[i] = rdf(b, d + 0x78 + 4 * i);
	}
	e->seed = rd32(b, d + 0x88);
	e->bb = rd8(b, d + 0x8D);
	/* the draw setting */
	e->draw = rd16(b, ds);
	e->cmp[0] = rd8(b, ds + 2) & 7;
	e->cmp[1] = rd8(b, ds + 3) & 7;
	e->cmp[2] = rd8(b, ds + 4) & 3;
	e->blend[0] = rd8(b, ds + 0x58) & 3;
	e->blend[1] = rd8(b, ds + 0x59) & 7;
	e->blend[2] = rd8(b, ds + 0x5A) & 7;
	e->blend[3] = rd8(b, ds + 0x5B) & 15;
	e->zfunc = rd8(b, ds + 0x64) & 7;
	/* the particle */
	for (i = 0; i < 4; ++i) {
		e->color[0][i] = rd8(b, p + i);
		e->color[1][i] = rd8(b, p + 4 + i);
	}
	for (i = 0; i < 2; ++i) {
		e->size[i] = rdf(b, p + 0x10 + 4 * i);
		e->pscale[i] = rdf(b, p + 0x18 + 4 * i);
	}
	for (i = 0; i < 3; ++i)
		e->prot[i] = rdf(b, p + 0x20 + 4 * i);
	e->wrap = rd16(b, p + 0x74);
	e->ref[0] = rd8(b, p + 0x77);
	e->ref[1] = rd8(b, p + 0x78);
	e->size_rnd = (int8_t) rd8(b, p + 0x79);
	len = rd16(b, p + 0x7A);   /* the first texture's name (the others are not drawn) */
	e->tex = -1;
	if (len > 1) {
		if (rdname(b, p + 0x7C, len, p + psz, tex, sizeof(tex)))
			return MII_E_FORMAT;
		e->tex = find_tex(fx, tex);
	}
	/* the curves: particle, then emitter; counts, init counts, runtime words, sizes */
	c = p + psz;
	for (l = 0; l < 2; ++l) {
		n[l] = rd16(b, c);
		init[l] = rd16(b, c + 2);
		if (n[l] > MII_FX_TRACKS || init[l] > n[l])
			return MII_E_FORMAT;
		for (i = 0; i < n[l]; ++i)
			sizes[l][i] = rd32(b, c + 4 + 4 * (uint32_t) n[l] + 4 * (uint32_t) i);
		c += 4 + 8 * (uint32_t) n[l];
	}
	if (b->bad)
		return MII_E_FORMAT;
	e->track = (uint32_t) fx->ncurves;
	e->ntracks = n[0];
	e->ninit = init[0];
	e->nemit_tracks = n[1];
	for (l = 0; l < 2; ++l)
		for (i = 0; i < n[l]; ++i) {
			at = c;
			if (!in(b, at, sizes[l][i]))
				return MII_E_FORMAT;
			if ((r = read_curve(fx, b, at, sizes[l][i], ccap, kcap, rcap)) < 0)
				return r;
			c += sizes[l][i];
		}
	return b->bad ? MII_E_FORMAT : 0;
}

int mii_fx_find(const mii_fx *fx, const char *name) {
	int i;

	for (i = 0; fx && name && i < fx->nemitters; ++i)
		if (!strcmp(fx->emitter[i].name, name))
			return i;
	return -1;
}

void mii_fx_free(mii_fx *fx) {
	int i;

	if (!fx)
		return;
	for (i = 0; i < fx->ntex; ++i)
		free(fx->tex[i].gx);
	free(fx->emitter);
	free(fx->curve);
	free(fx->key);
	free(fx->range);
	memset(fx, 0, sizeof(*fx));
}

int mii_fx_attach(mii_fx *fx, const void *breff, size_t breff_size, const void *breft, size_t breft_size) {
	buf b, t;
	uint32_t nt, end, at, data, size;
	int n, i, r = MII_E_FORMAT, ccap = 0, kcap = 0, rcap = 0;

	if (!fx || !breff)
		return MII_E_ARG;
	memset(fx, 0, sizeof(*fx));
	if (breff_size > 0x1000000 || breft_size > 0x1000000)
		return MII_E_FORMAT;
	b.d = breff;
	b.size = (uint32_t) breff_size;
	b.bad = 0;
	if (breft) {   /* the textures first: emitters name theirs */
		t.d = breft;
		t.size = (uint32_t) breft_size;
		t.bad = 0;
		if ((r = read_textures(fx, &t)) < 0)
			goto fail;
		r = MII_E_FORMAT;
	}
	n = name_table(&b, "REFF", &nt, &end);
	if (n <= 0 || n > MII_FX_EMITTERS)
		goto fail;
	fx->emitter = calloc((size_t) n, sizeof(mii_fx_emitter));
	if (!fx->emitter) {
		r = MII_E_MEM;
		goto fail;
	}
	at = nt + 8;   /* the names first: child curves name emitters */
	for (i = 0; i < n; ++i)
		if (entry(&b, nt, end, &at, fx->emitter[i].name, sizeof(fx->emitter[i].name), &data, &size))
			goto fail;
	fx->nemitters = n;
	at = nt + 8;
	for (i = 0; i < n; ++i) {
		buf eb;
		char name[32];

		entry(&b, nt, end, &at, name, sizeof(name), &data, &size);
		eb.d = b.d + data;
		eb.size = size;
		eb.bad = 0;
		if ((r = read_emitter(fx, &eb, &fx->emitter[i], &ccap, &kcap, &rcap)) < 0)
			goto fail;
	}
	return 0;
fail:
	mii_fx_free(fx);
	return r;
}

static int read_file(const char *path, void **data, size_t *size) {
	FILE *f = fopen(path, "rb");
	long n;

	*data = NULL;
	if (!f)
		return errno == ENOENT || errno == ENOTDIR ? MII_E_NO_FILE : MII_E_IO;
	if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || n > 0x1000000 || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return MII_E_FORMAT;
	}
	*data = malloc(n);
	if (!*data) {
		fclose(f);
		return MII_E_MEM;
	}
	if (fread(*data, 1, n, f) != (size_t) n) {
		fclose(f);
		free(*data);
		*data = NULL;
		return MII_E_IO;
	}
	fclose(f);
	*size = (size_t) n;
	return 0;
}

int mii_fx_load(mii_fx *fx, const char *breff_path, const char *breft_path) {
	void *eff, *tex;
	size_t esize = 0, tsize = 0;
	int r;

	if (!fx)
		return MII_E_ARG;
	memset(fx, 0, sizeof(*fx));
	if ((r = read_file(breff_path ? breff_path : MII_FX_BREFF_PATH, &eff, &esize)) < 0)
		return r;
	if ((r = read_file(breft_path ? breft_path : MII_FX_BREFT_PATH, &tex, &tsize)) < 0) {
		free(eff);
		return r;
	}
	r = mii_fx_attach(fx, eff, esize, tex, tsize);
	free(eff);
	free(tex);
	return r;
}

/* ---- Small maths ------------------------------------------------------------ */

static uint32_t mix(uint32_t h) {
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

static uint32_t rnd(uint32_t *s) {
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

/* 0 to just under 1. */
static float rnd01(uint32_t *s) {
	return (float) (rnd(s) >> 8) * (1.0f / 16777216);
}

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static float norm3(float v[3]) {
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (l > 1e-12f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
	return l;
}

/* Rotation by r (radians): Z Y X, so X is applied first, as NW4R does. */
static void rot_mtx(float m[3][3], const float r[3]) {
	float cx = cosf(r[0]), sx = sinf(r[0]), cy = cosf(r[1]), sy = sinf(r[1]), cz = cosf(r[2]), sz = sinf(r[2]);

	m[0][0] = cz * cy;
	m[0][1] = cz * sy * sx - sz * cx;
	m[0][2] = cz * sy * cx + sz * sx;
	m[1][0] = sz * cy;
	m[1][1] = sz * sy * sx + cz * cx;
	m[1][2] = sz * sy * cx - cz * sx;
	m[2][0] = -sy;
	m[2][1] = cy * sx;
	m[2][2] = cy * cx;
}

/* out = a times (translate t, rotate r, scale s). */
static void place(float out[3][4], const float a[3][4], const float s[3], const float r[3], const float t[3]) {
	float rm[3][3], l[3][4], o[3][4];
	int i, j;

	rot_mtx(rm, r);
	for (i = 0; i < 3; ++i) {
		for (j = 0; j < 3; ++j)
			l[i][j] = rm[i][j] * s[j];
		l[i][3] = t[i];
	}
	for (i = 0; i < 3; ++i)
		for (j = 0; j < 4; ++j)
			o[i][j] = a[i][0] * l[0][j] + a[i][1] * l[1][j] + a[i][2] * l[2][j] + (j == 3 ? a[i][3] : 0);
	memcpy(out, o, sizeof(o));
}

static void xpoint(float out[3], const float m[3][4], const float v[3]) {
	int i;

	for (i = 0; i < 3; ++i)
		out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3];
}

static void xdir(float out[3], const float m[3][4], const float v[3]) {
	float o[3];
	int i;

	for (i = 0; i < 3; ++i)
		o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
	memcpy(out, o, sizeof(o));
}

/* v turned by angle about the unit axis k. */
static void turn(float v[3], const float k[3], float angle) {
	float c = cosf(angle), s = sinf(angle), d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2], o[3];

	o[0] = v[0] * c + (k[1] * v[2] - k[2] * v[1]) * s + k[0] * d * (1 - c);
	o[1] = v[1] * c + (k[2] * v[0] - k[0] * v[2]) * s + k[1] * d * (1 - c);
	o[2] = v[2] * c + (k[0] * v[1] - k[1] * v[0]) * s + k[2] * d * (1 - c);
	memcpy(v, o, sizeof(o));
}

static void rnd_unit(float v[3], uint32_t *r) {
	float z = 2 * rnd01(r) - 1, a = 2 * (float) M_PI * rnd01(r), s = sqrtf(1 - z * z);

	v[0] = s * cosf(a);
	v[1] = z;
	v[2] = s * sinf(a);
}

/* A direction within `angle` of the unit dir, at random. */
static void cone(float out[3], const float dir[3], float angle, uint32_t *r) {
	float u[3], w[3], c, s, a;
	int i;

	angle = clampf(angle, 0, (float) M_PI);
	if (angle <= 0) {
		memcpy(out, dir, sizeof(float) * 3);
		return;
	}
	c = 1 - rnd01(r) * (1 - cosf(angle));
	s = sqrtf(clampf(1 - c * c, 0, 1));
	a = 2 * (float) M_PI * rnd01(r);
	/* two axes square to dir */
	if (fabsf(dir[0]) < 0.9f) {
		u[0] = 0; u[1] = dir[2]; u[2] = -dir[1];
	} else {
		u[0] = -dir[2]; u[1] = 0; u[2] = dir[0];
	}
	norm3(u);
	w[0] = dir[1] * u[2] - dir[2] * u[1];
	w[1] = dir[2] * u[0] - dir[0] * u[2];
	w[2] = dir[0] * u[1] - dir[1] * u[0];
	for (i = 0; i < 3; ++i)
		out[i] = dir[i] * c + (u[i] * cosf(a) + w[i] * sinf(a)) * s;
}

/* ---- Evaluating a curve ------------------------------------------------------- */

/* The curve's frame at this age of something living `life` frames:
 * stretched over the life (FITTING), looping, held at the end. */
static float curve_frame(const mii_fx_curve *c, int age, int life) {
	float p = (float) c->frames - 1, f = (float) age, k;

	if (p <= 0)
		return 0;
	if (c->flags & C_FIT)
		f = life > 1 ? f * p / (float) (life - 1) : 0;
	if ((c->flags & C_LOOP) || (c->loop > 1 && f < p * c->loop)) {
		k = floorf(f / p);
		f -= k * p;
		if ((c->flags & C_TURN) && fmodf(k, 2) >= 1)
			f = p - f;
	}
	return f < p ? f : p;
}

static void key_value(const mii_fx *fx, const mii_fx_curve *c, int i, uint32_t seed, float out[6]) {
	const mii_fx_key *k = &fx->key[c->key + i];
	const mii_fx_range *r;
	uint32_t h;
	int j;

	if (!k->random) {
		memcpy(out, k->v, sizeof(k->v));
		return;
	}
	/* the same particle draws the same numbers every frame */
	r = &fx->range[c->range + k->ref];
	h = mix(seed + 0x9E3779B9u * (uint32_t) (i + 1));
	for (j = 0; j < 6; ++j) {
		h = mix(h + (uint32_t) j);
		out[j] = r->lo[j] + r->span[j] * (float) (h >> 8) * (1.0f / 16777216);
	}
	if (r->sign && (mix(h) & 1))
		for (j = 0; j < 6; ++j)
			out[j] = -out[j];
}

/* The key at or before frame f. */
static int key_at(const mii_fx *fx, const mii_fx_curve *c, float f) {
	const mii_fx_key *k = fx->key + c->key;
	int i;

	for (i = 0; i + 1 < c->nkeys && k[i + 1].frame <= f; ++i)
		;
	return i;
}

/* The curve's value at frame f, interpolated between its keys. 0 if it has
 * none. */
static int curve_value(const mii_fx *fx, const mii_fx_curve *c, float f, uint32_t seed, float out[6]) {
	const mii_fx_key *k = fx->key + c->key;
	float a[6], b[6], t;
	int i, j;

	if (!c->nkeys)
		return 0;
	i = key_at(fx, c, f);
	key_value(fx, c, i, seed, a);
	if (i + 1 < c->nkeys && f > k[i].frame && k[i + 1].frame > k[i].frame && k[i].interp != 2) {
		t = clampf((f - k[i].frame) / (float) (k[i + 1].frame - k[i].frame), 0, 1);
		if (k[i].interp == 1)
			t = t * t * (3 - 2 * t);
		key_value(fx, c, i + 1, seed, b);
		for (j = 0; j < 6; ++j)
			a[j] += (b[j] - a[j]) * t;
	}
	memcpy(out, a, sizeof(a));
	return 1;
}

/* ---- Playing ---------------------------------------------------------------- */

static const float ident[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

void mii_fx_init(mii_fx_state *s, const mii_fx *fx, uint32_t seed) {
	int i;

	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->fx = fx;
	s->seed = seed;
	for (i = 0; i < MII_FX_INSTANCES; ++i)
		s->inst[i].emitter = -1;
	for (i = 0; i < MII_FX_PARTICLES; ++i)
		s->ptcl[i].emitter = -1;
}

static int start(mii_fx_state *s, int emitter, const float at[3][4]) {
	mii_fx_inst *in = NULL;
	int i, slot = 0;

	for (i = 0; i < MII_FX_INSTANCES; ++i)   /* a free one, else the oldest */
		if (s->inst[i].emitter < 0) {
			slot = i;
			break;
		} else if (s->inst[i].serial < s->inst[slot].serial) {
			slot = i;
		}
	in = &s->inst[slot];
	memset(in, 0, sizeof(*in));
	in->emitter = (int16_t) emitter;
	if (++s->gen > 0x7FFF)
		s->gen = 1;
	in->gen = s->gen;
	in->serial = ++s->serial;
	in->rng = s->fx->emitter[emitter].seed ? s->fx->emitter[emitter].seed : mix(s->seed ^ mix(in->serial));
	if (!in->rng)
		in->rng = 1;
	memcpy(in->at, at ? at : ident, sizeof(in->at));
	return slot;
}

int mii_fx_play(mii_fx_state *s, int emitter, const float at[3][4]) {
	int slot;

	if (!s || !s->fx || emitter < 0 || emitter >= s->fx->nemitters)
		return MII_E_ARG;
	slot = start(s, emitter, at);
	return s->inst[slot].gen << 8 | slot;
}

static mii_fx_inst *handle(const mii_fx_state *s, int h) {
	const mii_fx_inst *in;

	if (!s || h <= 0 || (h & 0xFF) >= MII_FX_INSTANCES)
		return NULL;
	in = &s->inst[h & 0xFF];
	return in->emitter >= 0 && in->gen == h >> 8 ? (mii_fx_inst *) in : NULL;
}

void mii_fx_move(mii_fx_state *s, int h, const float at[3][4]) {
	mii_fx_inst *in = handle(s, h);

	if (in)
		memcpy(in->at, at ? at : ident, sizeof(in->at));
}

void mii_fx_stop(mii_fx_state *s, int h) {
	mii_fx_inst *in = handle(s, h);

	if (in)
		in->emitter = -1;
}

int mii_fx_playing(const mii_fx_state *s, int h) {
	return handle(s, h) != NULL;
}

/* Child emitters whose keys the particle passed this frame: started where
 * it now is, placed as the app placed its parent (its emitter's own
 * rotation not passed on: none of these emitters asks for it). */
static void children(mii_fx_state *s, const mii_fx_ptcl *p, int birth) {
	const mii_fx *fx = s->fx;
	const mii_fx_emitter *e = &fx->emitter[p->emitter];
	float at[3][4], f, prev;
	int i, k, j;

	for (i = 0; i < e->ntracks; ++i) {
		const mii_fx_curve *c = &fx->curve[e->track + i];

		if (c->kind != K_CHILD || (i < e->ninit && !birth))
			continue;
		f = curve_frame(c, p->age, p->life);
		prev = birth || !p->age ? -1 : curve_frame(c, p->age - 1, p->life);
		if (prev == f)
			continue;
		for (k = 0; k < c->nkeys; ++k) {
			const mii_fx_key *key = &fx->key[c->key + k];
			float kf = key->frame;

			if (!(prev < f ? kf > prev && kf <= f : kf > prev || kf <= f) || key->child != 1 ||
					key->ref >= MII_FX_NAMES || c->ref[key->ref] < 0)
				continue;
			for (j = 0; j < 3; ++j) {
				memcpy(at[j], p->at[j], sizeof(p->at[j]));
				at[j][3] = p->pos[j];
			}
			start(s, c->ref[key->ref], (const float (*)[4]) at);
		}
	}
}

/* A particle's curves at its age: colours, size, scale, rotation, texture,
 * children, and (not at birth) the fields and its motion. */
static void run(mii_fx_state *s, mii_fx_ptcl *p, int birth) {
	const mii_fx *fx = s->fx;
	const mii_fx_emitter *e = &fx->emitter[p->emitter];
	float v[6], f, ax[3], d[3];
	int i, k, j;

	for (i = 0; i < e->ntracks; ++i) {
		const mii_fx_curve *c = &fx->curve[e->track + i];
		uint32_t seed = p->seed ^ mix((uint32_t) i + 1);

		if (c->kind == K_OFF || (i < e->ninit && !birth))
			continue;
		f = curve_frame(c, p->age, p->life);
		switch (c->kind) {
		case K_COLOR0: case K_ALPHA0: case K_COLOR0S: case K_ALPHA0S:
			if ((c->frames <= 1 && !birth) || !curve_value(fx, c, f, seed, v))
				break;
			for (j = 0; j < 4; ++j)   /* colour: rgb from bits 0-2; alpha: bit 0 */
				if (c->kind & 1 ? j == 3 && (c->mask & 1) : j < 3 && (c->mask >> j & 1))
					p->color[c->kind >> 1][j] = clampf(v[c->kind & 1 ? 0 : j], 0, 255);
			break;
		case K_SIZE: case K_SCALE:
			if ((c->frames <= 1 && !birth) || !curve_value(fx, c, f, seed, v))
				break;
			for (j = 0; j < 2; ++j)
				if (c->mask >> j & 1) {
					if (c->kind == K_SCALE)
						p->scale[j] = v[j];
					else   /* a size: scaled as the particle's size was at birth */
						p->size[j] = v[j] * (e->size[j] ? p->size[j] / e->size[j] : 1);
				}
			break;
		case K_ROTATE:
			if ((c->frames <= 1 && !birth) || !curve_value(fx, c, f, seed, v))
				break;
			for (j = 0; j < 3; ++j)
				if (c->mask >> j & 1)
					p->rot[j] = v[j];
			break;
		case K_TEXTURE:
			if (c->nkeys) {
				k = fx->key[c->key + key_at(fx, c, f)].ref;
				if (k < MII_FX_NAMES && c->ref[k] >= 0)
					p->tex = c->ref[k];
			}
			break;
		case K_SPEED:
			if (!birth && curve_value(fx, c, f, seed, v))
				for (j = 0; j < 3; ++j)
					p->vel[j] *= v[0];
			break;
		case K_GRAVITY:
			if (birth || !curve_value(fx, c, f, seed, v))
				break;
			memcpy(ax, c->dir, sizeof(ax));
			if (c->coord == 1)
				xdir(ax, p->m, ax);
			norm3(ax);
			for (j = 0; j < 3; ++j)
				(c->target == 1 ? p->pos : p->vel)[j] += ax[j] * v[0];
			break;
		case K_SPIN: {   /* about the axis rot turns Y to, through the emitter */
			float rm[3][3];

			if (birth || !curve_value(fx, c, f, seed, v))
				break;
			rot_mtx(rm, v + 1);
			for (j = 0; j < 3; ++j)
				ax[j] = rm[j][1];
			if (c->coord == 1)
				xdir(ax, p->m, ax);
			if (norm3(ax) <= 1e-12f)
				break;
			if (c->target == 1) {
				for (j = 0; j < 3; ++j)
					d[j] = p->pos[j] - p->m[j][3];
				turn(d, ax, v[0]);
				for (j = 0; j < 3; ++j)
					p->pos[j] = p->m[j][3] + d[j];
			} else {
				turn(p->vel, ax, v[0]);
			}
			break;
		}
		case K_VORTEX: {   /* roughly: round the axis, inner speed near it, outer from `distance` out */
			float r, sp, t[3];

			if (birth || !curve_value(fx, c, f, seed, v))
				break;
			memcpy(ax, v + 3, sizeof(ax));
			if (norm3(ax) <= 1e-12f) {
				ax[0] = 0; ax[1] = 1; ax[2] = 0;
			}
			if (c->coord == 1)
				xdir(ax, p->m, ax);
			norm3(ax);
			for (j = 0; j < 3; ++j)
				d[j] = p->pos[j] - p->m[j][3];
			t[0] = ax[1] * d[2] - ax[2] * d[1];
			t[1] = ax[2] * d[0] - ax[0] * d[2];
			t[2] = ax[0] * d[1] - ax[1] * d[0];
			r = norm3(t);
			if (r <= 1e-6f)
				break;
			sp = v[2] > 0 && r < v[2] ? v[0] + (v[1] - v[0]) * r / v[2] : v[1];
			for (j = 0; j < 3; ++j)
				(c->target == 1 ? p->pos : p->vel)[j] += t[j] * sp;
			break;
		}
		}
	}
	if (!birth)
		for (j = 0; j < 3; ++j)
			p->pos[j] += p->vel[j];
	children(s, p, birth);
}

/* Where emission i of n starts on the emitter's form, and the form's normal
 * there, in the emitter's space. */
static void form(const mii_fx_emitter *e, uint32_t *r, int i, int n, int even, float pos[3], float nrm[3]) {
	int kind = e->emit_flag & 0xFF;
	float rx = e->param[0], rz, inner, a0 = e->param[2], span = e->param[3] - e->param[2], a, t;

	pos[0] = pos[1] = pos[2] = 0;
	nrm[0] = 0; nrm[1] = 1; nrm[2] = 0;
	if (kind != 0 && kind != 7)
		return;   /* a point (9), or a form these files do not use: its centre */
	rz = e->emit_flag & (1u << 25) ? rx : kind == 7 ? e->param[5] : e->param[4] ? e->param[4] : rx;
	inner = clampf(e->param[1] / 100, 0, 1);
	if (even)   /* evenly round; a random start unless FIXED_START_ANGLE */
		a = a0 + span * ((float) i + (e->emit_flag & (1u << 18) ? 0 : rnd01(r))) / (float) (n > 0 ? n : 1);
	else
		a = a0 + span * rnd01(r);
	t = rnd01(r);
	t = e->emit_flag & (1u << 24) ? sqrtf(inner * inner + (1 - inner * inner) * t) : inner + (1 - inner) * t;
	pos[0] = rx * t * cosf(a);
	pos[2] = rz * t * sinf(a);
	if (kind == 7) {   /* a cylinder: height, the normal outward */
		pos[1] = (rnd01(r) - 0.5f) * e->param[4];
		nrm[0] = cosf(a); nrm[1] = 0; nrm[2] = sinf(a);
	}
}

static mii_fx_ptcl *new_particle(mii_fx_state *s) {
	int i, slot = 0;

	for (i = 0; i < MII_FX_PARTICLES; ++i)   /* a free one, else the oldest */
		if (s->ptcl[i].emitter < 0) {
			slot = i;
			break;
		} else if (s->ptcl[i].serial < s->ptcl[slot].serial) {
			slot = i;
		}
	return &s->ptcl[slot];
}

static void spawn(mii_fx_state *s, mii_fx_inst *in, const float m[3][4], int i, int n, int even) {
	const mii_fx_emitter *e = &s->fx->emitter[in->emitter];
	mii_fx_ptcl *p = new_particle(s);
	float pos[3], nrm[3], vel[3] = { 0, 0, 0 }, d[3], unit, f;
	int j, life;

	memset(p, 0, sizeof(*p));
	p->emitter = in->emitter;
	p->serial = ++s->serial;
	p->seed = rnd(&in->rng);
	form(e, &in->rng, i, n, even, pos, nrm);
	/* speed: outward, from the Y axis, at random, along the normal, along spec_dir */
	memcpy(d, pos, sizeof(d));
	if (norm3(d) <= 1e-12f)
		rnd_unit(d, &in->rng);
	for (j = 0; j < 3; ++j)
		vel[j] += d[j] * e->v_radial;
	d[0] = pos[0]; d[1] = 0; d[2] = pos[2];
	if (norm3(d) > 1e-12f)
		for (j = 0; j < 3; ++j)
			vel[j] += d[j] * e->v_yaxis;
	if (e->v_random) {
		rnd_unit(d, &in->rng);
		for (j = 0; j < 3; ++j)
			vel[j] += d[j] * e->v_random;
	}
	if (e->v_normal) {
		cone(d, nrm, e->v_normal_cone, &in->rng);
		for (j = 0; j < 3; ++j)
			vel[j] += d[j] * e->v_normal;
	}
	if (e->v_spec) {
		float rm[3][3], y[3];

		rot_mtx(rm, e->spec_dir);
		y[0] = rm[0][1]; y[1] = rm[1][1]; y[2] = rm[2][1];
		cone(d, y, e->v_spec_cone, &in->rng);
		for (j = 0; j < 3; ++j)
			vel[j] += d[j] * e->v_spec;
	}
	f = 1 - clampf(e->vel_rnd, 0, 100) / 100 * (2 * rnd01(&in->rng) - 1);
	for (j = 0; j < 3; ++j)
		vel[j] *= f;
	xpoint(p->pos, m, pos);
	xdir(p->vel, m, vel);
	memcpy(p->m, m, sizeof(p->m));
	for (j = 0; j < 3; ++j)
		memcpy(p->at[j], in->at[j], sizeof(p->at[j]));
	/* the app's scale scales the particle: its columns' mean length */
	unit = 0;
	for (j = 0; j < 3; ++j)
		unit += sqrtf(in->at[0][j] * in->at[0][j] + in->at[1][j] * in->at[1][j] + in->at[2][j] * in->at[2][j]) / 3;
	life = e->ptcl_life - (int) (e->ptcl_life * rnd01(&in->rng) * clampf(e->life_rnd, 0, 100) / 100);
	p->life = (uint16_t) (life < 1 ? 1 : life);
	f = 1 + clampf(e->size_rnd, 0, 100) / 100 * (2 * rnd01(&in->rng) - 1);
	for (j = 0; j < 2; ++j) {
		p->size[j] = e->size[j] * f * unit;
		p->scale[j] = e->pscale[j];
	}
	memcpy(p->rot, e->prot, sizeof(p->rot));
	for (j = 0; j < 4; ++j) {
		p->color[0][j] = e->color[0][j];
		p->color[1][j] = e->color[1][j];
	}
	p->tex = (int16_t) e->tex;
	run(s, p, 1);
}

/* One frame of an emitter: where it is, and what it emits. */
static void emit(mii_fx_state *s, mii_fx_inst *in) {
	const mii_fx_emitter *e = &s->fx->emitter[in->emitter];
	float m[3][4], sc[3], ro[3], tr[3], v[6], count = e->emit;
	uint32_t t = in->tick++;
	int forever = (e->common & 4) || e->emit_life == 0xFFFF, life = forever ? 0xFFFF : e->emit_life, i, j, n;
	int even = !!(e->emit_flag & (1u << 17)), at_tick;

	memcpy(sc, e->scale, sizeof(sc));
	memcpy(ro, e->rot, sizeof(ro));
	memcpy(tr, e->trans, sizeof(tr));
	for (i = 0; i < e->nemit_tracks; ++i) {   /* the emitter's own curves, by its age */
		const mii_fx_curve *c = &s->fx->curve[e->track + e->ntracks + i];
		float *to = c->kind == K_EMIT_SCALE ? sc : c->kind == K_EMIT_ROTATE ? ro : c->kind == K_EMIT_TRANSLATE ? tr : NULL;

		if (c->kind == K_OFF || !curve_value(s->fx, c, curve_frame(c, (int) (t > 0xFFFF ? 0xFFFF : t), life),
												   in->rng, v))
			continue;
		if (c->kind == K_EMIT_EMISSION)
			count = v[0];
		for (j = 0; to && j < 3; ++j)
			if (c->mask >> j & 1)
				to[j] = v[j];
	}
	place(m, (const float (*)[4]) in->at, sc, ro, tr);
	at_tick = t >= e->start && (forever || t - e->start < e->emit_life) &&
		(t - e->start) % ((uint32_t) e->interval + 1) == 0;
	if (at_tick) {
		if (even) {
			n = e->div;
		} else {   /* fractions owed carry over; the first emission is at least one */
			count *= 1 + clampf(e->emit_rnd, 0, 100) / 100 * (2 * rnd01(&in->rng) - 1);
			in->accum += clampf(count, 0, MII_FX_PARTICLES);
			n = (int) in->accum;
			in->accum -= (float) n;
			if (!in->emitted && !n && count > 0) {
				n = 1;
				in->accum = 0;
			}
		}
		if (n > MII_FX_PARTICLES)
			n = MII_FX_PARTICLES;
		for (i = 0; i < n; ++i)
			spawn(s, in, (const float (*)[4]) m, i, n, even);
		if (n)
			in->emitted = 1;
	}
	if (!forever && t + 1 >= (uint32_t) e->start + e->emit_life)
		in->emitter = -1;   /* done: its particles live on */
}

static void step(mii_fx_state *s) {
	int i, more, rounds;

	++s->frame;
	for (i = 0; i < MII_FX_PARTICLES; ++i) {
		mii_fx_ptcl *p = &s->ptcl[i];

		if (p->emitter < 0)
			continue;
		if (++p->age >= p->life) {
			p->emitter = -1;
			continue;
		}
		run(s, p, 0);
	}
	/* every emitter once, those started this frame (children) included */
	for (rounds = 0, more = 1; more && rounds < 4 * MII_FX_INSTANCES; ) {
		more = 0;
		for (i = 0; i < MII_FX_INSTANCES && rounds < 4 * MII_FX_INSTANCES; ++i) {
			mii_fx_inst *in = &s->inst[i];

			if (in->emitter < 0 || in->stepped == s->frame)
				continue;
			in->stepped = s->frame;
			emit(s, in);
			more = 1;
			++rounds;
		}
	}
}

void mii_fx_step(mii_fx_state *s, float frames) {
	int n;

	if (!s || !s->fx || !(frames > 0))
		return;
	s->frac += frames < 8 ? frames : 8;
	for (n = 0; s->frac >= 1 && n < 8; ++n) {
		s->frac -= 1;
		step(s);
	}
}

/* The live particles' slots, oldest first. */
static int order(const mii_fx_state *s, int idx[MII_FX_PARTICLES]) {
	int i, j, n = 0;

	for (i = 0; i < MII_FX_PARTICLES; ++i) {
		if (s->ptcl[i].emitter < 0)
			continue;
		for (j = n++; j > 0 && s->ptcl[idx[j - 1]].serial > s->ptcl[i].serial; --j)
			idx[j] = idx[j - 1];
		idx[j] = i;
	}
	return n;
}

int mii_fx_particles(const mii_fx_state *s, mii_fx_particle *out, int max) {
	int idx[MII_FX_PARTICLES], n, i, j;

	if (!s || !s->fx)
		return 0;
	n = order(s, idx);
	for (i = 0; out && i < n && i < max; ++i) {
		const mii_fx_ptcl *p = &s->ptcl[idx[i]];
		const mii_fx_emitter *e = &s->fx->emitter[p->emitter];
		mii_fx_particle *o = &out[i];

		memcpy(o->pos, p->pos, sizeof(o->pos));
		o->size[0] = p->size[0] * p->scale[0];
		o->size[1] = p->size[1] * p->scale[1];
		o->rot = p->rot[2];
		for (j = 0; j < 4; ++j)
			o->color[j] = (uint8_t) (p->color[0][j] + 0.5f);
		for (j = 0; j < 3; ++j)
			o->color2[j] = (uint8_t) (p->color[1][j] + 0.5f);
		o->tex = p->tex;
		o->emitter = p->emitter;
		o->age = p->age;
		o->life = p->life;
		o->hidden = (e->draw & 0x400) || (e->common & 2);
	}
	return n;
}

/* ---- Drawing ----------------------------------------------------------------- */

#ifndef MII_NO_OGC

void mii_fx_draw(const mii_fx_state *s, Mtx view) {
	int idx[MII_FX_PARTICLES], n, i, j, last_e = -1, last_t = -1;
	Mtx id;
	GXTexObj obj;
	float u = 1, v = 1;

	if (!s || !s->fx || !(n = order(s, idx)))
		return;
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	guMtxIdentity(id);
	GX_LoadPosMtxImm(id, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	/* rgb = lerp(secondary, primary, the texture's intensity); alpha = primary's times the texture's */
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_KONST, GX_CC_C0, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_A0, GX_CA_TEXA, GX_CA_ZERO);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K1);
	GX_SetCullMode(GX_CULL_NONE);
	for (i = 0; i < n; ++i) {
		const mii_fx_ptcl *p = &s->ptcl[idx[i]];
		const mii_fx_emitter *e = &s->fx->emitter[p->emitter];
		const mii_fx_tex *t;
		GXColor pri, sec;
		float c[3], w, h, a, ca, sa, x[4], y[4];

		if ((e->draw & 0x400) || (e->common & 2) || p->tex < 0 || p->tex >= s->fx->ntex)
			continue;
		t = &s->fx->tex[p->tex];
		if (!t->gx)
			continue;
		if (p->emitter != last_e) {
			GX_SetBlendMode(e->blend[0], e->blend[1], e->blend[2], e->blend[3]);
			GX_SetAlphaCompare(e->cmp[0], e->ref[0], e->cmp[2], e->cmp[1], e->ref[1]);
			GX_SetZMode(e->draw & 1 ? GX_TRUE : GX_FALSE, e->zfunc, e->draw & 2 ? GX_TRUE : GX_FALSE);
			GX_SetZCompLoc(e->draw & 4 ? GX_TRUE : GX_FALSE);
			last_t = -1;
		}
		if (p->emitter != last_e || p->tex != last_t) {   /* mirror wrap: the quad runs over the image twice */
			int ws = e->wrap & 3, wt = e->wrap >> 2 & 3;

			GX_InitTexObj(&obj, t->gx, t->w, t->h, t->fmt, ws > 2 ? 0 : ws, wt > 2 ? 0 : wt, GX_FALSE);
			GX_InitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
			GX_LoadTexObj(&obj, GX_TEXMAP0);
			u = ws == 2 ? 2 : 1;
			v = wt == 2 ? 2 : 1;
		}
		last_e = p->emitter;
		last_t = p->tex;
		pri = (GXColor) { (u8) p->color[0][0], (u8) p->color[0][1], (u8) p->color[0][2], (u8) p->color[0][3] };
		sec = (GXColor) { (u8) p->color[1][0], (u8) p->color[1][1], (u8) p->color[1][2], 255 };
		GX_SetTevColor(GX_TEVREG0, pri);
		GX_SetTevKColor(GX_KCOLOR1, sec);
		/* the corners in view space, turned about the line of sight */
		for (j = 0; j < 3; ++j)
			c[j] = view[j][0] * p->pos[0] + view[j][1] * p->pos[1] + view[j][2] * p->pos[2] + view[j][3];
		w = p->size[0] * p->scale[0] / 2;
		h = p->size[1] * p->scale[1] / 2;
		a = p->rot[2];
		if (e->bb == 2) {   /* its Y along the motion, as seen */
			float mx = view[0][0] * p->vel[0] + view[0][1] * p->vel[1] + view[0][2] * p->vel[2];
			float my = view[1][0] * p->vel[0] + view[1][1] * p->vel[1] + view[1][2] * p->vel[2];

			if (mx * mx + my * my > 1e-12f)
				a += atan2f(my, mx) - (float) M_PI / 2;
		}
		ca = cosf(a);
		sa = sinf(a);
		x[0] = -w; y[0] = h;
		x[1] = w; y[1] = h;
		x[2] = w; y[2] = -h;
		x[3] = -w; y[3] = -h;
		GX_Begin(GX_QUADS, GX_VTXFMT2, 4);
		for (j = 0; j < 4; ++j) {
			GX_Position3f32(c[0] + x[j] * ca - y[j] * sa, c[1] + x[j] * sa + y[j] * ca, c[2]);
			GX_TexCoord2f32(j == 1 || j == 2 ? u : 0, j >= 2 ? v : 0);
		}
		GX_End();
	}
}

#endif
