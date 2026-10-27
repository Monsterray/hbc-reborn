/* sdk/mii's drawing (miidraw.c) on a PC, built with -DMII_NO_OGC, for
 * tests/test_miidraw.py and for looking at a Mii without a Wii:
 *
 *   miidraw_host RES [DB SLOT | -d SEX HEIGHT BUILD FAV] OUTDIR
 *
 * builds the Mii in RFL_DB.dat's SLOT, or a default Mii (-d: of that sex,
 * height, build and favourite colour; none: a default man), from RFL_Res.dat
 * RES and writes OUTDIR/mask_N.rgba (each expression's face, raw RGBA with
 * a "W H WRAP_S WRAP_T\n" header), OUTDIR/tex_N.rgba, OUTDIR/mii.obj (every part, placed
 * standing as mii_draw places it, without a pose) and prints a summary.
 * Exits 0, or 1 with the error. RES and DB are the user's own files.
 *
 * With MII_BODY set to a Mii Channel body (maleBody.brres, femaleBody.brres:
 * the user's own), the OBJ has that body instead, posed with MII_ANIM
 * (bodyWait, bodyGreeting, ...) at MII_FRAME, the head on its head bone; it
 * prints the bones, meshes and animations it read. */

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mii.h"
#include "miidraw.h"

static int write_tex(const char *dir, const char *name, int n, const mii_tex *t) {
	char path[512];
	FILE *f;

	if (!t->rgba)
		return 0;
	snprintf(path, sizeof(path), "%s/%s_%d.rgba", dir, name, n);
	f = fopen(path, "wb");
	if (!f)
		return -1;
	fprintf(f, "%d %d %d %d\n", t->w, t->h, t->wrap_s, t->wrap_t);
	fwrite(t->rgba, 4, (size_t) t->w * t->h, f);
	fclose(f);
	return 0;
}

static const char *part_names[MII_PARTS] = {
	"beard", "nose", "forehead", "hair", "cap", "faceline", "mask", "noseline", "glass",
	"torso", "hips", "leg", "shoe", "arm", "hand", "neck"
};

/* Places body parts as mii_draw does for a still pose (no rotation). */
static void place(const mii_model *md, int part, int side, float o[3], float s[3]) {
	float sx = md->body_scale[0], sy = md->body_scale[1], x;

	o[0] = o[1] = o[2] = 0;
	s[0] = s[2] = sx;
	s[1] = sy;
	switch (part) {
	case MII_BODY_LEG:
		o[0] = side ? -md->hip[0] : md->hip[0];
		o[1] = md->hip[1];
		break;
	case MII_BODY_SHOE:
		o[0] = side ? -md->hip[0] : md->hip[0];
		o[1] = md->shoe[0] * sy;
		o[2] = md->shoe[1];
		break;
	case MII_BODY_ARM:
	case MII_BODY_HAND:
		x = side ? -md->shoulder[0] : md->shoulder[0];
		o[0] = x;
		o[1] = md->shoulder[1] - (part == MII_BODY_HAND ? md->arm_len * sy : 0);
		if (part == MII_BODY_HAND)
			s[0] = s[1] = s[2] = (sx + sy) / 2;
		break;
	case MII_BODY_NECK:
		o[1] = md->head_y - 2;
		s[0] = s[1] = s[2] = 1;
		break;
	default:
		if (part < MII_HEAD_PARTS) {
			o[1] = md->head_y;
			s[0] = s[1] = s[2] = 1;
		}
	}
}

