/*
 * miibody.c - the Wii's own Mii bodies (miidraw.h). Public domain.
 *
 * Reads the Mii Channel's maleBody.brres and femaleBody.brres (NW4R BRRES:
 * one MDL0 model, CHR0 animations), poses them and skins them on the CPU.
 * The layouts are the NW4R formats as BrawlBox and others document them,
 * checked against the Mii Channel's own files; the per-bone scale for
 * height and build follows Nintendo's later Mii libraries
 * (docs/research/mii-bodies.md). The files are the user's own copies.
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

static float rdf(buf *b, uint32_t at) {
	uint32_t u = rd32(b, at);
	float f;

	memcpy(&f, &u, 4);
	return f;
}

/* A NUL-terminated name inside the file, or "" (and bad) if it runs out. */
static const char *rdname(buf *b, uint32_t at) {
	uint32_t i;

	if (at >= b->size) {
		b->bad = 1;
		return "";
	}
	for (i = at; i < b->size; ++i)
		if (!b->d[i])
			return (const char *) b->d + at;
	b->bad = 1;
	return "";
}

/* A BRRES index group: entry i's name and data offset (absolute). */
static int group_count(buf *b, uint32_t at) {
	uint32_t n = rd32(b, at + 4);

	return n > 4096 || !in(b, at, 8 + 16 * (n + 1)) ? -1 : (int) n;
}

static void group_entry(buf *b, uint32_t at, int i, const char **name, uint32_t *data) {
	uint32_t e = at + 8 + 16 * (i + 1);

	*name = rdname(b, at + rd32(b, e + 8));
	*data = at + rd32(b, e + 12);
}

/* ---- Small maths ------------------------------------------------------------ */

typedef float m33[3][3];

static void rot_xyz(m33 out, const float deg[3]) {
	float x = deg[0] * (float) M_PI / 180, y = deg[1] * (float) M_PI / 180, z = deg[2] * (float) M_PI / 180;
	float cx = cosf(x), sx = sinf(x), cy = cosf(y), sy = sinf(y), cz = cosf(z), sz = sinf(z);

	/* Rz Ry Rx: X applied first, as NW4R's bones. */
	out[0][0] = cz * cy;
	out[0][1] = cz * sy * sx - sz * cx;
	out[0][2] = cz * sy * cx + sz * sx;
	out[1][0] = sz * cy;
	out[1][1] = sz * sy * sx + cz * cx;
	out[1][2] = sz * sy * cx - cz * sx;
	out[2][0] = -sy;
	out[2][1] = cy * sx;
	out[2][2] = cy * cx;
}

static void mul33(m33 out, m33 a, m33 b) {
	m33 t;
	int i, j;

	for (i = 0; i < 3; ++i)
		for (j = 0; j < 3; ++j)
			t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
	memcpy(out, t, sizeof(t));
}

/* ---- MDL0 -------------------------------------------------------------------- */

/* Section order by MDL0 version: where the bones, vertices, normals and
 * objects are. */
static int sections(uint32_t ver, int *bones, int *verts, int *norms, int *objects, int *count) {
	if (ver == 8 || ver == 9) {
		*bones = 1; *verts = 2; *norms = 3; *objects = 8; *count = 11;
	} else if (ver == 10 || ver == 11) {
		*bones = 1; *verts = 2; *norms = 3; *objects = 10; *count = 14;
	} else {
		return 0;
	}
	return 1;
}

/* A coordinate, or a key's frame, value or slope: finite, and within any body's reach (the
 * Mii Channel's are within a few hundred). */
static int sane(float v) {
	return v == v && v > -1e5f && v < 1e5f;
}

/* A copy the GPU reads: 32-byte aligned, padded with zeros (GX_NOPs) to
 * 32 bytes, out of the cache on a Wii. */
static uint8_t *gpu_copy(const uint8_t *src, uint32_t n, uint32_t *padded) {
	uint32_t size = (n + 31) & ~31u;
	uint8_t *p;

#ifndef MII_NO_OGC
	p = memalign(32, size ? size : 32);
#else
	p = malloc(size ? size : 32);
#endif
	if (!p)
		return NULL;
	memset(p, 0, size ? size : 32);
	memcpy(p, src, n);
#ifndef MII_NO_OGC
	DCFlushRange(p, size ? size : 32);
#endif
	if (padded)
		*padded = size;
	return p;
}

/* A vertex or normal array as floats: count tuples of 3, and (raw) a copy of
 * it as the GPU reads it. */
static float *array3(buf *b, uint32_t at, int normals, int *count, mii_body_array *raw) {
	uint32_t data = at + rd32(b, at + 8), comp = rd32(b, at + 0x14), fmt = rd32(b, at + 0x18);
	unsigned div = rd8(b, at + 0x1C), stride = rd8(b, at + 0x1D), n = rd16(b, at + 0x1E), i, k;
	static const unsigned size[] = { 1, 1, 2, 2, 4 };
	float *out;

	*count = 0;
	if ((normals ? comp != 0 : comp != 1) || fmt > 4 || div > 15 || b->bad ||
			!in(b, data, stride * n) || stride < 3 * size[fmt])
		return NULL;
	out = malloc(sizeof(float) * 3 * (n ? n : 1));
	if (!out)
		return NULL;
	raw->gx = gpu_copy(b->d + data, stride * n, NULL);
	raw->stride = stride;
	raw->fmt = (uint8_t) fmt;
	raw->frac = (uint8_t) div;
	if (!raw->gx) {
		free(out);
		return NULL;
	}
	for (i = 0; i < n; ++i)
		for (k = 0; k < 3; ++k) {
			uint32_t p = data + i * stride + k * size[fmt];
			float v;

			switch (fmt) {
			case 0: v = rd8(b, p); break;
			case 1: v = (int8_t) rd8(b, p); break;
			case 2: v = rd16(b, p); break;
			case 3: v = (int16_t) rd16(b, p); break;
			default: v = rdf(b, p);
			}
			out[3 * i + k] = fmt == 4 ? v : v / (float) (1u << div);
			if (!sane(out[3 * i + k])) {
				free(out);
				return NULL;
			}
		}
	*count = n;
	return out;
}

static int bone_named(const mii_body *body, const char *name) {
	int i;

	for (i = 0; i < body->nbones; ++i)
		if (!strcmp(body->bone[i].name, name))
			return i;
	return -1;
}

/* Bones: names, parents (by entry address), matrix ids, local transforms. */
static int read_bones(buf *b, uint32_t grp, mii_body *body) {
	uint32_t at[MII_BODY_BONES], seen = 0;
	int n = group_count(b, grp), i, j;

	if (n <= 0 || n > MII_BODY_BONES)
		return MII_E_FORMAT;
	for (i = 0; i < n; ++i) {
		const char *name;
		uint32_t e;
		mii_bone *bn;

		group_entry(b, grp, i, &name, &e);
		if (rd32(b, e + 0x0C) >= (uint32_t) n || seen >> rd32(b, e + 0x0C) & 1)
			return MII_E_FORMAT;   /* each index once: at[] filled for all */
		seen |= 1u << rd32(b, e + 0x0C);
		bn = &body->bone[rd32(b, e + 0x0C)];   /* by the bone's own index */
		at[rd32(b, e + 0x0C)] = e;
		snprintf(bn->name, sizeof(bn->name), "%s", name);
		bn->matrix = (int) rd32(b, e + 0x10);
		for (j = 0; j < 3; ++j) {
			bn->s[j] = rdf(b, e + 0x20 + 4 * j);
			bn->r[j] = rdf(b, e + 0x2C + 4 * j);
			bn->t[j] = rdf(b, e + 0x38 + 4 * j);
			if (!sane(bn->s[j]) || !sane(bn->r[j]) || !sane(bn->t[j]))
				return MII_E_FORMAT;
		}
		bn->parent = (int) rd32(b, e + 0x5C);   /* an offset from e for now */
	}
	if (b->bad)
		return MII_E_FORMAT;
	body->nbones = n;
	for (i = 0; i < n; ++i) {
		int32_t off = (int32_t) body->bone[i].parent;

		body->bone[i].parent = -1;
		if (!off)
			continue;
		for (j = 0; j < n; ++j)
			if (at[j] == at[i] + (uint32_t) off)
				body->bone[i].parent = j;
		/* Parents first: the pose walks the bones in order. */
		if (body->bone[i].parent < 0 || body->bone[i].parent >= i)
			return MII_E_FORMAT;
	}
	for (i = 0; i < n; ++i) {
		m33 r;

		rot_xyz(r, body->bone[i].r);
		if (body->bone[i].parent >= 0)
			mul33(body->bone[i].bind_rot, body->bone[body->bone[i].parent].bind_rot, r);
		else
			memcpy(body->bone[i].bind_rot, r, sizeof(r));
	}
	body->head = bone_named(body, "head");
	body->chest = bone_named(body, "chest");
	return body->head < 0 ? MII_E_FORMAT : MII_OK;
}

/* Two skin vertices at one spot: on one bone, at one position. Each corner
 * of a list is a skin vertex of its own, so a strip's turning triangles
 * (two corners at one spot) show only so. */
static int same_spot(const mii_body *body, int a, int b) {
	return body->vbone[a] == body->vbone[b] && !memcmp(body->vpos + 3 * a, body->vpos + 3 * b, 3 * sizeof(float));
}

/* One object's display list into skin vertices and triangles. Supports
 * what the Mii Channel's bodies use: a position-matrix index per vertex,
 * texture-matrix indexes (ignored), indexed positions and normals; the
 * XF loads that put bone matrices in slots. */
