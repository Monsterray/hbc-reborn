/* sdk/mii's drawing (miidraw.c) on a PC, built with -DMII_NO_OGC, for
 * tests/test_miidraw.py and for looking at a Mii without a Wii:
 *
 *   miidraw_host RES [DB SLOT] OUTDIR
 *
 * builds the Mii in RFL_DB.dat's SLOT (or a default Mii) from RFL_Res.dat
 * RES and writes OUTDIR/mask_N.rgba (each expression's face, raw RGBA with
 * a "W H WRAP_S WRAP_T\n" header), OUTDIR/tex_N.rgba, OUTDIR/mii.obj (every part, placed
 * standing as mii_draw places it, without a pose) and prints a summary.
 * Exits 0, or 1 with the error. RES and DB are the user's own files. */

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
		o[1] = 3.2f * sy;
		o[2] = 2.5f;
		break;
	case MII_BODY_ARM:
	case MII_BODY_HAND:
		x = side ? -md->shoulder[0] : md->shoulder[0];
		o[0] = x;
		o[1] = md->shoulder[1] - (part == MII_BODY_HAND ? 28.0f * sy : 0);
		s[0] = s[1] = s[2] = sy;
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

	if (argc != 3 && argc != 5) {
		fprintf(stderr, "usage: miidraw_host RES [DB SLOT] OUTDIR\n");
		return 2;
	}
	if ((r = mii_res_load(&res, argv[1])) < 0) {
		printf("res: %s\n", mii_strerror(r));
		return 1;
	}
	printf("res: version %#x, %u bytes\n", (unsigned) res.version, (unsigned) res.size);
	if (argc == 5) {
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
	for (i = 0; i < MII_TEX_COUNT; ++i)
		write_tex(dir, "tex", i, &md.tex[i]);
	snprintf(path, sizeof(path), "%s/mii.obj", dir);
	obj = fopen(path, "w");
	if (!obj) {
		printf("cannot write %s\n", path);
		return 1;
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