int main(int argc, char **argv) {
	const char *dir = argv[argc - 1];
	mii_res res;
	mii_model md;
	mii m;
	char path[512];
	FILE *obj;
	int r, i, e, side, base = 1;

	if (argc != 3 && argc != 5 && !(argc == 8 && !strcmp(argv[2], "-d"))) {
		fprintf(stderr, "usage: miidraw_host RES [DB SLOT | -d SEX HEIGHT BUILD FAV] OUTDIR\n");
		return 2;
	}
	if ((r = mii_res_load(&res, argv[1])) < 0) {
		printf("res: %s\n", mii_strerror(r));
		return 1;
	}
	printf("res: version %#x, %u bytes\n", (unsigned) res.version, (unsigned) res.size);
	if (argc == 8) {   /* a default Mii: sex (0 male, 1 female), height, build, favourite colour */
		mii_default(&m, atoi(argv[3]));
		m.height = (uint8_t) atoi(argv[4]);
		m.build = (uint8_t) atoi(argv[5]);
		m.fav_color = (uint8_t) atoi(argv[6]);
	} else if (argc == 5) {
		FILE *f = fopen(argv[2], "rb");
		static uint8_t buf[MII_DB_FILE_SIZE];
		mii_db db;
		size_t n;

		if (!f) {
			printf("db: cannot open\n");
			return 1;
		}
		n = fread(buf, 1, sizeof(buf), f);
		fclose(f);
		if ((r = mii_db_attach(&db, buf, n)) < 0 || (r = mii_db_get(&db, atoi(argv[3]), &m)) < 0) {
			printf("db: %s\n", mii_strerror(r));
			return 1;
		}
	} else {
		mii_default(&m, MII_MALE);
	}
	if ((r = mii_model_make(&res, &m, 128, &md)) < 0) {
		printf("model: %s\n", mii_strerror(r));
		return 1;
	}
	for (e = 0; e < MII_EXPR_COUNT; ++e)
		write_tex(dir, "mask", e, &md.mask[e]);
	{   /* The packed meshes give back the floats; RGB5A3 the texels, to its bits. */
		float pe = 0, ne = 0, se = 0;
		long corners = 0, verts = 0;
		int te = 0, ae = 0;

		for (i = 0; i < MII_PARTS; ++i) {
			const mii_mesh *p = &md.part[i];
			const int16_t *pos = (const int16_t *) p->pack, *st = (const int16_t *) (p->pack + p->pack_st);
			const int8_t *nrm = (const int8_t *) (p->pack + p->pack_nrm);
			int k, j;

			corners += p->count;
			verts += p->nverts;
			for (k = 0; k < p->count; ++k) {
				const float *v = p->v + 8 * k;
				int x = p->idx[k];

				for (j = 0; j < 3; ++j) {
					float d = fabsf(pos[3 * x + j] / (float) (1 << p->pos_frac) - v[j]);
					float n = fabsf(nrm[3 * x + j] / 64.0f - v[3 + j]);

					pe = d > pe ? d : pe;
					ne = n > ne ? n : ne;
				}
				for (j = 0; j < 2; ++j) {
					float d = fabsf(st[2 * x + j] / (float) (1 << p->st_frac) - v[6 + j]);

					se = d > se ? d : se;
				}
			}
		}
		for (i = 0; i < MII_TEX_COUNT + MII_EXPR_COUNT; ++i) {
			const mii_tex *t = i < MII_TEX_COUNT ? &md.tex[i] : &md.mask[i - MII_TEX_COUNT];
			uint8_t *g;
			int x, y;

			if (!t->rgba)
				continue;
			g = malloc(mii_tex_rgb5a3_size(t->w, t->h));
			mii_tex_rgb5a3(t, g);
			for (y = 0; y < t->h; ++y)
				for (x = 0; x < t->w; ++x) {
					const uint8_t *q = g + 32 * ((y / 4) * ((t->w + 3) / 4) + x / 4) + 2 * (4 * (y % 4) + x % 4);
					const uint8_t *p = t->rgba + 4 * (y * t->w + x);
					int v = q[0] << 8 | q[1], c[4], k;

					if (v & 0x8000) {
						c[0] = (v >> 10 & 31) * 255 / 31;
						c[1] = (v >> 5 & 31) * 255 / 31;
						c[2] = (v & 31) * 255 / 31;
						c[3] = 255;
					} else {
						c[0] = (v >> 8 & 15) * 17;
						c[1] = (v >> 4 & 15) * 17;
						c[2] = (v & 15) * 17;
						c[3] = (v >> 12 & 7) * 255 / 7;
					}
					for (k = 0; k < 3; ++k)
						if (p[3] && abs(c[k] - p[k]) > te)
							te = abs(c[k] - p[k]);   /* colour where there is any */
					if (abs(c[3] - p[3]) > ae)
						ae = abs(c[3] - p[3]);
				}
			free(g);
		}
		printf("pack: %ld corners, %ld vertices; largest error: position %.4f, normal %.4f, "
			   "coordinate %.5f; rgb5a3 colour %d, alpha %d\n", corners, verts, pe, ne, se, te, ae);
	}
	{   /* mii_model_opts: only the expressions asked for (and the normal one), at 32 px */
		mii_model_opts o = { 32, MII_EXPR_BIT(MII_EXPR_BLINK), NULL };
		mii_model few;
		unsigned built = 0;

		if ((r = mii_model_build(&res, &m, &o, &few)) < 0) {
			printf("model (opts): %s\n", mii_strerror(r));
			return 1;
		}
		for (e = 0; e < MII_EXPR_COUNT; ++e)
			if (few.mask[e].rgba)
				built |= 1u << e;
		printf("opts: expressions %#x, %d px\n", built, few.mask[MII_EXPR_NORMAL].w);
		mii_model_free(&few);
	}
	{   /* A cache: two of the same Mii share every part; the last one out frees them.
		 * mii_model_set_mask_size: the faces rebuilt at another size, still shared. */
		mii_model_opts o = { 128, MII_EXPR_BIT(MII_EXPR_SMILE), NULL };
		mii_model a, b;
		int same = 0, parts = 0, entries[4], users[4];
		uint32_t bytes[4];

		o.cache = mii_cache_new();
		if (!o.cache || mii_model_build(&res, &m, &o, &a) < 0 || mii_model_build(&res, &m, &o, &b) < 0) {
			printf("cache: did not build\n");
			return 1;
		}
		for (i = 0; i < MII_PARTS; ++i)
			if (a.part[i].count) {
				++parts;
				same += a.part[i].pack == b.part[i].pack && a.part[i].idx == b.part[i].idx;
			}
		same += a.mask[MII_EXPR_SMILE].rgba == b.mask[MII_EXPR_SMILE].rgba;
		mii_cache_stats(o.cache, &entries[0], &bytes[0], &users[0]);
		r = mii_model_set_mask_size(&a, &res, 32);
		mii_cache_stats(o.cache, &entries[1], &bytes[1], &users[1]);
		if (r < 0 || mii_model_set_mask_size(&b, &res, 32) < 0) {
			printf("cache: set_mask_size %d\n", r);
			return 1;
		}
		mii_cache_stats(o.cache, &entries[2], &bytes[2], &users[2]);
		mii_model_free(&a);
		mii_model_free(&b);
		mii_cache_stats(o.cache, &entries[3], &bytes[3], &users[3]);
		mii_cache_free(o.cache);
		printf("cache: %d of %d parts and the face shared; entries %d %d %d %d, users %d %d %d %d; "
			   "bytes %u then %u at 32 px; faces %d px\n", same - 1, parts, entries[0], entries[1],
			   entries[2], entries[3], users[0], users[1], users[2], users[3], (unsigned) bytes[0],
			   (unsigned) bytes[2], 32);
	}
	for (i = 0; i < MII_TEX_COUNT; ++i)
		write_tex(dir, "tex", i, &md.tex[i]);
	snprintf(path, sizeof(path), "%s/mii.obj", dir);
	obj = fopen(path, "w");
	if (!obj) {
		printf("cannot write %s\n", path);
		return 1;
	}
	if (getenv("MII_BODY")) {
		static mii_body body;
		mii_pose pose;
		float rot[MII_BODY_BONES][3][3], pos[MII_BODY_BONES][3], hm[3][4];
		const float (*b)[3];
		int a, j, k;

		if ((r = mii_body_load(&body, getenv("MII_BODY"))) < 0) {
			printf("body: %s\n", mii_strerror(r));
			return 1;
		}
		printf("body: %d bones, %d skin vertices, %d + %d triangles, %d animations\n", body.nbones,
			   body.nverts, body.ntris[0], body.ntris[1], body.nanims);
		for (a = 0; a < body.nanims; ++a)
			printf("anim %s: %d frames\n", body.anim[a].name, body.anim[a].frames);
		if ((r = mii_model_set_body(&md, &body)) < 0) {
			printf("set_body: %s\n", mii_strerror(r));
			return 1;
		}
		{   /* The body's pose memo: a frame posed again is found, and is the same. */
			static float r1[MII_BODY_BONES][3][3], p1[MII_BODY_BONES][3];
			int walk = 1 + mii_body_anim(&body, "bodyWalk"), f;

			memset(&pose, 0, sizeof(pose));
			pose.anim = walk;
			pose.frame = 10.5f;
			mii_body_pose(&md, &pose, r1, p1);
			for (f = 0; f < MII_POSE_MEMO - 1; ++f) {   /* fills all but the first */
				pose.frame = 20.0f + f;
				mii_body_pose(&md, &pose, rot, pos);
			}
			body.memo_hits = 0;
			pose.frame = 10.5f;
			mii_body_pose(&md, &pose, rot, pos);
			printf("memo: hits %u, same %d\n", (unsigned) body.memo_hits,
				   !memcmp(rot, r1, sizeof(float) * 9 * body.nbones) && !memcmp(pos, p1, sizeof(float) * 3 * body.nbones));
		}
		if (getenv("MII_FADE")) {
			/* Cross-fades (mii_pose.from_*): a pose faded into itself is
			 * unchanged (the quaternion round trip), weight 1 is the
			 * animation before alone, and a fade's steps are small. */
			static float ref[2048 * 3], prev[2048 * 3];
			int wait = 1 + mii_body_anim(&body, "bodyWait"), greet = 1 + mii_body_anim(&body, "bodyGreeting");
			float self = 0, whole = 0, step = 0;
			int f, s, n = 3 * body.nverts < 2048 * 3 ? 3 * body.nverts : 2048 * 3;

			for (f = 0; f < 200; f += 7) {
				memset(&pose, 0, sizeof(pose));
				pose.anim = greet;
				pose.frame = f;
				mii_body_skin(&md, &pose);
				memcpy(ref, md.skin_pos, sizeof(float) * n);
				pose.from_anim = greet;
				pose.from_frame = f;
				pose.from_weight = 0.5f;
				mii_body_skin(&md, &pose);
				for (i = 0; i < n; ++i)
					if (fabsf(md.skin_pos[i] - ref[i]) > self)
						self = fabsf(md.skin_pos[i] - ref[i]);
				memset(&pose, 0, sizeof(pose));
				pose.anim = wait;
				pose.frame = 3 * f;
				mii_body_skin(&md, &pose);
				memcpy(ref, md.skin_pos, sizeof(float) * n);
				pose.anim = greet;
				pose.frame = f;
				pose.from_anim = wait;
				pose.from_frame = 3 * f;
				pose.from_weight = 1;
				mii_body_skin(&md, &pose);
				for (i = 0; i < n; ++i)
					if (fabsf(md.skin_pos[i] - ref[i]) > whole)
						whole = fabsf(md.skin_pos[i] - ref[i]);
				/* The fade itself, in 60 steps of the weight. */
				for (s = 0; s <= 60; ++s) {
					pose.from_weight = mii_fade_weight(s / 240.0f, 0.25f);
					mii_body_skin(&md, &pose);
					for (i = 0; s && i < n; ++i)
						if (fabsf(md.skin_pos[i] - prev[i]) > step)
							step = fabsf(md.skin_pos[i] - prev[i]);
					memcpy(prev, md.skin_pos, sizeof(float) * n);
				}
			}
			printf("fade: self %.5f, whole %.5f, largest step %.3f\n", self, whole, step);
			{
				/* HBC's way out of the wave, at 60 frames a second: Wait
				 * fading in over 0.4 s from Greeting's last frame, held or
				 * played on (and wrapping), against Wait's own motion. */
				int last = body.anim[greet - 1].frames - 1, mode, w0;
				float most[3] = { 0, 0, 0 };

				for (w0 = 0; w0 < 120; w0 += 10)
					for (mode = 0; mode < 3; ++mode)
						for (s = 0; s <= 30; ++s) {
							memset(&pose, 0, sizeof(pose));
							pose.anim = wait;
							pose.frame = (float) (w0 + s);
							if (mode) {
								pose.from_anim = greet;
								pose.from_frame = mode == 1 ? (float) last : (float) (last + s);
								pose.from_weight = mii_fade_weight(s / 60.0f, 0.4f);
							}
							mii_body_skin(&md, &pose);
							for (i = 0; s && i < n; ++i)
								if (fabsf(md.skin_pos[i] - prev[i]) > most[mode])
									most[mode] = fabsf(md.skin_pos[i] - prev[i]);
							memcpy(prev, md.skin_pos, sizeof(float) * n);
						}
				printf("out of the wave: largest frame step: wait alone %.3f, held %.3f, played on %.3f\n",
					   most[0], most[1], most[2]);
			}
			return 0;
		}
		if (getenv("MII_PALETTE")) {
			/* The GPU's way (sdk/mii 2.0): each vertex through its bone's
			 * palette matrices, against the CPU reference, mii_body_skin.
			 * Positions must match; normals point the same way (the GPU's
			 * leave out the scale's inverse). And every primitive of every
			 * object's list rewritten to GX_VTXFMT1. */
			static float pm[MII_BODY_BONES][3][4], nm[MII_BODY_BONES][3][3];
			float worst = 0, least = 1;
			long poses = 0, prims = 0, fmt1 = 0;
			int f, o;

			for (a = 0; a <= body.nanims; ++a)
				for (f = 0; f < 600; ++f) {
					memset(&pose, 0, sizeof(pose));
					pose.anim = a;
					pose.frame = f * 0.37f;
					mii_body_skin(&md, &pose);
					mii_body_matrices(&md, &pose, pm, nm);
					++poses;
					for (i = 0; i < body.nverts; ++i) {
						int bn = body.vbone[i];
						const float *p = body.vpos + 3 * i, *n = body.vnrm + 3 * i;
						float q[3], r[3], l = 0, d = 0;

						for (k = 0; k < 3; ++k) {
							q[k] = pm[bn][k][0] * p[0] + pm[bn][k][1] * p[1] + pm[bn][k][2] * p[2] +
								   pm[bn][k][3];
							r[k] = nm[bn][k][0] * n[0] + nm[bn][k][1] * n[1] + nm[bn][k][2] * n[2];
							if (fabsf(q[k] - md.skin_pos[3 * i + k]) > worst)
								worst = fabsf(q[k] - md.skin_pos[3 * i + k]);
							l += r[k] * r[k];
						}
						l = l > 0 ? 1 / sqrtf(l) : 0;
						for (k = 0; k < 3; ++k)
							d += r[k] * l * md.skin_nrm[3 * i + k];
						if (d < least)
							least = d;
					}
				}
			for (o = 0; o < body.nobjs; ++o) {
				const uint8_t *d = body.obj[o].dl;
				uint32_t at = 0, n = body.obj[o].dl_size;
				int tex = 0;

				for (k = 1; k <= 8; ++k)
					tex += body.obj[o].vcd >> k & 1;
				while (at < n) {
					uint8_t op = d[at];

					if (op == 0) {
						++at;
					} else if (op == 0x20 || op == 0x28 || op == 0x30) {
						at += 5;
					} else {
						int cnt = d[at + 1] << 8 | d[at + 2];
						int vsz = 1 + tex + ((body.obj[o].vcd >> 9 & 3) == 2 ? 1 : 2) +
								  ((body.obj[o].vcd >> 11 & 3) == 2 ? 1 : 2);

						++prims;
						fmt1 += (op & 7) == 1;
						at += 3 + (uint32_t) cnt * vsz;
					}
				}
			}
			printf("palette: %ld poses, objects %d, matrices %d, worst position %.5f, least normal dot %.4f, "
				   "primitives %ld, on VTXFMT1 %ld\n", poses, body.nobjs, body.nmatrices, worst, least, prims, fmt1);
			return 0;
		}
		if (getenv("MII_SWEEP")) {
			/* Every animation at many fractional frames, and the avatar's
			 * large ones: every posed vertex finite and near the body. */
			long poses = 0, bad = 0, sub = 0;
			int f;
			float big = 0, nbig = 0, wf = 0;
			int wa = 0, wv = 0;

			for (a = 0; a <= body.nanims; ++a)
				for (f = 0; f < 3000; ++f) {
					if (getenv("MII_ANIM") && a != 1 + mii_body_anim(&body, getenv("MII_ANIM")))
						break;
					memset(&pose, 0, sizeof(pose));
					pose.anim = a;
					pose.frame = f < 2000 ? f * 0.137f : 1e3f * (float) (f - 2000) + 0.3f;
					mii_body_skin(&md, &pose);
					++poses;
					for (i = 0; i < 3 * body.nverts; ++i) {
						float p = fabsf(md.skin_pos[i]), n = fabsf(md.skin_nrm[i]);

						if (!isfinite(p) || !isfinite(n))
							++bad;
						else {
							if ((p != 0 && p < FLT_MIN) || (n != 0 && n < FLT_MIN))
								++sub;
							if (p > big) {
								big = p;
								wa = a;
								wf = pose.frame;
								wv = i / 3;
							}
							if (n > nbig)
								nbig = n;
						}
					}
				}
			printf("sweep: %ld poses, %ld non-finite, largest position %.1f, normal %.3f\n", poses,
				   bad, big, nbig);
			printf("subnormal: %ld\n", sub);
			printf("largest: %s frame %.3f vertex %d\n", wa ? body.anim[wa - 1].name : "bind", wf, wv);
			return 0;
		}
		memset(&pose, 0, sizeof(pose));
		pose.anim = getenv("MII_ANIM") ? 1 + mii_body_anim(&body, getenv("MII_ANIM")) : 0;
		pose.frame = getenv("MII_FRAME") ? (float) atof(getenv("MII_FRAME")) : 0;
		mii_body_skin(&md, &pose);
		mii_body_pose(&md, &pose, rot, pos);
		b = body.bone[body.head].bind_rot;
		for (j = 0; j < 3; ++j) {
			for (k = 0; k < 3; ++k)
				hm[j][k] = rot[body.head][j][0] * b[k][0] + rot[body.head][j][1] * b[k][1] +
						   rot[body.head][j][2] * b[k][2];
			hm[j][3] = pos[body.head][j];
		}
		printf("head bone at %.2f %.2f %.2f, ground %.2f\n", pos[body.head][0], pos[body.head][1],
			   pos[body.head][2], md.ground);
		for (i = 0; i < MII_HEAD_PARTS; ++i) {
			const mii_mesh *p = &md.part[i];

			if (!p->count)
				continue;
			fprintf(obj, "o %s\nusemtl c%08x_t%d\n", part_names[i], (unsigned) p->color, p->tex);
			for (k = 0; k < p->count; ++k) {
				const float *v = p->v + 8 * k;
				float w[3], n[3];

				for (j = 0; j < 3; ++j) {
					w[j] = hm[j][0] * v[0] + hm[j][1] * v[1] + hm[j][2] * v[2] + hm[j][3];
					n[j] = hm[j][0] * v[3] + hm[j][1] * v[4] + hm[j][2] * v[5];
				}
				fprintf(obj, "v %.4f %.4f %.4f\nvn %.4f %.4f %.4f\nvt %.5f %.5f\n", w[0], w[1], w[2],
						n[0], n[1], n[2], v[6], v[7]);
			}
			for (k = 0; k < p->count; k += 3)
				fprintf(obj, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", base + k, base + k, base + k,
						base + k + 1, base + k + 1, base + k + 1, base + k + 2, base + k + 2, base + k + 2);
			base += p->count;
		}
		for (i = 0; i < 2; ++i) {
			fprintf(obj, "o body_%s\nusemtl c%08x_t-1\n", i ? "pants" : "shirt",
					(unsigned) (i ? md.pants : md.shirt));
			for (k = 0; k < 3 * body.ntris[i]; ++k) {
				int v = body.tris[i][k];
				const float *q = md.skin_pos + 3 * v, *n = md.skin_nrm + 3 * v;

				fprintf(obj, "v %.4f %.4f %.4f\nvn %.4f %.4f %.4f\nvt 0 0\n", q[0], q[1], q[2], n[0], n[1], n[2]);
			}
			for (k = 0; k < 3 * body.ntris[i]; k += 3)
				fprintf(obj, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", base + k, base + k, base + k,
						base + k + 1, base + k + 1, base + k + 1, base + k + 2, base + k + 2, base + k + 2);
			base += 3 * body.ntris[i];
		}
		fclose(obj);
		mii_model_free(&md);
		mii_body_free(&body);
		mii_res_free(&res);
		printf("ok\n");
		return 0;
	}
	for (i = 0; i < MII_PARTS; ++i) {
		const mii_mesh *p = &md.part[i];
		int sides = i == MII_BODY_LEG || i == MII_BODY_SHOE || i == MII_BODY_ARM || i == MII_BODY_HAND ? 2 : 1;

		printf("part %s: %d vertices, colour %08x, texture %d\n", part_names[i], p->count,
			   (unsigned) p->color, p->tex);
		for (side = 0; side < sides; ++side) {
			float o[3], s[3];
			int k;

			if (!p->count)
				continue;
			place(&md, i, side, o, s);
			fprintf(obj, "o %s%s\nusemtl c%08x_t%d\n", part_names[i], side ? "_left" : "",
					(unsigned) p->color, p->tex);
			for (k = 0; k < p->count; ++k) {
				const float *v = p->v + 8 * k;

				fprintf(obj, "v %.4f %.4f %.4f\nvn %.4f %.4f %.4f\nvt %.5f %.5f\n",
						o[0] + v[0] * s[0], o[1] + v[1] * s[1], o[2] + v[2] * s[2], v[3], v[4], v[5],
						v[6], v[7]);
			}
			for (k = 0; k < p->count; k += 3)
				fprintf(obj, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", base + k, base + k, base + k,
						base + k + 1, base + k + 1, base + k + 1, base + k + 2, base + k + 2, base + k + 2);
			base += p->count;
		}
	}
	fclose(obj);
	printf("body scale %.3f %.3f, head at %.2f\n", md.body_scale[0], md.body_scale[1], md.head_y);
	mii_model_free(&md);
	mii_res_free(&res);
	printf("ok\n");
	return 0;
}
