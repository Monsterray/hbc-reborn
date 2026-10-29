/* sdk/mii's effects (miifx.c) on a PC, built with -DMII_NO_OGC, for
 * tests/test_miifx.py:
 *
 *   miifx_host BREFF BREFT [EMITTER FRAMES [SEED]]
 *
 * loads the two files (BREFT "-": none) and prints each emitter and
 * texture; with EMITTER (a name), plays it for FRAMES frames and prints
 * every live particle after each. MII_FX_AT (12 numbers, a 3 x 4 matrix
 * row by row) places it; MII_FX_STOP=N stops it after frame N. Exits 0, or
 * 1 with the error. The files are the user's own (effect_nigaoe.breff and
 * .breft from the Mii Channel), or the test's. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mii.h"
#include "miidraw.h"

int main(int argc, char **argv) {
	static mii_fx fx;
	static mii_fx_state s;
	static mii_fx_particle out[MII_FX_PARTICLES];
	float at[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
	int r, i, j, e, frames, h, n, stop = -1;

	if (argc != 3 && argc != 5 && argc != 6) {
		fprintf(stderr, "usage: miifx_host BREFF BREFT [EMITTER FRAMES [SEED]]\n");
		return 2;
	}
	if (!strcmp(argv[2], "-")) {
		FILE *f = fopen(argv[1], "rb");
		static unsigned char data[0x1000000];
		size_t size;

		if (!f) {
			printf("load: no file\n");
			return 1;
		}
		size = fread(data, 1, sizeof(data), f);
		fclose(f);
		r = mii_fx_attach(&fx, data, size, NULL, 0);
	} else {
		r = mii_fx_load(&fx, argv[1], argv[2]);
	}
	if (r < 0) {
		printf("load: %s\n", mii_strerror(r));
		return 1;
	}
	printf("emitters %d curves %d keys %d ranges %d\n", fx.nemitters, fx.ncurves, fx.nkeys, fx.nranges);
	for (i = 0; i < fx.nemitters; ++i) {
		const mii_fx_emitter *em = &fx.emitter[i];

		printf("emitter %d %s form %u tracks %d init %d emit_tracks %d tex %d life %u\n", i, em->name,
			   (unsigned) (em->emit_flag & 0xFF), em->ntracks, em->ninit, em->nemit_tracks, em->tex,
			   (unsigned) em->ptcl_life);
	}
	for (i = 0; i < fx.ntex; ++i)
		printf("texture %d %s %ux%u fmt %u bytes %u\n", i, fx.tex[i].name, (unsigned) fx.tex[i].w,
			   (unsigned) fx.tex[i].h, (unsigned) fx.tex[i].fmt, (unsigned) fx.tex[i].size);
	if (argc == 3) {
		mii_fx_free(&fx);
		return 0;
	}
	e = mii_fx_find(&fx, argv[3]);
	frames = atoi(argv[4]);
	if (e < 0) {
		printf("no emitter %s\n", argv[3]);
		return 1;
	}
	if (getenv("MII_FX_AT")) {
		const char *p = getenv("MII_FX_AT");
		char *q;

		for (i = 0; i < 12; ++i, p = q)
			at[i / 4][i % 4] = strtof(p, &q);
	}
	if (getenv("MII_FX_STOP"))
		stop = atoi(getenv("MII_FX_STOP"));
	mii_fx_init(&s, &fx, argc == 6 ? (uint32_t) strtoul(argv[5], NULL, 0) : 1);
	h = mii_fx_play(&s, e, (const float (*)[4]) at);
	printf("play %s %d\n", argv[3], h);
	for (i = 0; i < frames; ++i) {
		mii_fx_step(&s, 1);
		n = mii_fx_particles(&s, out, MII_FX_PARTICLES);
		printf("frame %d live %d playing %d\n", i, n, mii_fx_playing(&s, h));
		for (j = 0; j < n && j < MII_FX_PARTICLES; ++j) {
			const mii_fx_particle *p = &out[j];

			printf("p %d age %d life %d pos %.4f %.4f %.4f size %.4f %.4f rot %.5f col %u %u %u %u sec %u %u %u"
				   " tex %d hidden %d\n", p->emitter, p->age, p->life, p->pos[0], p->pos[1], p->pos[2], p->size[0],
				   p->size[1], p->rot, p->color[0], p->color[1], p->color[2], p->color[3], p->color2[0],
				   p->color2[1], p->color2[2], p->tex, p->hidden);
		}
		if (i == stop)
			mii_fx_stop(&s, h);
	}
	mii_fx_free(&fx);
	return 0;
}