static int read_object(buf *b, uint32_t o, mii_body *body, const float *pos, int npos, const float *nrm,
					   int nnrm, int mesh, int pv, int pn) {
	uint32_t lo = rd32(b, o + 0x0C), hi = rd32(b, o + 0x10);
	uint32_t prim = o + 0x24 + rd32(b, o + 0x2C), size = rd32(b, o + 0x28), at, end;
	int slot_bone[16], i, pos_w, nrm_w, tex_mtx = 0, verts = 0;
	mii_body_obj *ob;
	uint16_t *tri;
	int ntri = 0, cap;

	for (i = 0; i < 16; ++i)
		slot_bone[i] = -1;
	for (i = 1; i <= 8; ++i)
		tex_mtx += lo >> i & 1;
	pos_w = (lo >> 9 & 3) == 2 ? 1 : (lo >> 9 & 3) == 3 ? 2 : 0;
	nrm_w = (lo >> 11 & 3) == 2 ? 1 : (lo >> 11 & 3) == 3 ? 2 : 0;
	if (!(lo & 1) || !pos_w || !nrm_w || (lo >> 13 & 15) || hi || !in(b, prim, size))
		return MII_E_FORMAT;
	cap = (int) size / 3 + 1;   /* triangles: a vertex takes at least 3 bytes */
	tri = malloc(sizeof(uint16_t) * 3 * (cap + body->ntris[mesh]));
	if (!tri)
		return MII_E_MEM;
	if (body->ntris[mesh]) {
		memcpy(tri, body->tris[mesh], sizeof(uint16_t) * 3 * body->ntris[mesh]);
		free(body->tris[mesh]);
		ntri = body->ntris[mesh];
	}
	body->tris[mesh] = tri;
	/* The display list as the GPU draws it (patched below, then flushed). */
	if (body->nobjs >= MII_BODY_OBJS)
		return MII_E_FORMAT;
	ob = &body->obj[body->nobjs];
	ob->dl = gpu_copy(b->d + prim, size, &ob->dl_size);
	if (!ob->dl)
		return MII_E_MEM;
	++body->nobjs;
	ob->vcd = lo;
	ob->mesh = mesh;
	ob->pos = pv;
	ob->nrm = pn;
	at = prim;
	end = prim + size;
	while (at < end) {
		uint8_t op = rd8(b, at);

		if (op == 0) {
			++at;
			continue;
		}
		if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {   /* indexed XF loads */
			int mid = rd16(b, at + 1);

			if (end - at < 5)   /* within the object: its list is copied alone */
				return MII_E_FORMAT;
			/* The GPU reads these from the palette by id: in range, and
			 * no light matrices (no light palette is set). */
			if (op == 0x38 || mid >= MII_BODY_MATRICES)
				return MII_E_FORMAT;
			if (mid >= body->nmatrices)
				body->nmatrices = mid + 1;
			if (op == 0x20) {   /* a position matrix: matrix id -> slot */
				int id = rd16(b, at + 1), slot = (rd16(b, at + 3) & 0xFFF) / 12, k;

				if (slot < 16) {
					slot_bone[slot] = -1;
					for (k = 0; k < body->nbones; ++k)
						if (body->bone[k].matrix == id)
							slot_bone[slot] = k;
				}
			}
			at += 5;
			continue;
		}
		if ((op & 0xF8) == 0x90 || (op & 0xF8) == 0x98 || (op & 0xF8) == 0xA0) {
			int kind = op & 0xF8, n = rd16(b, at + 1), first = body->nverts, v;

			if (end - at < 3 || (uint32_t) n * (1 + tex_mtx + pos_w + nrm_w) > end - at - 3)
				return MII_E_FORMAT;   /* its vertices within the object */
			ob->dl[at - prim] = (uint8_t) (kind | 1);   /* GX_VTXFMT1: the head keeps VTXFMT0 */
			at += 3;
			for (v = 0; v < n; ++v) {
				int mtx = rd8(b, at), p, q, k, bone;

				at += 1 + tex_mtx;
				p = pos_w == 1 ? rd8(b, at) : rd16(b, at);
				at += pos_w;
				q = nrm_w == 1 ? rd8(b, at) : rd16(b, at);
				at += nrm_w;
				bone = mtx / 3 < 16 ? slot_bone[mtx / 3] : -1;
				if (b->bad || p >= npos || q >= nnrm || bone < 0 || body->nverts >= body->vcap)
					return MII_E_FORMAT;
				k = body->nverts++;
				body->vbone[k] = (uint8_t) bone;
				memcpy(body->vpos + 3 * k, pos + 3 * p, 3 * sizeof(float));
				memcpy(body->vnrm + 3 * k, nrm + 3 * q, 3 * sizeof(float));
			}
			for (v = 0; v + 2 < n; ++v) {
				int a, c, d;

				if (kind == 0x90) {
					if (v % 3)
						continue;
					a = v; c = v + 1; d = v + 2;
				} else if (kind == 0x98) {
					a = v % 2 ? v + 1 : v;
					c = v % 2 ? v : v + 1;
					d = v + 2;
				} else {
					a = 0; c = v + 1; d = v + 2;
				}
				if (ntri >= cap + body->ntris[mesh])
					return MII_E_FORMAT;
				tri[3 * ntri] = (uint16_t) (first + a);
				tri[3 * ntri + 1] = (uint16_t) (first + c);
				tri[3 * ntri + 2] = (uint16_t) (first + d);
				++ntri;
			}
			verts += n;
			continue;
		}
		return MII_E_FORMAT;   /* something these bodies do not use */
	}
	body->ntris[mesh] = ntri;
#ifndef MII_NO_OGC
	DCFlushRange(ob->dl, ob->dl_size);
#endif
	return b->bad || !verts ? MII_E_FORMAT : MII_OK;
}

/* The materials' look, as names to find in the file's textures: the
 * see-through one's (the shadow) and its colour (TEV register 0, in its
 * display list), and an opaque one's (env0, the light the shirt and
 * trousers reflect). Any of it missing: drawn as before. */
static void read_materials(buf *b, uint32_t grp, mii_body *body, const char **env, const char **shadow) {
	int n = group_count(b, grp), i;

	for (i = 0; i < n && i < 16; ++i) {
		const char *name;
		uint32_t e, ref, dl, k;
		int xlu;

		group_entry(b, grp, i, &name, &e);
		if (!rd32(b, e + 0x2C))
			continue;   /* no texture */
		xlu = rd32(b, e + 0x10) >> 31;
		ref = e + rd32(b, e + 0x30);
		name = rdname(b, ref + rd32(b, ref));
		if (b->bad)
			return;
		if (!xlu) {
			if (!*env)
				*env = name;
			continue;
		}
		if (*shadow)
			continue;
		*shadow = name;
		body->shadow_rgb[0] = body->shadow_rgb[1] = body->shadow_rgb[2] = 32;
		dl = e + rd32(b, e + 0x38);
		for (k = 0; k + 5 <= 0x180 && in(b, dl + k, 5); ++k) {   /* BP writes: 61 rr vvvvvv */
			uint32_t v = (uint32_t) rd8(b, dl + k + 2) << 16 | (uint32_t) rd8(b, dl + k + 3) << 8 |
						 rd8(b, dl + k + 4);

			if (rd8(b, dl + k) != 0x61 || v >> 23 & 1)
				continue;   /* not a BP write, or a constant (konst) colour */
			if (rd8(b, dl + k + 1) == 0xE2) {
				body->shadow_rgb[0] = (uint8_t) ((v & 0x7FF) > 255 ? 255 : v & 0x7FF);
				k += 4;
			} else if (rd8(b, dl + k + 1) == 0xE3) {
				body->shadow_rgb[2] = (uint8_t) ((v & 0x7FF) > 255 ? 255 : v & 0x7FF);
				body->shadow_rgb[1] = (uint8_t) ((v >> 12 & 0x7FF) > 255 ? 255 : v >> 12 & 0x7FF);
				k += 4;
			}
		}
	}
}

