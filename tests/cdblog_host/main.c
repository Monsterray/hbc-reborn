// Host driver for channel/channelapp/source/cdblog.c (tests/test_cdblog.py):
//   cdblog_host IMAGE NAME ID START END NOW [WIIID-HEX]
// Times are local, YYYY-MM-DDTHH:MM:SS. Prints cdblog_add()'s result.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdblog.h"

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

static uint64_t ticks(const char *s) {
	int y, mo, d, h, mi, sec = 0;

	if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 5) {
		fprintf(stderr, "bad time %s\n", s);
		exit(2);
	}
	return ((uint64_t) (days_from_civil(y, mo, d) - 10957) * 86400 + h * 3600 + mi * 60 + sec) *
		   CDBLOG_TICKS_PER_SEC;
}

int main(int argc, char **argv) {
	cdblog_session s;
	cdblog_io io;
	uint8_t id[8];
	FILE *f;
	size_t i;
	int res;

	if (argc < 7) {
		fprintf(stderr, "usage: cdblog_host IMAGE NAME ID START END NOW [WIIID-HEX]\n");
		return 2;
	}
	memset(&s, 0, sizeof(s));
	for (i = 0; argv[2][i] && i < 40; ++i)
		s.name[i] = (uint8_t) argv[2][i];
	strncpy(s.id, argv[3], sizeof(s.id));
	s.start = ticks(argv[4]);
	s.end = ticks(argv[5]);
	if (argc > 7)
		for (i = 0; i < 8; ++i)
			sscanf(argv[7] + 2 * i, "%2hhx", &id[i]);
	f = fopen(argv[1], "r+b");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	io.ctx = f;
	io.read = file_read;
	io.write = file_write;
	res = cdblog_add(&io, &s, ticks(argv[6]), argc > 7 ? id : NULL);
	fclose(f);
	printf("%d\n", res);
	return 0;
}
