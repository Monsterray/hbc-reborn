// Host driver for sdk/mboard/mboard.c's cores (tests/test_mboard.py):
//   mboard_host play IMAGE NAME ID START END NOW [WIIID-HEX]
//     Times are local, YYYY-MM-DDTHH:MM:SS. Like tests/cdblog_host.
//   mboard_host post CTL MBX WII-NUMBER DOMAIN APPID-HEX GROUP-HEX UTC LOCAL TITLE BODY
//     UTC and LOCAL as above; TITLE and BODY as hex UTF-8.
//     CTL is written back only when the post succeeds, as mboard_post() does.
// Prints the result code.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mboard.h"

static int file_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
	FILE *f = ctx;
	return fseek(f, (long) off, SEEK_SET) || fread(buf, 1, len, f) != len;
}

static int file_write(void *ctx, uint32_t off, const void *buf, uint32_t len) {
	FILE *f = ctx;
	return fseek(f, (long) off, SEEK_SET) || fwrite(buf, 1, len, f) != len;
}

static int64_t days_from_civil(int64_t y, int m, int d) {
	int64_t era, yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static int64_t secs_2000(const char *s) {
	int y, mo, d, h, mi, sec = 0;

	if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 5) {
		fprintf(stderr, "bad time %s\n", s);
		exit(2);
	}
	return (days_from_civil(y, mo, d) - 10957) * 86400 + h * 3600 + mi * 60 + sec;
}

static uint64_t ticks(const char *s) {
	return (uint64_t) secs_2000(s) * MBOARD_TICKS_PER_SEC;
}

// TITLE and BODY come as hex UTF-8: Windows hands argv over in its own code page.
static void unhex(char *s) {
	char *o = s;
	unsigned v;

	for (; s[0] && s[1] && sscanf(s, "%2x", &v) == 1; s += 2)
		*o++ = (char) v;
	*o = 0;
}

static int play(int argc, char **argv) {
	mboard_session s;
	mboard_io io;
	uint8_t id[8];
	FILE *f;
	size_t i;
	int res;

	if (argc < 8) {
		fprintf(stderr, "usage: mboard_host play IMAGE NAME ID START END NOW [WIIID-HEX]\n");
		return 2;
	}
	memset(&s, 0, sizeof(s));
	for (i = 0; argv[3][i] && i < 40; ++i)
		s.name[i] = (uint8_t) argv[3][i];
	// The ID is NUL-padded, not NUL-terminated: up to all 6 bytes are used.
	memcpy(s.id, argv[4], strlen(argv[4]) < sizeof(s.id) ? strlen(argv[4]) : sizeof(s.id));
	s.start = ticks(argv[5]);
	s.end = ticks(argv[6]);
	if (argc > 8)
		for (i = 0; i < 8; ++i)
			sscanf(argv[8] + 2 * i, "%2hhx", &id[i]);
	f = fopen(argv[2], "r+b");
	if (!f) {
		perror(argv[2]);
		return 2;
	}
	io.ctx = f;
	io.read = file_read;
	io.write = file_write;
	res = mboard_play_vff(&io, &s, ticks(argv[7]), argc > 8 ? id : NULL);
	fclose(f);
	printf("%d\n", res);
	return 0;
}

static int post(int argc, char **argv) {
	static uint8_t ctl[MBOARD_RECV_CTL_SIZE];
	static uint16_t title[MBOARD_TITLE_MAX], body[MBOARD_BODY_MAX];
	mboard_env env;
	mboard_io io;
	FILE *fc, *fm;
	int res, nt, nb;

	if (argc < 12) {
		fprintf(stderr, "usage: mboard_host post CTL MBX WII-NUMBER DOMAIN APPID GROUP UTC LOCAL "
						"TITLE BODY\n");
		return 2;
	}
	fc = fopen(argv[2], "r+b");
	fm = fopen(argv[3], "r+b");
	if (!fc || !fm || fread(ctl, 1, sizeof(ctl), fc) != sizeof(ctl)) {
		fprintf(stderr, "cannot read %s or %s\n", argv[2], argv[3]);
		return 2;
	}
	memset(&env, 0, sizeof(env));
	env.wii_number = strtoull(argv[4], NULL, 10);
	env.domain = argv[5];
	env.app_id = (uint32_t) strtoul(argv[6], NULL, 16);
	env.group_id = (uint16_t) strtoul(argv[7], NULL, 16);
	env.utc = secs_2000(argv[8]) + 946684800;
	env.local = ticks(argv[9]);
	unhex(argv[10]);
	unhex(argv[11]);
	nt = mboard_utf16(title, MBOARD_TITLE_MAX, argv[10]);
	nb = mboard_utf16(body, MBOARD_BODY_MAX, argv[11]);
	io.ctx = fm;
	io.read = file_read;
	io.write = file_write;
	res = mboard_post_box(ctl, &io, &env, title, nt, body, nb);
	if (res == MBOARD_OK && (fseek(fc, 0, SEEK_SET) || fwrite(ctl, 1, sizeof(ctl), fc) != sizeof(ctl)))
		res = MBOARD_E_IO;
	fclose(fc);
	fclose(fm);
	printf("%d\n", res);
	return 0;
}

int main(int argc, char **argv) {
	if (argc > 1 && !strcmp(argv[1], "play"))
		return play(argc, argv);
	if (argc > 1 && !strcmp(argv[1], "post"))
		return post(argc, argv);
	fprintf(stderr, "usage: mboard_host play|post ...\n");
	return 2;
}