static int read_model(buf *b, uint32_t m, mii_body *body, const char **env, const char **shadow) {
	uint32_t ver = rd32(b, m + 8), sec[14];
	int bi, vi, ni, oi, count, i, r, nv, no;
	uint32_t vg, ng, og;
	float *pos[8] = { 0 }, *nrm[8] = { 0 };
	int npos[8] = { 0 }, nnrm[8] = { 0 };
	const char *vname[8] = { 0 };

	if (!sections(ver, &bi, &vi, &ni, &oi, &count))
		return MII_E_FORMAT;
	for (i = 0; i < count; ++i)
		sec[i] = rd32(b, m + 0x10 + 4 * i);
	if (b->bad || !sec[bi] || !sec[vi] || !sec[ni] || !sec[oi])
		return MII_E_FORMAT;
	if ((r = read_bones(b, m + sec[bi], body)) < 0)
		return r;
	if (sec[count == 11 ? 6 : 8]) {   /* the look is extra: a fault in it fails nothing */
		read_materials(b, m + sec[count == 11 ? 6 : 8], body, env, shadow);
		b->bad = 0;
	}
	memset(body->matrix_bone, -1, sizeof(body->matrix_bone));
	for (i = 0; i < body->nbones; ++i)
		if (body->bone[i].matrix >= 0 && body->bone[i].matrix < MII_BODY_MATRICES)
			body->matrix_bone[body->bone[i].matrix] = (int8_t) i;
	vg = m + sec[vi];
	ng = m + sec[ni];
	og = m + sec[oi];
	nv = group_count(b, vg);
	no = group_count(b, ng);
	if (nv <= 0 || nv > 8 || no <= 0 || no > 8 || group_count(b, og) <= 0)
		return MII_E_FORMAT;
	r = MII_OK;
	for (i = 0; i < nv && r == MII_OK; ++i) {
		uint32_t e;

		group_entry(b, vg, i, &vname[i], &e);
		pos[i] = array3(b, e, 0, &npos[i], &body->pos_array[i]);
		if (!pos[i] && npos[i] == 0 && rd32(b, e + 0x14) == 1)
			r = MII_E_FORMAT;
	}
	for (i = 0; i < no && r == MII_OK; ++i) {
		const char *n;
		uint32_t e;

		group_entry(b, ng, i, &n, &e);
		nrm[i] = array3(b, e, 1, &nnrm[i], &body->nrm_array[i]);
		if (!nrm[i])
			r = MII_E_FORMAT;
	}
	if (r == MII_OK) {
		int n = group_count(b, og);
		uint32_t cap = 0;

		for (i = 0; i < n && cap <= 0x10000; ++i) {   /* stops before it can wrap */
			const char *name;
			uint32_t o, verts;

			group_entry(b, og, i, &name, &o);
			verts = rd32(b, o + 0x28);
			cap += verts > 0x30000 ? 0x10001 : verts / 3 + 1;
		}
		if (b->bad || cap > 0x10000)
			r = MII_E_FORMAT;
		body->vcap = (int) cap;
		if (r == MII_OK) {
			body->vbone = malloc(cap + 1);
			body->vpos = malloc(sizeof(float) * 3 * (cap + 1));
			body->vnrm = malloc(sizeof(float) * 3 * (cap + 1));
			if (!body->vbone || !body->vpos || !body->vnrm)
				r = MII_E_MEM;
		}
		for (i = 0; i < n && r == MII_OK; ++i) {
			const char *name;
			uint32_t o;
			int node, pv, pn;

			group_entry(b, og, i, &name, &o);
			node = (int32_t) rd32(b, o + 8);
			pv = (int16_t) rd16(b, o + 0x48);
			if (node >= 0) {   /* single-bone objects: the shadow quad, flat on its bone */
				int k;

				if (node < MII_BODY_MATRICES && body->matrix_bone[node] >= 0 && pv >= 0 && pv < nv &&
						pos[pv] && body->shadow < 0) {
					body->shadow = body->matrix_bone[node];
					for (k = 0; k < npos[pv]; ++k)
						if (fabsf(pos[pv][3 * k]) > body->shadow_half)
							body->shadow_half = fabsf(pos[pv][3 * k]);
				}
				continue;
			}
			pn = (int16_t) rd16(b, o + 0x4A);
			if (pv < 0 || pv >= nv || pn < 0 || pn >= no || !pos[pv]) {
				r = MII_E_FORMAT;
				break;
			}
			/* The trousers are the array named for them; the rest is the shirt. */
			r = read_object(b, o, body, pos[pv], npos[pv], nrm[pn], nnrm[pn],
							strstr(vname[pv], "pants") ? 1 : 0, pv, pn);
		}
	}
	for (i = 0; i < 8; ++i) {
		free(pos[i]);
		free(nrm[i]);
	}
	if (r == MII_OK && (!body->ntris[0] || !body->ntris[1]))
		r = MII_E_FORMAT;
	if (r == MII_OK) {   /* the whole body's triangles, without the strips' degenerate ones */
		int m, t;

		for (m = 0; m < 2; ++m)
			for (t = 0; t < body->ntris[m]; ++t) {
				const uint16_t *c = body->tris[m] + 3 * t;

				body->full_tris += !same_spot(body, c[0], c[1]) && !same_spot(body, c[1], c[2]) &&
								   !same_spot(body, c[0], c[2]);
			}
	}
	return r;
}

/* ---- CHR0 v3 ------------------------------------------------------------------ */

/* Code bits: 1 identity, 4 uniform scale, 5 rotation zero, 6 translation
 * zero, 13-21
 * constant X Y Z of scale, rotation, translation, 22-24 has scale,
 * rotation, translation, 25-26 the rotation keys' format (1: a u32 of 8
 * bits frame, 12 bits angle in 4096ths of a turn, 12 bits signed slope in
 * 32nds of a degree a frame, as bodyWallow's; 2: u16 frame in 32nds, s16
 * angle and slope in 65536ths of a turn; 3: f32 frame, value, slope), each
 * after a header of 8 bytes (u16 count). Translation and scale keys are f32
 * frame, value, slope. */
static float key_f(const uint8_t *p);

static int read_anim(buf *b, uint32_t c, mii_body *body, const char *name, mii_anim *a) {
	uint32_t grp = c + rd32(b, c + 0x10);
	int n, i;

	if (rd32(b, c + 8) != 3)
		return MII_E_FORMAT;
	memset(a, 0, sizeof(*a));
	snprintf(a->name, sizeof(a->name), "%s", name);
	a->frames = rd16(b, c + 0x1C);
	a->loop = rd32(b, c + 0x20) != 0;
	n = group_count(b, grp);
	if (n < 0 || a->frames <= 0)
		return MII_E_FORMAT;
	for (i = 0; i < n; ++i) {
		const char *bname;
		uint32_t e, code, p;
		int bone, comp, ax;

		group_entry(b, grp, i, &bname, &e);
		bone = bone_named(body, bname);
		if (bone < 0)
			continue;   /* nw4r_root, a camera target */
		code = rd32(b, e + 4);
		a->has[bone] = 1;
		a->track[bone].code = code;
		p = e + 8;
		for (comp = 0; comp < 3; ++comp) {   /* scale, rotation, translation */
			int axes;

			if (!(code >> (22 + comp) & 1))
				continue;
			axes = comp == 0 && (code >> 4 & 1) ? 1 : 3;
			for (ax = 0; ax < axes; ++ax, p += 4) {
				int slot = 3 * comp + ax;

				if (code >> (13 + 3 * comp + ax) & 1) {
					a->track[bone].c[slot] = rdf(b, p);
					if (!sane(a->track[bone].c[slot]))
						return MII_E_FORMAT;
				} else {
					uint32_t k = e + rd32(b, p), j;
					unsigned keys = rd16(b, k), fmt = comp == 1 ? (code >> 25 & 3) : 3;

					if (comp == 1 && !fmt)
						return MII_E_FORMAT;
					/* At most a key a frame (keys_at reads them each draw). */
					if (!keys || keys > (unsigned) a->frames + 1 ||
							!in(b, k + 8, keys * (fmt == 1 ? 4 : fmt == 2 ? 6 : 12)))
						return MII_E_FORMAT;
					/* Float keys finite and in reach: an infinite or huge
					 * angle would never come within 180 of the last. */
					for (j = 0; fmt == 3 && j < 3 * keys; ++j)
						if (!sane(key_f(b->d + k + 8 + 4 * j)))
							return MII_E_FORMAT;
					a->track[bone].keys[slot] = b->d + k;
				}
			}
			if (axes == 1) {
				a->track[bone].c[1] = a->track[bone].c[2] = a->track[bone].c[0];
				a->track[bone].keys[1] = a->track[bone].keys[2] = a->track[bone].keys[0];
			}
		}
	}
	return b->bad ? MII_E_FORMAT : MII_OK;
}

