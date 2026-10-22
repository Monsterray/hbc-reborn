/*
 * mboard.h - post to the Wii Message Board from homebrew (libogc).
 *
 * Two ways in, both offline (docs/mboard.md has the formats and the proof):
 *
 *   mboard_post("WiiStation", "Played Final Fantasy VII for 42 minutes.");
 *       A message on today's page, as a game posts one: the SDK's own
 *       loop-back delivery, written into the WiiConnect24 receive box
 *       (/shared2/wc24/mbox/wc24recv.*). The Wii Menu moves it onto the board
 *       the next time it runs. Needs the Wii's Wii number (any Wii that has
 *       been set up has one); needs no WiiConnect24 service, no internet and
 *       no AHBPROT. The title is shown as the sender.
 *
 *   mboard_log_play("Final Fantasy VII", "WSFF7", 42 * 60);
 *       A line in today's "Today's Play History", ending now, the way the
 *       Wii Menu logs a disc. Written into the Wii Menu's own cdb.vff, so it
 *       needs AHBPROT (meta.xml <ahb_access/>): IOS's NAND permission check
 *       is lifted for the moment of the write and put back. The same writer
 *       as the Homebrew Channel's play log (1.8.9 on).
 *
 * Each returns a result >= 0 on success or a negative MBOARD_E_* code, and
 * either writes all of its change or none of it as far as the reader sees:
 * nothing it finds unexpected is touched (MBOARD_E_FORMAT), other messages
 * are never deleted to make room (MBOARD_E_FULL), and new data lands before
 * anything that points to it. Call from one thread at a time, with ISFS
 * free to use; neither call touches the network.
 *
 * Needs nothing beyond libogc. This file and mboard.c are in the public
 * domain; copy them into your project.
 */

#ifndef MBOARD_H
#define MBOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	MBOARD_OK = 0,         /* posted; for mboard_log_play: a new line today */
	MBOARD_COMBINED = 1,   /* mboard_log_play: the title's line today got longer */
	MBOARD_CREATED = 2,    /* mboard_log_play: today's first line, a new message */

	MBOARD_E_IO = -1,      /* a NAND read or write failed part way */
	MBOARD_E_FORMAT = -2,  /* a file is not as expected: nothing was written */
	MBOARD_E_FULL = -3,    /* the receive box, or today's 12 titles, is full */
	MBOARD_E_MEM = -4,     /* out of memory */
	MBOARD_E_ARG = -5,     /* empty text, or an ID that is not 1-6 ASCII letters */
	MBOARD_E_BUSY = -6,    /* WiiConnect24 is using its files now: try later */
	MBOARD_E_NO_ID = -7,   /* this Wii has no Wii number (nwc24msg.cfg) */
	MBOARD_E_DENIED = -8,  /* IOS refused a file (for cdb.vff: no AHBPROT) */
};

/* The longest sender name and text kept, in UTF-16 units; more is cut. */
#define MBOARD_TITLE_MAX 34
#define MBOARD_BODY_MAX 1000

/* UTF-8. A '\n' in the body starts a new line. */
int mboard_post(const char *title_utf8, const char *body_utf8);

/* name: UTF-8, up to 40 UTF-16 units kept. id: 1-6 ASCII letters or digits
 * that tell titles apart (a disc's game ID, a channel's four letters); lines
 * with the same name and ID on one day are added together. */
int mboard_log_play(const char *name_utf8, const char *id, uint32_t seconds);

const char *mboard_strerror(int result);

/* ---- The cores, on any storage --------------------------------------------
 * mboard_post() and mboard_log_play() read the console's settings and the
 * clock, open the files through IOS, and call these. tests/mboard_host runs
 * them on PC images. Define MBOARD_NO_OGC to build mboard.c without libogc
 * (then only these exist). */

/* UTF-8 to UTF-16, at most max units; a bad sequence becomes U+FFFD.
 * Returns the units written. */
int mboard_utf16(uint16_t *out, int max, const char *utf8);

/* Ticks: 60,750,000 a second since 2000-01-01, local time (libogc's gettime()). */
#define MBOARD_TICKS_PER_SEC 60750000ull

typedef struct {
	void *ctx;
	/* 0 on success. Offsets are bytes into the file. */
	int (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);
	int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t len);
} mboard_io;

typedef struct {
	uint64_t wii_number;     /* nwc24msg.cfg: the Wii's own mail ID */
	const char *domain;      /* nwc24msg.cfg: "@wii.com" */
	uint32_t app_id;         /* the posting title, its four letters */
	uint16_t group_id;       /* its maker code */
	int64_t utc;             /* seconds since 1970-01-01, UTC */
	uint64_t local;          /* MBOARD_TICKS_PER_SEC ticks, for file times */
} mboard_env;

#define MBOARD_RECV_CTL_SIZE 0x8000u

/* ctl: the whole wc24recv.ctl, updated in place (the caller writes it back
 * when the result is MBOARD_OK); mbx: wc24recv.mbx. Text in UTF-16. */
int mboard_post_box(uint8_t *ctl, const mboard_io *mbx, const mboard_env *env,
					const uint16_t *title, int title_len, const uint16_t *body, int body_len);

typedef struct {
	uint16_t name[40];  /* UTF-16, NUL-terminated unless all 40 are used */
	char id[6];         /* ASCII, NUL-padded */
	uint64_t start, end;
} mboard_session;

/* cdb: the Wii Menu's cdb.vff. wiiid: the console's 8-byte ID
 * (data/nocopy/cdbwiiid.dat) for a new message; NULL takes it from a message
 * already in the file. */
int mboard_play_vff(const mboard_io *cdb, const mboard_session *s, uint64_t now,
					const uint8_t *wiiid);

#ifdef __cplusplus
}
#endif

#endif
