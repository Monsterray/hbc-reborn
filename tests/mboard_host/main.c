// Host driver for sdk/mboard/mboard.c's cores (tests/test_mboard.py):
//   mboard_host play IMAGE NAME ID START END NOW [WIIID-HEX]
//     Times are local, YYYY-MM-DDTHH:MM:SS (tests/test_cdblog.py, and HBC's
//     play log, which is this writer).
//   mboard_host post CTL MBX WII-NUMBER DOMAIN APPID-HEX GROUP-HEX UTC LOCAL TITLE BODY [OPT...]
//     UTC and LOCAL as above; TITLE and BODY as hex UTF-8. OPT: face=FILE
//     jpeg=FILE board=FILE reply=1 delay=HOURS date=YYYY-MM-DD (mboard_msg);
//     to=WII-NUMBER: to another Wii, CTL and MBX being the send box.
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

// A whole file, for an mboard_msg part.
static void *slurp(const char *path, uint32_t *size) {
	FILE *f = fopen(path, "rb");
	void *p;
	long n;

	if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		perror(path);
		exit(2);
	}
	p = malloc(n ? (size_t) n : 1);
	if (!p || fread(p, 1, (size_t) n, f) != (size_t) n) {
		perror(path);
		exit(2);
	}
	fclose(f);
	*size = (uint32_t) n;
	return p;
}

static int post(int argc, char **argv) {
	static uint8_t ctl[MBOARD_RECV_CTL_SIZE];
	mboard_env env;
	mboard_msg m;
	mboard_io io;
	FILE *fc, *fm;
	uint32_t size, ctl_size = MBOARD_RECV_CTL_SIZE;
	uint64_t to = 0;
	int res, i, y, mo, d;

	if (argc < 12) {
		fprintf(stderr, "usage: mboard_host post CTL MBX WII-NUMBER DOMAIN APPID GROUP UTC LOCAL "
						"TITLE BODY\n");
		return 2;
	}
	for (i = 12; i < argc; ++i)
		if (!strncmp(argv[i], "to=", 3)) {
			to = strtoull(argv[i] + 3, NULL, 10);
			ctl_size = MBOARD_SEND_CTL_SIZE;
		}
	fc = fopen(argv[2], "r+b");
	fm = fopen(argv[3], "r+b");
	if (!fc || !fm || fread(ctl, 1, ctl_size, fc) != ctl_size) {
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
	memset(&m, 0, sizeof(m));
	m.title = argv[10];
	m.body = argv[11];
	for (i = 12; i < argc; ++i) {
		if (!strncmp(argv[i], "face=", 5)) {
			m.face = slurp(argv[i] + 5, &size);
			if (size != MBOARD_FACE_SIZE) {
				fprintf(stderr, "a face is %d bytes\n", MBOARD_FACE_SIZE);
				return 2;
			}
		} else if (!strncmp(argv[i], "jpeg=", 5)) {
			m.jpeg = slurp(argv[i] + 5, &m.jpeg_size);
		} else if (!strncmp(argv[i], "board=", 6)) {
			m.board = slurp(argv[i] + 6, &m.board_size);
		} else if (!strncmp(argv[i], "reply=", 6)) {
			m.reply = atoi(argv[i] + 6);
		} else if (!strncmp(argv[i], "delay=", 6)) {
			m.delay_hours = atoi(argv[i] + 6);
		} else if (!strncmp(argv[i], "to=", 3)) {
			/* read above */
		} else if (sscanf(argv[i], "date=%d-%d-%d", &y, &mo, &d) == 3) {
			m.file_year = (uint16_t) y;
			m.file_month = (uint8_t) mo;
			m.file_day = (uint8_t) d;
		} else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}
	io.ctx = fm;
	io.read = file_read;
	io.write = file_write;
	res = to ? mboard_send_box(ctl, &io, &env, to, &m) : mboard_post_box(ctl, &io, &env, &m);
	if (res == MBOARD_OK && (fseek(fc, 0, SEEK_SET) || fwrite(ctl, 1, ctl_size, fc) != ctl_size))
		res = MBOARD_E_IO;
	fclose(fc);
	fclose(fm);
	printf("%d\n", res);
	return 0;
}

// The address book: mboard_host friends FILE list | add NUMBER NICK-HEX [MII-ID-HEX]
// | set SLOT NICK-HEX|- [MII-ID-HEX] | remove SLOT. Prints the result, and for
// list one line per contact: slot type confirmed number nick-utf16-hex mii-id-hex.
static int friends(int argc, char **argv) {
	static uint8_t fl[MBOARD_FRIENDS_SIZE];
	static mboard_friend list[MBOARD_FRIENDS];
	uint16_t nick[MBOARD_NICK_MAX + 1];
	uint8_t id[8];
	int res = 0, n, i, k, nl = 0;
	FILE *f;

	if (argc < 4 || !(f = fopen(argv[2], "r+b")) || fread(fl, 1, sizeof(fl), f) != sizeof(fl)) {
		fprintf(stderr, "usage: mboard_host friends FILE list|add|set|remove ...\n");
		return 2;
	}
	if (argc > 5 && strcmp(argv[5], "-")) {
		unhex(argv[5]);
		nl = mboard_utf16(nick, MBOARD_NICK_MAX + 1, argv[5]);
	}
	if (argc > 6)
		for (i = 0; i < 8; ++i)
			sscanf(argv[6] + 2 * i, "%2hhx", &id[i]);
	if (!strcmp(argv[3], "list")) {
		n = mboard_book_parse(fl, list, MBOARD_FRIENDS);
		printf("%d\n", n);
		for (i = 0; i < n; ++i) {
			printf("%d %d %d %016llu ", list[i].slot, list[i].type, list[i].confirmed,
				   (unsigned long long) list[i].wii_number);
			for (k = 0; list[i].nick[k]; ++k)
				printf("%04x", list[i].nick[k]);
			printf(" ");
			for (k = 0; k < 8; ++k)
				printf("%02x", list[i].mii_id[k]);
			printf("\n");
		}
		fclose(f);
		return 0;
	}
	if (!strcmp(argv[3], "add") && argc > 5) {
		res = mboard_book_add(fl, strtoull(argv[4], NULL, 10), nick, nl, argc > 6 ? id : NULL);
	} else if (!strcmp(argv[3], "set") && argc > 5) {
		res = mboard_book_set(fl, atoi(argv[4]), strcmp(argv[5], "-") ? nick : NULL, nl,
								 argc > 6 ? id : NULL);
	} else if (!strcmp(argv[3], "remove")) {
		res = mboard_book_remove(fl, atoi(argv[4]));
	} else {
		fprintf(stderr, "unknown friends command %s\n", argv[3]);
		return 2;
	}
	if (res >= 0 && (fseek(f, 0, SEEK_SET) || fwrite(fl, 1, sizeof(fl), f) != sizeof(fl)))
		res = MBOARD_E_IO;
	fclose(f);
	printf("%d\n", res);
	return 0;
}

int main(int argc, char **argv) {
	if (argc > 1 && !strcmp(argv[1], "play"))
		return play(argc, argv);
	if (argc > 1 && !strcmp(argv[1], "post"))
		return post(argc, argv);
	if (argc > 1 && !strcmp(argv[1], "friends"))
		return friends(argc, argv);
	fprintf(stderr, "usage: mboard_host play|post|friends ...\n");
	return 2;
}