static float key_f(const uint8_t *p) {
	uint32_t u = (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
	float f;

	memcpy(&f, &u, 4);
	return f;
}

/* Hermite between keys; fmt: the keys' format (above: 1, 2, or 3 for f32);
 * angle: a rotation,
 * which takes the short way round between keys (the right ankle's X in
 * bodyWait goes 179.1, -179.2, 179.1 degrees: a nod, not two spins). */
static float keys_at(const uint8_t *k, unsigned fmt, int angle, float f) {
	unsigned n = (unsigned) (k[0] << 8 | k[1]), i;
	float f0 = 0, v0 = 0, t0 = 0;

	for (i = 0; i < n; ++i) {
		float fi, vi, ti;

		if (fmt == 1) {
			const uint8_t *e = k + 8 + 4 * i;
			int slope = (e[2] & 0xF) << 8 | e[3];

			fi = e[0];
			vi = (float) (e[1] << 4 | e[2] >> 4) * 360 / 4096;
			ti = (float) (slope >= 0x800 ? slope - 0x1000 : slope) / 32;
		} else if (fmt == 2) {
			const uint8_t *e = k + 8 + 6 * i;

			fi = (float) (e[0] << 8 | e[1]) / 32;
			vi = (float) (int16_t) (e[2] << 8 | e[3]) * 360 / 65536;
			ti = (float) (int16_t) (e[4] << 8 | e[5]) * 360 / 65536;
		} else {
			const uint8_t *e = k + 8 + 12 * i;

			fi = key_f(e);
			vi = key_f(e + 4);
			ti = key_f(e + 8);
		}
		if (i == 0 && f <= fi)
			return vi;
		if (angle && i > 0)
			vi = v0 + remainderf(vi - v0, 360);   /* within 180 of the last */
		if (i > 0 && f <= fi) {
			float dt = fi - f0, s = dt > 0 ? (f - f0) / dt : 0, s2 = s * s, s3 = s2 * s;

			return v0 * (2 * s3 - 3 * s2 + 1) + vi * (-2 * s3 + 3 * s2) +
				   (s3 - 2 * s2 + s) * t0 * dt + (s3 - s2) * ti * dt;
		}
		f0 = fi;
		v0 = vi;
		t0 = ti;
	}
	return v0;
}

static float track_at(const mii_anim *a, int bone, int slot, float f) {
	const uint8_t *k = a->track[bone].keys[slot];

	if (!k)
		return a->track[bone].c[slot];
	return keys_at(k, slot >= 3 && slot < 6 ? a->track[bone].code >> 25 & 3 : 3, slot >= 3 && slot < 6, f);
}

/* ---- Loading -------------------------------------------------------------- */

/* A TEX0 of format I8 (all the bodies' are) into tex; anything else is
 * left out. */
static void read_tex(buf *b, uint32_t t, mii_body_tex *tex) {
	uint32_t w = rd16(b, t + 0x1C), h = rd16(b, t + 0x1E), data = t + rd32(b, t + 0x10);

	if (b->bad || memcmp(b->d + t, "TEX0", 4) || rd32(b, t + 0x20) != 1 || !w || !h || w > 1024 || h > 1024 ||
			w % 8 || h % 4 || !in(b, data, w * h) || tex->gx)
		return;
	tex->gx = gpu_copy(b->d + data, w * h, NULL);
	tex->w = (uint16_t) w;
	tex->h = (uint16_t) h;
}

void mii_body_free(mii_body *body) {
	int i;

	if (!body)
		return;
	free(body->data);
	free(body->vbone);
	free(body->vpos);
	free(body->vnrm);
	free(body->tris[0]);
	free(body->tris[1]);
	free(body->memo);
	free(body->env.gx);
	free(body->shadow_tex.gx);
	for (i = 0; i < MII_BODY_LODS; ++i) {
		int k;

		for (k = 0; k < MII_BODY_OBJS; ++k)
			free(body->lod[i].dl[k]);
	}
	for (i = 0; i < MII_BODY_OBJS; ++i)
		free(body->obj[i].dl);
	for (i = 0; i < 8; ++i) {
		free(body->pos_array[i].gx);
		free(body->nrm_array[i].gx);
	}
	memset(body, 0, sizeof(*body));
}

int mii_body_attach(mii_body *body, void *data, size_t size) {
	buf b;
	int nf, i, j, r = MII_E_FORMAT, model = 0;
	uint32_t root;
	const char *env = NULL, *shadow = NULL;

	if (!body || !data)
		return MII_E_ARG;
	memset(body, 0, sizeof(*body));
	body->data = data;
	body->size = (uint32_t) size;
	b.d = data;
	b.size = (uint32_t) size;
	b.bad = 0;
	body->shadow = -1;
	if (size < 0x20 || size > 0x1000000 || memcmp(data, "bres", 4))
		goto fail;
	root = rd16(&b, 0x0C);
	nf = group_count(&b, root + 8);
	if (nf <= 0)
		goto fail;
	for (i = 0; i < nf; ++i) {   /* 3DModels(NW4R) first: animations need its bones */
		const char *folder;
		uint32_t fat;
		int n;

		group_entry(&b, root + 8, i, &folder, &fat);
		if (model || strcmp(folder, "3DModels(NW4R)"))
			continue;   /* the first model only */
		n = group_count(&b, fat);
		if (n > 0) {
			const char *name;
			uint32_t m;

			group_entry(&b, fat, 0, &name, &m);
			if (in(&b, m, 0x40) && !memcmp(body->data + m, "MDL0", 4)) {
				if ((r = read_model(&b, m, body, &env, &shadow)) < 0)
					goto fail;
				model = 1;
			}
		}
	}
	if (!model) {
		r = MII_E_FORMAT;
		goto fail;
	}
	for (i = 0; i < nf; ++i) {
		const char *folder;
		uint32_t fat;
		int n;

		group_entry(&b, root + 8, i, &folder, &fat);
		if (strcmp(folder, "AnmChr(NW4R)"))
			continue;
		n = group_count(&b, fat);
		for (j = 0; j < n && body->nanims < MII_BODY_ANIMS; ++j) {
			const char *name;
			uint32_t c;

			group_entry(&b, fat, j, &name, &c);
			b.bad = 0;   /* one bad animation skips that one only */
			if (!in(&b, c, 0x28) || memcmp(body->data + c, "CHR0", 4))
				continue;
			if (read_anim(&b, c, body, name, &body->anim[body->nanims]) == MII_OK)
				++body->nanims;
		}
	}
	for (i = 0; i < nf; ++i) {   /* the materials' textures, by name */
		const char *folder;
		uint32_t fat;
		int n;

		group_entry(&b, root + 8, i, &folder, &fat);
		if (strcmp(folder, "Textures(NW4R)"))
			continue;
		n = group_count(&b, fat);
		for (j = 0; j < n; ++j) {
			const char *name;
			uint32_t t;

			group_entry(&b, fat, j, &name, &t);
			b.bad = 0;
			if (env && !strcmp(name, env))
				read_tex(&b, t, &body->env);
			if (shadow && !strcmp(name, shadow))
				read_tex(&b, t, &body->shadow_tex);
		}
	}
	if (!body->shadow_tex.gx || body->shadow_half <= 0)
		body->shadow = -1;
	body->wait_anim = mii_body_anim(body, "bodyWait");
	return MII_OK;
fail:
	body->data = NULL;   /* the caller keeps its data on failure */
	mii_body_free(body);
	return r;
}

int mii_body_load(mii_body *body, const char *path) {
	FILE *f;
	long size;
	void *data;
	int r;

	if (!body)
		return MII_E_ARG;
	memset(body, 0, sizeof(*body));
	f = fopen(path ? path : MII_BODY_MALE_PATH, "rb");
	if (!f)
		return errno == ENOENT || errno == ENOTDIR ? MII_E_NO_FILE : MII_E_IO;
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 || size > 0x1000000 || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return MII_E_FORMAT;
	}
	data = malloc(size);
	if (!data) {
		fclose(f);
		return MII_E_MEM;
	}
	if (fread(data, 1, size, f) != (size_t) size) {
		fclose(f);
		free(data);
		return MII_E_IO;
	}
	fclose(f);
	r = mii_body_attach(body, data, size);
	if (r < 0)
		free(data);
	return r;
}

int mii_body_anim(const mii_body *body, const char *name) {
	int i;

	for (i = 0; body && name && i < body->nanims; ++i)
		if (!strcmp(body->anim[i].name, name))
			return i;
	return -1;
}

/* ---- Posing -------------------------------------------------------------------- */

/* An animation and its frame for a pose's anim (1 + index; 0 or out of
 * range: Wait's first frame, standing with the arms down). */
static const mii_anim *pick_anim(const mii_body *body, int anim1, float frame, float *f) {
	int anim = anim1 - 1;
	const mii_anim *a;

	*f = 0;
	if (anim < 0 || anim >= body->nanims)
		anim = body->wait_anim;   /* found once, at load */
	a = anim >= 0 ? &body->anim[anim] : NULL;
	if (a && anim1 > 0 && anim1 <= body->nanims) {
		*f = fmodf(frame, (float) a->frames);
		if (*f < 0)
			*f += a->frames;
	}
	return a;
}

/* A bone's own rotation (degrees), translation and scale at frame f of a. */
static void bone_local(const mii_body *body, const mii_anim *a, float f, int i, float r[3], float t[3],
					   float s[3]) {
	const mii_bone *bn = &body->bone[i];
	/* The roots only place a Mii in the plaza (aslope_root tilts it for
	 * the sloped ground): they stay as bound. */
	int animated = a && a->has[i] && bn->parent >= 0 && strcmp(bn->name, "aslope_root") &&
				   strcmp(bn->name, "position_root"), k;

	memcpy(r, bn->r, sizeof(float) * 3);
	memcpy(t, bn->t, sizeof(float) * 3);
	memcpy(s, bn->s, sizeof(float) * 3);
	if (animated) {
		uint32_t code = a->track[i].code;

		if (code >> 22 & 1)
			for (k = 0; k < 3; ++k)
				s[k] = track_at(a, i, k, f);

		if (code & 2) {
			r[0] = r[1] = r[2] = t[0] = t[1] = t[2] = 0;
		} else {
			if (code >> 23 & 1)
				for (k = 0; k < 3; ++k)
					r[k] = track_at(a, i, 3 + k, f);
			else if (code & 0x20)
				r[0] = r[1] = r[2] = 0;
			if (code >> 24 & 1)
				for (k = 0; k < 3; ++k)
					t[k] = track_at(a, i, 6 + k, f);
			else if (code & 0x40)
				t[0] = t[1] = t[2] = 0;
		}
	}
}

