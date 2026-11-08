/* sdk/slotled's pure parts on a PC, for tests/test_slotled.py.
 *
 *   slotled_host duty LEVEL
 *   slotled_host steps KIND ARGS...          one "level fade ms" line a step
 *   slotled_host at KIND ARGS... -- REPEAT START MS...
 *                                            one "level done" line a time
 *   slotled_host sysconf FILE SET            the old value; FILE rewritten
 *
 * KIND: blink LEVEL ON OFF | pulse LO HI PERIOD | heart LEVEL PERIOD |
 *       morse TEXT LEVEL UNIT | raw LEVEL,FADE,MS...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slotled.h"

static slotled_step steps[SLOTLED_MAX_STEPS];

static int build(int argc, char **argv, int *used)
{
	const char *k = argv[0];
	int chars;

	if (!strcmp(k, "blink") && argc >= 4)
		return *used = 4, slotled_build_blink(steps, SLOTLED_MAX_STEPS, atoi(argv[1]),
		                                       atoi(argv[2]), atoi(argv[3]));
	if (!strcmp(k, "pulse") && argc >= 4)
		return *used = 4, slotled_build_pulse(steps, SLOTLED_MAX_STEPS, atoi(argv[1]),
		                                       atoi(argv[2]), atoi(argv[3]));
	if (!strcmp(k, "heart") && argc >= 3)
		return *used = 3, slotled_build_heartbeat(steps, SLOTLED_MAX_STEPS, atoi(argv[1]),
		                                           atoi(argv[2]));
	if (!strcmp(k, "morse") && argc >= 4) {
		int n = slotled_build_morse(steps, SLOTLED_MAX_STEPS, argv[1], atoi(argv[2]),
		                            atoi(argv[3]), &chars);
		*used = 4;
		printf("chars %d\n", chars);
		return n;
	}
	if (!strcmp(k, "raw")) {
		int n = 0;

		for (*used = 1; *used < argc && strcmp(argv[*used], "--"); (*used)++, n++) {
			unsigned l, f, ms;

			if (sscanf(argv[*used], "%u,%u,%u", &l, &f, &ms) != 3)
				return SLOTLED_E_ARG;
			steps[n].level = (uint8_t)l;
			steps[n].fade = (uint8_t)f;
			steps[n].ms = (uint16_t)ms;
		}
		return n;
	}
	return SLOTLED_E_ARG;
}

int main(int argc, char **argv)
{
	int n, used, i;

	if (argc == 3 && !strcmp(argv[1], "duty")) {
		printf("%u\n", (unsigned)slotled_duty_us(atoi(argv[2])));
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "steps")) {
		n = build(argc - 2, argv + 2, &used);
		if (n < 0) {
			printf("error %d\n", n);
			return 0;
		}
		for (i = 0; i < n; i++)
			printf("%d %d %d\n", steps[i].level, steps[i].fade, steps[i].ms);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "at")) {
		slotled_effect e;
		int start;

		n = build(argc - 2, argv + 2, &used);
		used += 2;
		if (n < 0 || used + 3 > argc || strcmp(argv[used], "--")) {
			printf("error %d\n", n);
			return 1;
		}
		e.steps = steps;
		e.count = (uint32_t)n;
		e.repeat = (uint32_t)atoi(argv[used + 1]);
		start = atoi(argv[used + 2]);
		for (i = used + 3; i < argc; i++) {
			int done, l = slotled_effect_level(&e, strtoull(argv[i], NULL, 10), start, &done);
			printf("%d %d\n", l, done);
		}
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "sysconf")) {
		static uint8_t sc[0x4000];
		FILE *f = fopen(argv[2], "rb");
		size_t size;
		int r;

		if (!f)
			return 1;
		size = fread(sc, 1, sizeof sc, f);
		fclose(f);
		r = slotled_sysconf_illum(sc, size, atoi(argv[3]));
		printf("%d\n", r);
		f = fopen(argv[2], "wb");
		if (!f)
			return 1;
		fwrite(sc, 1, size, f);
		fclose(f);
		return 0;
	}
	fprintf(stderr, "usage: see main.c\n");
	return 2;
}
