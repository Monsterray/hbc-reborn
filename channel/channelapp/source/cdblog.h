// The Wii Message Board's play log: add one session to the day's
// "playtimelog" message in the Wii Menu's cdb.vff (docs/messageboard.md).
// No libogc here: the file is reached through callbacks, so the same code
// runs on a PC against an image (tests/cdblog_host).

#ifndef _CDBLOG_H_
#define _CDBLOG_H_

#include <stdint.h>

// Ticks: 60,750,000 a second since 2000-01-01, local time (the Wii's clock).
#define CDBLOG_TICKS_PER_SEC 60750000ull

typedef struct {
	void *ctx;
	// 0 on success. Offsets are bytes into cdb.vff.
	int (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);
	int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t len);
} cdblog_io;

typedef struct {
	uint16_t name[40];  // UTF-16, NUL-terminated unless all 40 are used
	char id[6];         // ASCII, NUL-padded
	uint64_t start, end;
} cdblog_session;

enum {
	CDBLOG_ADDED = 0,     // a new line in the day's message
	CDBLOG_COMBINED = 1,  // the title's line for the day got longer
	CDBLOG_CREATED = 2,   // the day's first line: a new message
	CDBLOG_E_IO = -1,
	CDBLOG_E_FORMAT = -2, // not a cdb.vff this understands; nothing written
	CDBLOG_E_FULL = -3,   // 12 titles already, or no free cluster
	CDBLOG_E_MEM = -4,
};

// wiiid: the console's 8-byte ID (data/nocopy/cdbwiiid.dat), for a new
// message; NULL takes it from a message already in the file.
int cdblog_add(const cdblog_io *io, const cdblog_session *s, uint64_t now,
			   const uint8_t *wiiid);

#endif