/* Rotation matrix <-> unit quaternion (w, x, y, z). */
static void m33_quat(m33 m, float q[4]) {
	float tr = m[0][0] + m[1][1] + m[2][2], s;

	if (tr > 0) {
		s = sqrtf(tr + 1) * 2;
		q[0] = s / 4;
		q[1] = (m[2][1] - m[1][2]) / s;
		q[2] = (m[0][2] - m[2][0]) / s;
		q[3] = (m[1][0] - m[0][1]) / s;
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		s = sqrtf(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
		q[0] = (m[2][1] - m[1][2]) / s;
		q[1] = s / 4;
		q[2] = (m[0][1] + m[1][0]) / s;
		q[3] = (m[0][2] + m[2][0]) / s;
	} else if (m[1][1] > m[2][2]) {
		s = sqrtf(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
		q[0] = (m[0][2] - m[2][0]) / s;
		q[1] = (m[0][1] + m[1][0]) / s;
		q[2] = s / 4;
		q[3] = (m[1][2] + m[2][1]) / s;
	} else {
		s = sqrtf(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
		q[0] = (m[1][0] - m[0][1]) / s;
		q[1] = (m[0][2] + m[2][0]) / s;
		q[2] = (m[1][2] + m[2][1]) / s;
		q[3] = s / 4;
	}
}

static void quat_m33(const float q[4], m33 m) {
	float w = q[0], x = q[1], y = q[2], z = q[3];

	m[0][0] = 1 - 2 * (y * y + z * z);
	m[0][1] = 2 * (x * y - w * z);
	m[0][2] = 2 * (x * z + w * y);
	m[1][0] = 2 * (x * y + w * z);
	m[1][1] = 1 - 2 * (x * x + z * z);
	m[1][2] = 2 * (y * z - w * x);
	m[2][0] = 2 * (x * z - w * y);
	m[2][1] = 2 * (y * z + w * x);
	m[2][2] = 1 - 2 * (x * x + y * y);
}

/* a toward b by w (0 a, 1 b), the short way round, kept unit length. */
static void blend_rot(m33 a, m33 b, float w, m33 out) {
	float qa[4], qb[4], q[4], d = 0, l = 0;
	int k;

	m33_quat(a, qa);
	m33_quat(b, qb);
	for (k = 0; k < 4; ++k)
		d += qa[k] * qb[k];
	for (k = 0; k < 4; ++k) {
		q[k] = qa[k] * (1 - w) + (d < 0 ? -qb[k] : qb[k]) * w;
		l += q[k] * q[k];
	}
	l = l > 0 ? 1 / sqrtf(l) : 1;
	for (k = 0; k < 4; ++k)
		q[k] *= l;
	quat_m33(q, out);
}

float mii_anim_frame(const mii_body *body, int anim, float seconds, int *done) {
	const mii_anim *a = anim > 0 && anim <= body->nanims ? &body->anim[anim - 1] : NULL;
	float f = seconds > 0 ? seconds * 60 : 0;

	if (done)
		*done = 0;
	if (!a || a->loop)
		return f;   /* frames wrap as they are drawn */
	if (f >= a->frames - 0.01f) {
		/* The end, a hair before it: frames wrap at a->frames. */
		if (done)
			*done = 1;
		return a->frames - 0.01f;
	}
	return f;
}

float mii_fade_weight(float seconds, float length) {
	float x;

	if (!(length > 0) || !(seconds < length))
		return 0;
	x = seconds > 0 ? 1 - seconds / length : 1;
	return x * x * (3 - 2 * x);   /* eased in and out */
}

/* Faces for the animations: nothing in the body's file says which (no
 * SRT0, PAT0 or CLR0; the Mii Channel's own program changes them), so
 * these are this library's, by eye: parts of an animation (as fractions of
 * its length) and the face they get. An animation named by its start. */
static const struct {
	const char *anim;
	float from, to;
	int expr;
} face_cues[] = {
	{ "bodyCatnap_00", 0, 1, MII_EXPR_BLINK },        /* asleep */
	{ "bodyCatnap_01", 0, 0.5f, MII_EXPR_BLINK },     /* waking */
	{ "bodyCatnap_01", 0.5f, 0.7f, MII_EXPR_SURPRISE },
	{ "bodyGreeting", 0, 1, MII_EXPR_SMILE },
	{ "bodySlap_00", 0.45f, 1, MII_EXPR_ANGER },      /* flung, and cross about it */
	{ "bodySlap_01", 0.45f, 1, MII_EXPR_SMILE },      /* flung, and liked it */
	{ "bodySlap_02", 0.45f, 1, MII_EXPR_SMILE },
	{ "bodySlap_03", 0.45f, 1, MII_EXPR_ANGER },
	{ "bodySlap_04", 0.45f, 1, MII_EXPR_SMILE },
	{ "bodySlap_05", 0.45f, 1, MII_EXPR_SORROW },
	{ "bodySlap_", 0, 0.45f, MII_EXPR_SURPRISE },     /* flung (Slap_06: only that) */
	{ "bodySlip_", 0, 0.55f, MII_EXPR_SURPRISE },     /* falling */
	{ "bodySlip_", 0.55f, 1, MII_EXPR_SORROW },
	{ "bodysnart_", 0.2f, 0.45f, MII_EXPR_OPEN_MOUTH },  /* the sneeze coming */
	{ "bodysnart_", 0.45f, 0.65f, MII_EXPR_BLINK },
	{ "bodyWallow", 0, 1, MII_EXPR_SURPRISE },        /* held up */
};

int mii_anim_expression(const mii_body *body, int anim, float frame, int other) {
	const mii_anim *a;
	float x;
	size_t i;

	if (!body || anim < 1 || anim > body->nanims)
		return other;
	a = &body->anim[anim - 1];
	if (!strncmp(a->name, "bodyTalk", 8))   /* the mouth opening and closing, about four times a second */
		return (int) (frame / 7) % 2 ? MII_EXPR_OPEN_MOUTH : other;
	x = fmodf(frame, (float) a->frames) / a->frames;
	if (x < 0)
		x += 1;
	for (i = 0; i < sizeof(face_cues) / sizeof(face_cues[0]); ++i)
		if (!strncmp(a->name, face_cues[i].anim, strlen(face_cues[i].anim)) && x >= face_cues[i].from &&
				x < face_cues[i].to)
			return face_cues[i].expr;
	return other;
}

/* Each bone's local rotation and translation for the pose (no Mii's scale
 * yet). A drawn pose (not the bind pose a model is set up with, maybe on
 * another thread) is remembered in the body, and found again for the next
 * Mii at the same frame. */
static const mii_pose_memo *pose_locals(const mii_body *body, const mii_pose *pose, mii_pose_memo *own) {
	mii_body *b = (mii_body *) body;   /* the memo is scratch */
	const mii_anim *a, *a2 = NULL;
	float f, f2 = 0, w = 0;
	mii_pose_memo *out = own;
	int i, k;

	a = pick_anim(body, pose ? pose->anim : 0, pose ? pose->frame : 0, &f);
	if (pose && pose->from_weight > 0) {
		w = pose->from_weight < 1 ? pose->from_weight : 1;
		a2 = pick_anim(body, pose->from_anim, pose->from_frame, &f2);
	}
	if (pose) {
		if (!b->memo && (b->memo = calloc(MII_POSE_MEMO, sizeof(mii_pose_memo))))
			for (i = 0; i < MII_POSE_MEMO; ++i)
				b->memo[i].f = -1;   /* no frame is negative: empty */
		for (i = 0; b->memo && i < MII_POSE_MEMO; ++i) {
			const mii_pose_memo *m = &b->memo[i];

			if (m->a == a && m->f == f && m->w == w && (!w || (m->a2 == a2 && m->f2 == f2))) {
				++b->memo_hits;
				return m;
			}
		}
		++b->memo_misses;
		if (b->memo) {
			out = &b->memo[b->memo_next];
			b->memo_next = (b->memo_next + 1) % MII_POSE_MEMO;
		}
	}
	out->a = a;
	out->a2 = a2;
	out->f = f;
	out->f2 = f2;
	out->w = w;
	for (i = 0; i < body->nbones; ++i) {
		float r[3];

		bone_local(body, a, f, i, r, out->t[i], out->s[i]);
		rot_xyz(out->r[i], r);
		if (w > 0) {   /* the cross-fade from the animation before */
			float r2[3], t2[3], s2[3];
			m33 from;

			bone_local(body, a2, f2, i, r2, t2, s2);
			rot_xyz(from, r2);
			blend_rot(out->r[i], from, w, out->r[i]);
			for (k = 0; k < 3; ++k) {
				out->t[i][k] += (t2[k] - out->t[i][k]) * w;
				out->s[i][k] += (s2[k] - out->s[i][k]) * w;
			}
		}
	}
	return out;
}

void mii_body_pose(const mii_model *model, const mii_pose *pose, float rot[][3][3], float pos[][3]) {
	const mii_body *body = model ? model->body : NULL;
	const mii_pose_memo *lp;
	mii_pose_memo own;
	int i, k;

	if (!body || !rot || !pos)
		return;   /* no Wii body set on this model */
	lp = pose_locals(body, pose, &own);
	for (i = 0; i < body->nbones; ++i) {
		const mii_bone *bn = &body->bone[i];
		const float *t = lp->t[i];
		m33 local;

		memcpy(local, lp->r[i], sizeof(local));
		if (bn->parent < 0) {
			memcpy(rot[i], local, sizeof(local));
			memcpy(pos[i], t, sizeof(lp->t[i]));
		} else {
			const float *ps = model->bone_scale[bn->parent];
			float st[3] = { t[0] * ps[0], t[1] * ps[1], t[2] * ps[2] };

			mul33(rot[i], rot[bn->parent], local);
			for (k = 0; k < 3; ++k)
				pos[i][k] = pos[bn->parent][k] + rot[bn->parent][k][0] * st[0] +
							rot[bn->parent][k][1] * st[1] + rot[bn->parent][k][2] * st[2];
		}
	}
	for (i = 0; i < body->nbones; ++i)
		pos[i][1] += model->ground;
}

int mii_body_shadow(const mii_model *model, const mii_pose *pose, float m[3][4]) {
	const mii_body *body = model ? model->body : NULL;
	float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3], w;
	const mii_pose_memo *lp;
	mii_pose_memo own;
	const float *sc;
	int sb, r, j;

	if (!body || body->shadow < 0 || body->shadow >= body->nbones || !m)
		return 0;
	sb = body->shadow;
	mii_body_pose(model, pose, rot, pos);
	lp = pose_locals(body, pose, &own);   /* the memo mii_body_pose just filled */
	sc = lp->s[sb];
	if (fabsf(sc[0]) < 0.02f && fabsf(sc[2]) < 0.02f)
		return 0;
	/* As wide as the Mii: its build's width, the chest's. */
	w = body->shadow_half * model->bone_scale[body->chest >= 0 ? body->chest : body->head][1];
	for (r = 0; r < 3; ++r) {
		for (j = 0; j < 3; ++j)
			m[r][j] = rot[sb][r][j] * sc[j] * w;
		m[r][3] = pos[sb][r];
	}
	if (pose && pose->shadow_drop > 0) {   /* on the ground below: smaller, as a light overhead casts it */
		float k = 1 / (1 + pose->shadow_drop / 120);

		for (r = 0; r < 3; ++r)
			for (j = 0; j < 3; ++j)
				m[r][j] *= k;
		m[1][3] -= pose->shadow_drop;
	}
	m[1][3] += 0.1f;   /* a hair over the ground under it */
	return 1;
}

int mii_fx_place(const mii_model *model, const mii_pose *pose, int head, float at[3][4]) {
	/* The head bone's axes as bound: X up the neck, Y out of the face, Z
	 * to the Mii's left. */
	static const float bind_head[3][3] = { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } };
	float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3], f[3][3], o[3] = { 0, 0, 0 }, k = 1, cy, sy;
	const mii_body *body = model ? model->body : NULL;
	int i, j, wb = body && !(pose && pose->head_only);

	if (!model || !at)
		return MII_E_ARG;
	memcpy(f, bind_head, sizeof(f));
	if (!head)   /* the ground: upright, turned with the Mii */
		for (i = 0; i < 3; ++i)
			for (j = 0; j < 3; ++j)
				f[i][j] = i == j;
	if (wb) {
		k = model->bone_scale[body->chest >= 0 ? body->chest : body->head][0];
		mii_body_pose(model, pose, rot, pos);
		if (head) {   /* the head bone as posed: the files' pSphere14 */
			memcpy(f, rot[body->head], sizeof(f));
			memcpy(o, pos[body->head], sizeof(o));
		} else {      /* under the lowest bone: the feet, or a Mii flat on the ground */
			int lo = 0;

			for (i = 1; i < body->nbones; ++i)
				if (body->bone[i].parent >= 0 && i != body->shadow && (lo == 0 || pos[i][1] < pos[lo][1]))
					lo = i;
			o[0] = pos[lo][0];
			o[2] = pos[lo][2];
		}
	} else if (head && !(pose && pose->head_only)) {
		o[1] = model->head_y;   /* the built body: where the chin sits */
	}
	if (!(pose && pose->head_only))
		o[1] += pose ? pose->bob : 0;
	/* Turned with the Mii about Y, as mii_draw turns it. */
	cy = cosf(pose ? pose->yaw : 0);
	sy = sinf(pose ? pose->yaw : 0);
	for (j = 0; j < 3; ++j) {
		at[0][j] = (cy * f[0][j] + sy * f[2][j]) * k;
		at[1][j] = f[1][j] * k;
		at[2][j] = (-sy * f[0][j] + cy * f[2][j]) * k;
	}
	at[0][3] = cy * o[0] + sy * o[2];
	at[1][3] = o[1];
	at[2][3] = -sy * o[0] + cy * o[2];
	return MII_OK;
}

