/*
 * mboard.h - the Wii Message Board from homebrew (libogc): posts, mail to
 * another Wii, the play history, and the Address Book.
 *
 *   mboard_post(title, text)        a message on this Wii's board
 *   mboard_post_msg(&msg)           the same, with a Mii, a photo, stationery,
 *                                   Reply, a delay or a filing date
 *   mboard_send_msg(number, &msg)   that message to another Wii (the internet)
 *   mboard_log_play(name, id, s)    a line in today's play history (AHBPROT)
 *   mboard_mii(slot, face)          a Mii from this Wii's Mii database
 *   mboard_friends(...)             the Address Book: read, add, change, remove
 *
 * Posting writes into this Wii's WiiConnect24 receive box, as a game's
 * message to its own Wii is delivered (the SDK's loop-back); the Wii Menu
 * files it on the board the next time it starts. It needs the Wii's Wii
 * number (any Wii set up has one), and no internet, no WiiConnect24 service
 * and no AHBPROT. Sending writes into the send box instead and asks KD (IOS's
 * WiiConnect24 service) to upload it now; both Wiis then need a WiiConnect24
 * account that works (WiiLink's, today) and each other in the Address Book.
 * The play history is the Wii Menu's own cdb.vff, so writing it needs
 * AHBPROT (meta.xml <ahb_access/>).
 *
 * Every call returns a result >= 0 on success or a negative MBOARD_E_* code,
 * and writes all of its change or none of it as far as a reader sees: a file
 * it does not expect is left alone (MBOARD_E_FORMAT), nothing is deleted to
 * make room (MBOARD_E_FULL), and new data lands before anything that points
 * to it. Call from one thread at a time, with ISFS free to use. Only
 * mboard_send_msg touches the network (through KD).
 *
 * docs/mboard.md has the formats and the proof on a Wii. Needs nothing beyond
 * libogc. This file and mboard.c are in the public domain: copy them into
 * your project.
 */

#ifndef MBOARD_H
#define MBOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	MBOARD_OK = 0,         /* done; for mboard_log_play: a new line today */
	MBOARD_COMBINED = 1,   /* mboard_log_play: the title's line today got longer */
	MBOARD_CREATED = 2,    /* mboard_log_play: today's first line, a new message */
	MBOARD_QUEUED = 3,     /* mboard_send_msg: in the send box; KD would not send it
	                          now, so it goes on WiiConnect24's next round */

	MBOARD_E_IO = -1,      /* a NAND read or write failed part way */
	MBOARD_E_FORMAT = -2,  /* a file is not as expected: nothing was written */
	MBOARD_E_FULL = -3,    /* a mailbox, today's 12 play titles, or the Address Book is full */
	MBOARD_E_MEM = -4,     /* out of memory */
	MBOARD_E_ARG = -5,     /* a bad argument: empty text, a bad ID, Wii number,
	                          nickname, date or slot, or a Mii with no creation ID */
	MBOARD_E_BUSY = -6,    /* WiiConnect24 is using its files now: try again later */
	MBOARD_E_NO_ID = -7,   /* this Wii has no Wii number (nwc24msg.cfg) */
	MBOARD_E_DENIED = -8,  /* IOS refused a file (for cdb.vff: no AHBPROT) */
	MBOARD_E_TOO_BIG = -9, /* a photo or board data over its limit, or the whole message */
	MBOARD_E_NO_MII = -10, /* mboard_mii(): no Mii in that slot */
};

const char *mboard_strerror(int result);

/* ---- Posting and sending ---------------------------------------------------- */

/* The longest sender name and text kept, in UTF-16 units; more is cut. */
#define MBOARD_TITLE_MAX 34
#define MBOARD_BODY_MAX 1000

/* A Mii's RFLCharData, as the Wii's Mii database holds it. */
#define MBOARD_FACE_SIZE 74
/* The SDK's NWC24SetMsgAttached() limits. */
#define MBOARD_BOARD_MAX 0x1E000u     /* board data */
#define MBOARD_ATTACH_TOTAL 0x245B0u  /* photo and board data together: less than this */
#define MBOARD_DELAY_MAX 240          /* hours */

/* UTF-8. A '\n' in the body starts a new line. No Reply button. */
int mboard_post(const char *title_utf8, const char *body_utf8);

typedef struct {
	const char *title;          /* UTF-8: the sender's name */
	const char *body;           /* UTF-8; '\n' starts a new line */
	const uint8_t *face;        /* a Mii, MBOARD_FACE_SIZE bytes, shown as the icon; or NULL */
	const void *jpeg;           /* a photo, a JPEG (the Wii Menu converts it); or NULL */
	uint32_t jpeg_size;
	const void *board;          /* board data, a U8 archive (application/x-wii-msgboard):
	                               stationery (tools/msgboard/stationery.py) and/or a
	                               chjump.bin, a button that opens a channel; or NULL */
	uint32_t board_size;
	int reply;                  /* nonzero: a Reply button */
	int delay_hours;            /* 0, or hold it back this long before it shows (1-240) */
	uint16_t file_year;         /* 0, or the date it is filed under: 2000-2035, */
	uint8_t file_month, file_day;  /* month 1-12 */
} mboard_msg;

/* A message on this Wii's board, with everything in msg. */
int mboard_post_msg(const mboard_msg *msg);