#ifndef MII_NO_OGC
void mii_body_world(const mii_model *model, const mii_pose *pose, Mtx world[]) {
	const mii_body *body = model ? model->body : NULL;
	const mii_pose_memo *lp;
	mii_pose_memo own;
	int i;

	if (!body || !world)
		return;
	lp = pose_locals(body, pose, &own);
	for (i = 0; i < body->nbones; ++i) {
		const mii_bone *bn = &body->bone[i];
		const float *t = lp->t[i];
		Mtx local;
		int r;

		for (r = 0; r < 3; ++r) {
			local[r][0] = lp->r[i][r][0];
			local[r][1] = lp->r[i][r][1];
			local[r][2] = lp->r[i][r][2];
		}
		if (bn->parent < 0) {   /* a root: the ground under its feet */
			local[0][3] = t[0];
			local[1][3] = t[1] + model->ground;
			local[2][3] = t[2];
			guMtxCopy(local, world[i]);
		} else {   /* its offset in the parent's scaled frame */
			const float *ps = model->bone_scale[bn->parent];

			local[0][3] = t[0] * ps[0];
			local[1][3] = t[1] * ps[1];
			local[2][3] = t[2] * ps[2];
			guMtxConcat(world[bn->parent], local, world[i]);
		}
	}
}
#endif

void mii_body_skin(mii_model *model, const mii_pose *pose) {
	const mii_body *body = model ? model->body : NULL;
	float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3];
	int i, k;

	if (!body)
		return;
	if (!model->skin_pos)
		model->skin_pos = malloc(sizeof(float) * 3 * body->nverts);
	if (!model->skin_nrm)
		model->skin_nrm = malloc(sizeof(float) * 3 * body->nverts);
	if (!model->skin_pos || !model->skin_nrm)
		return;

	mii_body_pose(model, pose, rot, pos);
	for (i = 0; i < body->nverts; ++i) {
		int bn = body->vbone[i];
		const float *s = model->bone_scale[bn], *p = body->vpos + 3 * i, *n = body->vnrm + 3 * i;
		float q[3] = { p[0] * s[0], p[1] * s[1], p[2] * s[2] };
		float m[3] = { n[0] / s[0], n[1] / s[1], n[2] / s[2] }, l = 0;
		float *op = model->skin_pos + 3 * i, *on = model->skin_nrm + 3 * i;

		for (k = 0; k < 3; ++k) {
			op[k] = pos[bn][k] + rot[bn][k][0] * q[0] + rot[bn][k][1] * q[1] + rot[bn][k][2] * q[2];
			on[k] = rot[bn][k][0] * m[0] + rot[bn][k][1] * m[1] + rot[bn][k][2] * m[2];
			l += on[k] * on[k];
		}
		l = l > 0 ? 1 / sqrtf(l) : 1;
		for (k = 0; k < 3; ++k)
			on[k] *= l;
	}
}

void mii_body_matrices(const mii_model *model, const mii_pose *pose, float pm[][3][4], float nm[][3][3]) {
	float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3];

	if (!model || !model->body || !pm || !nm)
		return;
	mii_body_pose(model, pose, rot, pos);
	mii_body_matrices_posed(model, rot, pos, pm, nm);
}

void mii_body_matrices_posed(const mii_model *model, float rot[][3][3], float pos[][3], float pm[][3][4],
							 float nm[][3][3]) {
	const mii_body *body = model->body;
	int i, j, k;

	for (i = 0; i < body->nbones; ++i) {
		const float *s = model->bone_scale[i];

		for (j = 0; j < 3; ++j) {
			for (k = 0; k < 3; ++k) {
				pm[i][j][k] = rot[i][j][k] * s[k];   /* scale first, in the bone's frame */
				nm[i][j][k] = rot[i][j][k];
			}
			pm[i][j][3] = pos[i][j];
		}
	}
}

/* Each bone's own scale from the Mii's height and build, in the bone's
 * frame, where every limb runs along X: chest, hips, arms and legs long by
 * height and thick by build; wrists and ankles round; the head bone never
 * shorter. Not inherited: a child only follows its parent's scaled offset.
 * Middle height and build are 1. */
static void body_scales(mii_model *model) {
	const mii *m = &model->m;
	float h = m->height, b = m->build;
	float sx = ((b * (h * 0.47f / 128 + 0.4f)) / 128 + h * 0.23f / 128 + 0.4f) / 0.8325f;
	float sy = (h * 0.77f / 128 + 0.5f) / 0.885f;
	int i;

	for (i = 0; i < model->body->nbones; ++i) {
		const char *n = model->body->bone[i].name;
		float *s = model->bone_scale[i];

		if (!strncmp(n, "wrist", 5) || !strncmp(n, "ankle", 5)) {
			s[0] = s[1] = s[2] = sx;
		} else if (!strcmp(n, "head")) {
			s[0] = sy > 1 ? sy : 1;
			s[1] = s[2] = sx;
		} else if (!strcmp(n, "chest") || !strcmp(n, "hip") || !strncmp(n, "arm_", 4) ||
				   !strncmp(n, "foot_", 5)) {
			s[0] = sy;
			s[1] = s[2] = sx;
		} else {
			s[0] = s[1] = s[2] = 1;
		}
	}
}

/* ---- A model with a Wii body ------------------------------------------------- */

int mii_model_set_body(mii_model *model, const mii_body *body) {
	float min = 1e9f;
	int i;

	int pal_size;
	void *pal_pos, *pal_nrm, *pal_env, *last_pm, *last_w;

	if (!model || !body || !body->nverts)
		return MII_E_ARG;
	/* The palettes: an entry per matrix id the body uses, rounded up to 8
	 * (48 and 36 bytes an entry: every palette starts 32-byte aligned),
	 * MII_PALETTES of them. All made before anything is freed: on failure
	 * the model keeps the body it had. */
	pal_size = (body->nmatrices + 7) & ~7;
#ifndef MII_NO_OGC
	pal_pos = memalign(32, sizeof(float) * 12 * pal_size * MII_PALETTES);
	pal_nrm = memalign(32, sizeof(float) * 9 * pal_size * MII_PALETTES);
	pal_env = memalign(32, sizeof(float) * 12 * pal_size * MII_PALETTES);
#else
	pal_pos = malloc(sizeof(float) * 12 * pal_size * MII_PALETTES);
	pal_nrm = malloc(sizeof(float) * 9 * pal_size * MII_PALETTES);
	pal_env = malloc(sizeof(float) * 12 * pal_size * MII_PALETTES);
#endif
	last_pm = malloc(sizeof(float) * 12 * body->nbones);
	last_w = malloc(sizeof(float) * 12 * body->nbones);
	if (!pal_pos || !pal_nrm || !pal_env || !last_pm || !last_w) {
		free(pal_pos);
		free(pal_nrm);
		free(pal_env);
		free(last_pm);
		free(last_w);
		return MII_E_MEM;
	}
	model->body = body;
	body_scales(model);
	for (i = MII_HEAD_PARTS; i < MII_PARTS; ++i)
		mii_mesh_free(&model->part[i]);   /* the built body: not drawn again */
	free(model->skin_pos);
	free(model->skin_nrm);
	model->skin_pos = model->skin_nrm = NULL;   /* mii_body_skin makes them if it is called */
	free(model->pal_pos);
	free(model->pal_nrm);
	free(model->pal_env);
	free(model->last_pm);
	free(model->last_w);
	model->pal_size = pal_size;
	model->pal_pos = pal_pos;
	model->pal_nrm = pal_nrm;
	model->pal_env = pal_env;
	model->last_pm = last_pm;
	model->last_w = last_w;
	model->posed = 0;
	model->pal_next = 0;
	/* The feet on Y = 0 in the standing pose. */
	model->ground = 0;
	{
		float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3];

		mii_body_pose(model, NULL, rot, pos);
		for (i = 0; i < body->nverts; ++i) {
			int bn = body->vbone[i];
			const float *s = model->bone_scale[bn], *v = body->vpos + 3 * i;
			float y = pos[bn][1] + rot[bn][1][0] * v[0] * s[0] + rot[bn][1][1] * v[1] * s[1] +
					  rot[bn][1][2] * v[2] * s[2];

			if (y < min)
				min = y;
		}
	}
	model->ground = -min;
	/* Where the head sits, for framing (mii_model_bounds), and how wide. */
	{
		float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3];

		mii_body_pose(model, NULL, rot, pos);
		model->head_y = pos[body->head][1];
		model->shoulder[0] = 30 * model->bone_scale[body->head][1];
	}
	return MII_OK;
}

/* ---- Levels of detail ------------------------------------------------------- */

/* A quadric (Garland and Heckbert): the sum of squared distances to planes,
 * as the 10 numbers of a symmetric 4 x 4. */
static void quad_plane(double *q, const double n[3], double d, double w) {
	q[0] += w * n[0] * n[0]; q[1] += w * n[0] * n[1]; q[2] += w * n[0] * n[2]; q[3] += w * n[0] * d;
	q[4] += w * n[1] * n[1]; q[5] += w * n[1] * n[2]; q[6] += w * n[1] * d;
	q[7] += w * n[2] * n[2]; q[8] += w * n[2] * d;
	q[9] += w * d * d;
}

/* (a + b) at p. */
static double quad_at(const double *a, const double *b, const float *p) {
	double x = p[0], y = p[1], z = p[2], s;
	double c[10];
	int k;

	for (k = 0; k < 10; ++k)
		c[k] = a[k] + b[k];
	s = c[0] * x * x + 2 * c[1] * x * y + 2 * c[2] * x * z + 2 * c[3] * x + c[4] * y * y + 2 * c[5] * y * z +
		2 * c[6] * y + c[7] * z * z + 2 * c[8] * z + c[9];
	return s;
}

static void tri_normal(const float *a, const float *b, const float *c, double n[3]) {
	double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };

	n[0] = u[1] * v[2] - u[2] * v[1];
	n[1] = u[2] * v[0] - u[0] * v[2];
	n[2] = u[0] * v[1] - u[1] * v[0];
}

/* One run of an object's list: its matrix loads [h0, h1), then its
 * primitives [p0, p1), whose vertices are world[] from 0 on. Its vertices
 * become nodes (a slot, so a bone, and a position index: the strips repeat
 * a vertex for each normal); each triangle keeps its corners' own bytes
 * (normal and all) until a fold moves a corner, which then takes the kept
 * node's. Each level's bytes (the loads, then GX_TRIANGLES) go to out[]. */
static int lod_run(mii_body *body, const uint8_t *dl, int vs, int tex_mtx, int pos_w, uint32_t h0, uint32_t h1,
				   uint32_t p0, uint32_t p1, const float *world, const float *tol, int n, uint8_t **out,
				   uint32_t *out_len) {
	uint32_t at;
	int i, j, nn = 0, nt = 0, alive, lev, local = 0, corners = 0, r = MII_E_MEM;
	int *key = NULL, *canon = NULL, *tri = NULL, *traw = NULL;
	float *pos = NULL, *scale = NULL;
	double *q = NULL;
	uint8_t *dead = NULL;

	for (at = p0; at < p1;) {
		if (!dl[at]) {
			++at;
			continue;
		}
		corners += dl[at + 1] << 8 | dl[at + 2];
		at += 3 + (uint32_t) (dl[at + 1] << 8 | dl[at + 2]) * vs;
	}
	key = malloc(sizeof(int) * 2 * (corners + 1));
	canon = malloc(sizeof(int) * (corners + 1));
	pos = malloc(sizeof(float) * 3 * (corners + 1));
	tri = malloc(sizeof(int) * 3 * (corners + 1));
	traw = malloc(sizeof(int) * 3 * (corners + 1));
	q = calloc((size_t) (corners + 1) * 10, sizeof(double));
	dead = calloc(corners + 1, 1);
	scale = malloc(sizeof(float) * (corners + 1));
	if (!key || !canon || !pos || !tri || !traw || !q || !dead || !scale)
		goto done;
	/* Nodes and triangles. */
	for (at = p0; at < p1;) {
		uint8_t op = dl[at] & 0xF8;
		int cnt, v, nodes[3], raws[3];
		int *vn, *vr;

		if (!dl[at]) {
			++at;
			continue;
		}
		cnt = dl[at + 1] << 8 | dl[at + 2];
		at += 3;
		vn = malloc(sizeof(int) * (cnt ? cnt : 1));
		vr = malloc(sizeof(int) * (cnt ? cnt : 1));
		if (!vn || !vr) {
			free(vn);
			free(vr);
			goto done;
		}
		for (v = 0; v < cnt; ++v, at += vs, ++local) {
			int slot = dl[at], pi = pos_w == 1 ? dl[at + 1 + tex_mtx] : dl[at + 1 + tex_mtx] << 8 | dl[at + 2 + tex_mtx];

			for (j = 0; j < nn; ++j)
				if (key[2 * j] == slot && key[2 * j + 1] == pi)
					break;
			if (j == nn) {
				key[2 * nn] = slot;
				key[2 * nn + 1] = pi;
				canon[nn] = (int) at;
				memcpy(pos + 3 * nn, world + 3 * local, sizeof(float) * 3);
				scale[nn] = 0;   /* the area behind the node, below */
				++nn;
			}
			vn[v] = j;
			vr[v] = (int) at;
		}
		for (v = 0; v + 2 < cnt; ++v) {
			int a, c, d;

			if (op == 0x90) {
				if (v % 3)
					continue;
				a = v; c = v + 1; d = v + 2;
			} else if (op == 0x98) {
				a = v % 2 ? v + 1 : v;
				c = v % 2 ? v : v + 1;
				d = v + 2;
			} else {
				a = 0; c = v + 1; d = v + 2;
			}
			nodes[0] = vn[a]; nodes[1] = vn[c]; nodes[2] = vn[d];
			raws[0] = vr[a]; raws[1] = vr[c]; raws[2] = vr[d];
			if (nodes[0] == nodes[1] || nodes[1] == nodes[2] || nodes[0] == nodes[2])
				continue;   /* a strip's turn */
			memcpy(tri + 3 * nt, nodes, sizeof(nodes));
			memcpy(traw + 3 * nt, raws, sizeof(raws));
			++nt;
		}
		free(vn);
		free(vr);
	}
	/* Each node's quadric: its triangles' planes by area, and for each open
	 * edge a plane through it, square to the triangle, weighted heavily. */
	for (i = 0; i < nt; ++i) {
		const int *t = tri + 3 * i;
		double nm[3], l;
		int e;

		tri_normal(pos + 3 * t[0], pos + 3 * t[1], pos + 3 * t[2], nm);
		l = sqrt(nm[0] * nm[0] + nm[1] * nm[1] + nm[2] * nm[2]);
		if (l <= 0)
			continue;
		for (j = 0; j < 3; ++j)
			nm[j] /= l;
		for (j = 0; j < 3; ++j) {
			quad_plane(q + 10 * t[j], nm, -(nm[0] * pos[3 * t[0]] + nm[1] * pos[3 * t[0] + 1] + nm[2] * pos[3 * t[0] + 2]),
					   l / 2);
			scale[t[j]] += (float) (l / 2);
		}
		for (e = 0; e < 3; ++e) {
			int a = t[e], b = t[(e + 1) % 3], m, shared = 0;
			double d[3], sd[3], sl;

			for (m = 0; m < nt && !shared; ++m) {
				const int *o = tri + 3 * m;

				if (m != i && (o[0] == a || o[1] == a || o[2] == a) && (o[0] == b || o[1] == b || o[2] == b))
					shared = 1;
			}
			if (shared)
				continue;
			for (j = 0; j < 3; ++j)
				d[j] = pos[3 * b + j] - pos[3 * a + j];
			sd[0] = d[1] * nm[2] - d[2] * nm[1];
			sd[1] = d[2] * nm[0] - d[0] * nm[2];
			sd[2] = d[0] * nm[1] - d[1] * nm[0];
			sl = sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
			if (sl <= 0)
				continue;
			for (j = 0; j < 3; ++j)
				sd[j] /= sl;
			sl = -(sd[0] * pos[3 * a] + sd[1] * pos[3 * a + 1] + sd[2] * pos[3 * a + 2]);
			/* Weighted 100 times the edge's length squared: an open edge
			 * (a sleeve's, a collar's) folds only along itself. */
			quad_plane(q + 10 * a, sd, sl, 100 * (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
			quad_plane(q + 10 * b, sd, sl, 100 * (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
		}
	}
	/* Fold, cheapest first, while a fold moves the surface by no more than
	 * the level's tolerance; write the level. */
	alive = nt;
	for (lev = 0; lev < n; ++lev) {
		int tries = 0;
		uint8_t *o;

		while (alive > 2) {
			double best = 1e300;
			int bu = -1, bv = -1, ok = 1;

			for (i = 0; i < nt; ++i) {
				const int *t = tri + 3 * i;
				int e;

				if (dead[i])
					continue;
				for (e = 0; e < 6; ++e) {
					int u = t[e % 3], v = t[(e % 3 + (e < 3 ? 1 : 2)) % 3];
					double c;

					if (key[2 * u] != key[2 * v])
						continue;   /* another bone: the joint stays */
					/* A distance: the quadric over the area behind it.
					 * Flattening a limb moves its side by its radius. */
					c = quad_at(q + 10 * u, q + 10 * v, pos + 3 * v);
					c = sqrt((c > 0 ? c : 0) / (scale[u] + scale[v] + 1e-6));
					if (c < best) {
						best = c;
						bu = u;
						bv = v;
					}
				}
			}
			if (bu < 0 || best > tol[lev])   /* the next fold would show */
				break;
			/* No triangle around u may turn more than 60 degrees: folds
			 * that flatten a limb turn its sides over. */
			for (i = 0; i < nt && ok; ++i) {
				int *t = tri + 3 * i, h = -1;
				double n0[3], n1[3];
				const float *pp[3];

				if (dead[i])
					continue;
				for (j = 0; j < 3; ++j) {
					if (t[j] == bv)
						h = -2;
					else if (t[j] == bu && h != -2)
						h = j;
				}
				if (h < 0)
					continue;
				for (j = 0; j < 3; ++j)
					pp[j] = pos + 3 * t[j];
				tri_normal(pp[0], pp[1], pp[2], n0);
				pp[h] = pos + 3 * bv;
				tri_normal(pp[0], pp[1], pp[2], n1);
				if (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] <= 0.5 *   /* cos 60 degrees */
						sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]) *
						sqrt(n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2]))
					ok = 0;
			}
			if (!ok) {
				/* Make u dear to fold (a constant on its quadric), and look
				 * again; give up after many. */
				double none[3] = { 0, 0, 0 };

				if (++tries > 2000)
					break;
				quad_plane(q + 10 * bu, none, 1, 1e6);
				continue;
			}
			{
				float d0 = pos[3 * bu] - pos[3 * bv], d1 = pos[3 * bu + 1] - pos[3 * bv + 1],
					  d2 = pos[3 * bu + 2] - pos[3 * bv + 2], d = sqrtf(d0 * d0 + d1 * d1 + d2 * d2);

				if (d > body->lod[lev].err)
					body->lod[lev].err = d;
			}
			for (j = 0; j < 10; ++j)
				q[10 * bv + j] += q[10 * bu + j];
			for (i = 0; i < nt; ++i) {
				int *t = tri + 3 * i, has_v = 0, h = -1;

				if (dead[i])
					continue;
				for (j = 0; j < 3; ++j) {
					has_v |= t[j] == bv;
					if (t[j] == bu)
						h = j;
				}
				if (h < 0)
					continue;
				if (has_v) {
					dead[i] = 1;
					--alive;
				} else {
					t[h] = bv;
					traw[3 * i + h] = canon[bv];
				}
			}
		}
		/* The run's loads, then its triangles as strips (mii_strips: the
		 * GPU works out each vertex it is sent). A strip's vertex is a
		 * corner's bytes: corners with the same bytes are one vertex. */
		{
			uint16_t *tv = malloc(sizeof(uint16_t) * 3 * (alive + 1)), *sv = malloc(sizeof(uint16_t) * 3 * (alive + 1));
			uint16_t *lens = malloc(sizeof(uint16_t) * (alive + 1));
			int *rep = malloc(sizeof(int) * 3 * (alive + 1)), nr = 0, ns, s, k2, c = 0;

			if (!tv || !sv || !lens || !rep) {
				free(tv);
				free(sv);
				free(lens);
				free(rep);
				goto done;
			}
			for (i = 0; i < nt; ++i)
				if (!dead[i])
					for (j = 0; j < 3; ++j) {
						int raw = traw[3 * i + j], v;

						for (v = 0; v < nr; ++v)
							if (!memcmp(dl + rep[v], dl + raw, vs))
								break;
						if (v == nr)
							rep[nr++] = raw;
						tv[c++] = (uint16_t) v;
					}
			ns = mii_strips(tv, alive, sv, lens);
			if (ns < 0) {
				free(tv);
				free(sv);
				free(lens);
				free(rep);
				goto done;
			}
			out_len[lev] = h1 - h0;
			for (s = 0; s < ns; ++s)
				out_len[lev] += 3 + (uint32_t) lens[s] * vs;
			o = out[lev] = malloc(out_len[lev]);
			if (o) {
				memcpy(o, dl + h0, h1 - h0);
				at = h1 - h0;
				for (s = 0, c = 0; s < ns; ++s) {
					o[at++] = 0x98 | 1;   /* GX_TRIANGLESTRIP on GX_VTXFMT1 */
					o[at++] = (uint8_t) (lens[s] >> 8);
					o[at++] = (uint8_t) lens[s];
					for (k2 = 0; k2 < lens[s]; ++k2, ++c, at += vs)
						memcpy(o + at, dl + rep[sv[c]], vs);
				}
			}
			free(tv);
			free(sv);
			free(lens);
			free(rep);
			if (!o)
				goto done;
		}
		body->lod[lev].tris += alive;
	}
	r = MII_OK;
done:
	free(key);
	free(canon);
	free(pos);
	free(tri);
	free(traw);
	free(q);
	free(dead);
	free(scale);
	return r;
}