/* The same message to another Wii, by its 16-digit Wii number. MBOARD_OK when
 * KD took the upload request, MBOARD_QUEUED when it waits for KD's next round.
 * The other Wii files it when it next fetches mail, if each Wii has the other
 * in its Address Book. */
int mboard_send_msg(uint64_t to_wii_number, const mboard_msg *msg);

/* Slot 0-99 of this Wii's Mii database (/shared2/menu/FaceLib/RFL_DB.dat), for
 * mboard_msg.face or a contact's Mii. MBOARD_E_NO_MII for an empty slot. */
int mboard_mii(int slot, uint8_t face[MBOARD_FACE_SIZE]);

/* ---- The play history ---------------------------------------------------------- */

/* A line in today's "Today's Play History", ending now, as the Wii Menu logs a
 * disc. name: UTF-8, up to 40 UTF-16 units kept. id: 1-6 ASCII letters or
 * digits that tell titles apart (a disc's game ID, a channel's four letters);
 * lines with the same name and ID on one day are added together. Needs
 * AHBPROT: IOS's NAND permission check is lifted for the write and put back.
 * The Homebrew Channel's own play log is this writer. */
int mboard_log_play(const char *name_utf8, const char *id, uint32_t seconds);

/* ---- The Address Book ------------------------------------------------------------
 * /shared2/wc24/nwc24fl.bin, the Wii Menu's Address Book (docs/mboard.md,
 * "The Address Book"). Each call pauses KD around the file, as KD reads and
 * writes it too. */

#define MBOARD_FRIENDS 100
#define MBOARD_FRIENDS_SIZE 32864u
#define MBOARD_NICK_MAX 11        /* UTF-16 units */

typedef struct {
	int slot;                    /* 0-99 */
	int type;                    /* 1: a Wii, 2: an e-mail address */
	int confirmed;               /* both Wiis have registered each other */
	uint64_t wii_number;         /* type 1 */
	char email[64];              /* type 2, cut at 63 bytes */
	uint16_t nick[MBOARD_NICK_MAX + 1];  /* UTF-16, NUL-terminated */
	uint8_t mii_id[8];           /* the contact's Mii, as its creation ID; 0 for none */
} mboard_friend;

/* Every contact, up to max; returns how many. */
int mboard_friends(mboard_friend *out, int max);
/* Adds a Wii as the Wii Menu does: pending until both Wiis have registered
 * each other (KD sends the registration). nick: UTF-8, 1-11 UTF-16 units;
 * mii: a Mii (MBOARD_FACE_SIZE bytes) to show for it, or NULL. Returns the
 * slot, MBOARD_E_FULL, or MBOARD_E_ARG (a number already there, or bad). */
int mboard_friend_add(uint64_t wii_number, const char *nick_utf8, const uint8_t *mii);
/* A contact's nickname and Mii (either NULL: keep it). The address cannot
 * change: remove the contact and add it again. Returns the slot. */
int mboard_friend_set(int slot, const char *nick_utf8, const uint8_t *mii);
int mboard_friend_remove(int slot);

/* ---- The cores, on any storage ----------------------------------------------------
 * The calls above read the console's settings and clock, open the files
 * through IOS, and call these. tests/mboard_host runs them on PC images.
 * Define MBOARD_NO_OGC to build mboard.c without libogc: then only these
 * exist (the Homebrew Channel builds it so, for its play log). */

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
	const char *domain;      /* nwc24msg.cfg: "@rc24.xyz" (WiiLink), "@wii.com" */
	uint32_t app_id;         /* the posting title, its four letters */
	uint16_t group_id;       /* its maker code */
	int64_t utc;             /* seconds since 1970-01-01, UTC */
	uint64_t local;          /* MBOARD_TICKS_PER_SEC ticks, for file times */
} mboard_env;

#define MBOARD_RECV_CTL_SIZE 0x8000u
#define MBOARD_SEND_CTL_SIZE 0x4000u

/* ctl: the whole wc24recv.ctl, updated in place (the caller writes it back
 * when the result is MBOARD_OK); mbx: wc24recv.mbx. */
int mboard_post_box(uint8_t *ctl, const mboard_io *mbx, const mboard_env *env,
					const mboard_msg *msg);

/* ctl: the whole wc24send.ctl (MBOARD_SEND_CTL_SIZE), mbx: wc24send.mbx; to:
 * another Wii's number (this Wii's own is MBOARD_E_ARG: post that instead). */
int mboard_send_box(uint8_t *ctl, const mboard_io *mbx, const mboard_env *env, uint64_t to,
					const mboard_msg *msg);

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

/* fl: the whole nwc24fl.bin (MBOARD_FRIENDS_SIZE bytes), changed in place; the
 * caller writes it back when the result is >= 0. Results as the calls above;
 * mii_id: a Mii's creation ID (8 bytes, RFLCharData 0x18), or NULL. */
int mboard_book_parse(const uint8_t *fl, mboard_friend *out, int max);
int mboard_book_add(uint8_t *fl, uint64_t wii_number, const uint16_t *nick, int nick_len,
					const uint8_t *mii_id);
int mboard_book_set(uint8_t *fl, int slot, const uint16_t *nick, int nick_len,
					const uint8_t *mii_id);
int mboard_book_remove(uint8_t *fl, int slot);

#ifdef __cplusplus
}
#endif

#endif