/* One object's levels: each run of its list on its own (a list may load
 * other bones into its slots part way: the female shirt does), the runs'
 * bytes one after another, padded to 32 with GX_NOPs. world: its vertices. */
static int lod_object(mii_body *body, int k, const float *world, const float *tol, int n) {
	mii_body_obj *ob = &body->obj[k];
	uint32_t lo = ob->vcd, at = 0, len[MII_BODY_LODS], total[MII_BODY_LODS] = { 0 };
	uint8_t *chunk[MII_BODY_LODS], *runs[16][MII_BODY_LODS];
	uint32_t runs_len[16][MII_BODY_LODS];
	int tex_mtx = 0, pos_w, vs, i, l, nr = 0, r = MII_OK, verts = 0;

	for (i = 1; i <= 8; ++i)
		tex_mtx += lo >> i & 1;
	pos_w = (lo >> 9 & 3) == 2 ? 1 : 2;
	vs = 1 + tex_mtx + pos_w + ((lo >> 11 & 3) == 2 ? 1 : 2);
	memset(runs, 0, sizeof(runs));
	memset(runs_len, 0, sizeof(runs_len));
	while (at < ob->dl_size && r == MII_OK) {
		uint32_t h0, h1, p0, p1;
		int rv = 0;

		while (at < ob->dl_size && !ob->dl[at])
			++at;
		if (at >= ob->dl_size)
			break;
		h0 = at;
		while (at < ob->dl_size && (ob->dl[at] == 0x20 || ob->dl[at] == 0x28 || ob->dl[at] == 0x30))
			at += 5;
		h1 = p0 = at;
		while (at < ob->dl_size && ob->dl[at] != 0x20 && ob->dl[at] != 0x28 && ob->dl[at] != 0x30) {
			if (!ob->dl[at]) {
				++at;
				continue;
			}
			rv += ob->dl[at + 1] << 8 | ob->dl[at + 2];
			at += 3 + (uint32_t) (ob->dl[at + 1] << 8 | ob->dl[at + 2]) * vs;
		}
		p1 = at;
		if (nr >= 16) {   /* more runs than these bodies have: the ones made are freed below */
			r = MII_E_FORMAT;
			break;
		}
		r = lod_run(body, ob->dl, vs, tex_mtx, pos_w, h0, h1, p0, p1, world + 3 * verts, tol, n, runs[nr],
					runs_len[nr]);
		for (l = 0; l < n; ++l)
			total[l] += runs_len[nr][l];
		++nr;
		verts += rv;
	}
	for (l = 0; l < n && r == MII_OK; ++l) {
		len[l] = (total[l] + 31) & ~31u;
#ifndef MII_NO_OGC
		chunk[l] = memalign(32, len[l]);
#else
		chunk[l] = malloc(len[l]);
#endif
		if (!chunk[l]) {
			r = MII_E_MEM;
			break;
		}
		memset(chunk[l], 0, len[l]);   /* 0: GX_NOP */
		at = 0;
		for (i = 0; i < nr; ++i) {
			memcpy(chunk[l] + at, runs[i][l], runs_len[i][l]);
			at += runs_len[i][l];
		}
#ifndef MII_NO_OGC
		DCFlushRange(chunk[l], len[l]);
#endif
		body->lod[l].dl[k] = chunk[l];
		body->lod[l].dl_size[k] = len[l];
	}
	for (i = 0; i < nr; ++i)
		for (l = 0; l < n; ++l)
			free(runs[i][l]);
	return r < 0 ? r : verts;
}

int mii_body_make_lods(mii_body *body, const float *tolerances, int n) {
	/* Half a pixel at a Mii 150, 90, 50 and 25 pixels tall (about 170
	 * units), as the head's; mii_body_lod_for draws each level smaller than
	 * that (from 90, 39, 30.5 and 25), so each is a little finer than it
	 * need be. */
	static const float half_pixel[MII_BODY_LODS] = { 0.57f, 0.94f, 1.7f, 3.4f };
	float rot[MII_BODY_BONES][3][3], at[MII_BODY_BONES][3], *world;
	int i, k, vbase = 0, r = 0;

	if (!body || !body->nobjs || n < 0 || n > MII_BODY_LODS)
		return MII_E_ARG;
	if (!tolerances) {
		tolerances = half_pixel;
		if (!n)
			n = MII_BODY_LODS;
	}
	for (i = 0; i < n; ++i)
		if (!(tolerances[i] > 0) || (i && tolerances[i] < tolerances[i - 1]))
			return MII_E_ARG;
	for (i = 0; i < body->nlods; ++i)
		for (k = 0; k < MII_BODY_OBJS; ++k)
			free(body->lod[i].dl[k]);
	memset(body->lod, 0, sizeof(body->lod));
	body->nlods = 0;
	/* The bind pose in the body's space: where the folds are measured. */
	for (i = 0; i < body->nbones; ++i) {
		const mii_bone *bn = &body->bone[i];
		float r3[3], t[3], sc[3];
		m33 local;

		bone_local(body, NULL, 0, i, r3, t, sc);
		rot_xyz(local, r3);
		if (bn->parent < 0) {
			memcpy(rot[i], local, sizeof(local));
			memcpy(at[i], t, sizeof(t));
		} else {
			mul33(rot[i], rot[bn->parent], local);
			for (k = 0; k < 3; ++k)
				at[i][k] = at[bn->parent][k] + rot[bn->parent][k][0] * t[0] + rot[bn->parent][k][1] * t[1] +
						   rot[bn->parent][k][2] * t[2];
		}
	}
	world = malloc(sizeof(float) * 3 * (body->nverts ? body->nverts : 1));
	if (!world)
		return MII_E_MEM;
	for (i = 0; i < body->nverts; ++i) {
		int b = body->vbone[i];
		const float *v = body->vpos + 3 * i;

		for (k = 0; k < 3; ++k)
			world[3 * i + k] = at[b][k] + rot[b][k][0] * v[0] + rot[b][k][1] * v[1] + rot[b][k][2] * v[2];
	}
	for (k = 0; k < body->nobjs && r >= 0; ++k) {   /* each object's skin vertices follow the last's */
		r = lod_object(body, k, world + 3 * vbase, tolerances, n);
		vbase += r > 0 ? r : 0;
	}
	free(world);
	if (r < 0) {
		for (i = 0; i < n; ++i)
			for (k = 0; k < MII_BODY_OBJS; ++k)
				free(body->lod[i].dl[k]);
		memset(body->lod, 0, sizeof(body->lod));
		return r;
	}
	body->nlods = n;
	return MII_OK;
}

int mii_body_lod_for(const mii_body *body, float pixels_tall) {
	/* Each level's height and under: level 1 from 90 px; 2 and 3 halfway,
	 * in distance, from the last to 25 (39 and 30.5); 4 from 25. */
	static const float from[MII_BODY_LODS] = { 90, 39.1f, 30.5f, 25 };
	int l = 0;

	while (body && l < body->nlods && pixels_tall < from[l])
		++l;
	return l;
}
