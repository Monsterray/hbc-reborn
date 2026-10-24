/*
 * mii.h - Miis from homebrew (libogc): the data, this Wii's Mii database,
 * and the Miis kept on a Wii Remote.
 *
 *   mii_unpack / mii_pack       the 74-byte Mii (RFLCharData) and a struct
 *   mii_check(&m, &why)         would the Wii accept it? If not, why not
 *   mii_default / mii_random    a new Mii to start from
 *   mii_make_id(&m)             a new creation ID, as the Mii Channel makes it
 *   mii_import / mii_store      .mii files (74 bytes) and the 76-byte form
 *                               with a CRC (RFLStoreData)
 *   mii_db_load / _save         RFL_DB.dat: the Mii Channel's 100 Miis and
 *                               the hidden database of Miis met elsewhere
 *   mii_remote_read / _write    the 10 Miis a Wii Remote carries
 *
 * Everything that touches only bytes (packing, checks, the database and the
 * Remote's layout, the CRCs) works without libogc, so a PC tool or a test can
 * use it: build with MII_NO_OGC. The rest needs libogc, ISFS free to use, and
 * for a Remote, WPAD started and the Remote connected.
 *
 * Writes are careful. A database whose CRC is wrong, or that is not the size
 * the Wii Menu makes, is never written (MII_E_FORMAT): the Wii Menu deletes
 * and recreates a database it cannot read, so "fixing" one would hide that it
 * was already lost. mii_db_save writes the change in one open of the file,
 * new Mii data before the table that points to it, and reads it back. A
 * Remote keeps two copies of its Miis; mii_remote_write writes and checks one
 * copy at a time, so one of them always holds a whole set.
 *
 * Every call that can fail returns a result >= 0 on success or a negative
 * MII_E_* code (mii_strerror() names it). docs/mii.md has the formats, their
 * sources and the proof on a Wii. This file and mii.c are in the public
 * domain: copy them into your project.
 */

#ifndef MII_H
#define MII_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	MII_OK = 0,
	MII_E_IO = -1,         /* a NAND or Wii Remote transfer failed part way */
	MII_E_FORMAT = -2,     /* a database or Remote set is not as the Wii makes it: nothing written */
	MII_E_FULL = -3,       /* no free slot */
	MII_E_MEM = -4,        /* out of memory */
	MII_E_ARG = -5,        /* a bad argument: a slot out of range, a NULL, a bad size */
	MII_E_INVALID = -6,    /* the Mii fails the Wii's check (mii_check says why) */
	MII_E_EXISTS = -7,     /* a Mii with that creation ID is already in another slot */
	MII_E_EMPTY = -8,      /* no Mii there */
	MII_E_CRC = -9,        /* a 76-byte Mii whose CRC does not match */
	MII_E_NO_REMOTE = -10, /* no Wii Remote connected on that channel (or no WPAD) */
	MII_E_TIMEOUT = -11,   /* the Wii Remote did not answer */
	MII_E_VERIFY = -12,    /* what was read back is not what was written */
	MII_E_CHANGED = -13,   /* RFL_DB.dat changed since mii_db_load: load it again */
	MII_E_DENIED = -14,    /* IOS refused the file */
};

const char *mii_strerror(int result);

/* ---- A Mii ------------------------------------------------------------------ */

#define MII_SIZE 74          /* RFLCharData: the Mii as every Wii file holds it */
#define MII_STORE_SIZE 76    /* RFLStoreData: the same and a CRC-16 */
#define MII_NAME_MAX 10      /* UTF-16 units in a name, and in the creator's name */
#define MII_ID_SIZE 8

enum { MII_MALE = 0, MII_FEMALE = 1, MII_ANY = -1 };
enum { MII_CHILD = 0, MII_ADULT = 1, MII_ELDER = 2 };

/* Each field, in the order and with the ranges of the Wii's own check
 * (mii_check). Colours and types are indexes into the Mii Channel's own
 * pickers, left to right and top to bottom. */
typedef struct {
	uint8_t sex;             /* MII_MALE or MII_FEMALE */
	uint8_t birth_month;     /* 1-12, or 0 with birth_day 0 for none */
	uint8_t birth_day;       /* 1-31 (the month's length; February has 29) */
	uint8_t fav_color;       /* 0-11 */
	uint8_t favorite;        /* 1: one of the Mii Channel's favourites */
	uint16_t name[MII_NAME_MAX];     /* UTF-16; NUL after the last unit if under 10 */
	uint8_t height;          /* 0-128; 64 is the middle */
	uint8_t build;           /* 0-128; 64 is the middle */
	uint8_t id[MII_ID_SIZE]; /* the creation ID: which Mii this is (mii_make_id) */

	uint8_t face_type;       /* 0-7  */
	uint8_t face_color;      /* 0-5, the skin tone */
	uint8_t face_tex;        /* 0-11, face features (makeup, wrinkles) */
	uint8_t local_only;      /* 1: "Don't mingle": not sent to other Wiis */
	uint8_t kind;            /* 2 bits RFL keeps but never sets: leave them */

	uint8_t hair_type;       /* 0-71 */
	uint8_t hair_color;      /* 0-7  */
	uint8_t hair_flip;       /* 0-1, the parting on the other side */

	uint8_t brow_type;       /* 0-23 */
	uint8_t brow_rotate;     /* 0-11 */
	uint8_t brow_color;      /* 0-7  */
	uint8_t brow_scale;      /* 0-8  */
	uint8_t brow_y;          /* 3-18: the only part with a lower limit */
	uint8_t brow_x;          /* 0-12, the space between them */

	uint8_t eye_type;        /* 0-47 */
	uint8_t eye_rotate;      /* 0-7  */
	uint8_t eye_y;           /* 0-18 */
	uint8_t eye_color;       /* 0-5  */
	uint8_t eye_scale;       /* 0-7  */
	uint8_t eye_x;           /* 0-12 */

	uint8_t nose_type;       /* 0-11 */
	uint8_t nose_scale;      /* 0-8  */
	uint8_t nose_y;          /* 0-18 */

	uint8_t mouth_type;      /* 0-23 */
	uint8_t mouth_color;     /* 0-2  */
	uint8_t mouth_scale;     /* 0-8  */
	uint8_t mouth_y;         /* 0-18 */

	uint8_t glass_type;      /* 0-8, 0 for none */
	uint8_t glass_color;     /* 0-5  */
	uint8_t glass_scale;     /* 0-7  */
	uint8_t glass_y;         /* 0-20 */

	uint8_t mustache;        /* 0-3, 0 for none */
	uint8_t beard_type;      /* 0-3, 0 for none */
	uint8_t beard_color;     /* 0-7  */
	uint8_t beard_scale;     /* 0-8  */
	uint8_t beard_y;         /* 0-16 */

	uint8_t mole;            /* 0-1  */
	uint8_t mole_scale;      /* 0-8  */
	uint8_t mole_y;          /* 0-30 */
	uint8_t mole_x;          /* 0-16 */

	uint16_t creator[MII_NAME_MAX];  /* UTF-16, the creator's name; may be empty */

	uint8_t reserved[7];     /* the record's padding bits, kept so that
	                            mii_pack(mii_unpack(x)) == x */
} mii;

/* The 74 bytes and the struct. Neither checks anything. */
void mii_unpack(const uint8_t raw[MII_SIZE], mii *m);
void mii_pack(const mii *m, uint8_t raw[MII_SIZE]);

/* MII_OK if the Wii would accept the Mii (RFL's RFLiCheckValidInfo, and a
 * creation ID that is not 0); otherwise MII_E_INVALID, and *why (if not NULL)
 * names the first problem, such as "eyebrow Y below 3". */
int mii_check(const mii *m, const char **why);

/* Names: UTF-8 in and out. Set returns the UTF-16 units kept (at most 10;
 * more is cut); get returns the UTF-8 length written (always NUL ended). */
int mii_set_name(uint16_t name[MII_NAME_MAX], const char *utf8);
int mii_get_name(const uint16_t name[MII_NAME_MAX], char *utf8, size_t size);

/* A plain Mii to edit: every part in range, named "Mii", no creation ID.
 * sex is MII_MALE or MII_FEMALE. */
void mii_default(mii *m, int sex);

/* A random Mii in the Wii's proportions: sex MII_MALE, MII_FEMALE or MII_ANY;
 * age MII_CHILD, MII_ADULT, MII_ELDER or MII_ANY. *seed is any number (the
 * time, say) and is advanced, so a loop makes a different Mii each time.
 * Named "Mii", with no creation ID. */
void mii_random(mii *m, int sex, int age, uint32_t *seed);

/* ---- The creation ID ----------------------------------------------------------
 * Two big-endian words. The first holds flags and the creation time, in
 * 4-second units since 2006-01-01; the second comes from the Wi-Fi MAC
 * address of the Wii that made the Mii. All zero means no Mii. */
#define MII_ID_NORMAL 0x80000000u   /* set on every Mii a Wii makes; clear: "special" */
#define MII_ID_DS 0x40000000u       /* made on a Nintendo DS */
#define MII_ID_TEMP 0x20000000u     /* temporary: never stored on a Wii */

int mii_id_empty(const uint8_t id[MII_ID_SIZE]);
int mii_same_id(const uint8_t a[MII_ID_SIZE], const uint8_t b[MII_ID_SIZE]);
uint32_t mii_id_flags(const uint8_t id[MII_ID_SIZE]);
/* Seconds since 2006-01-01 00:00 (local time on the Wii that made it). */
uint32_t mii_id_time(const uint8_t id[MII_ID_SIZE]);
/* The ID RFL makes: ticks is the Wii's time base (60.75 MHz since
 * 2000-01-01, libogc's gettime()), mac the Wi-Fi MAC. */
void mii_id_make(uint8_t id[MII_ID_SIZE], uint64_t ticks, const uint8_t mac[6]);
/* The second word for a MAC: a Mii whose second word matches was made on
 * that Wii (RFL's "home" Mii). */
uint32_t mii_id_home(const uint8_t mac[6]);

/* ---- Files: .mii and the 76-byte form --------------------------------------------
 * CRC-16/XMODEM (polynomial 0x1021, start 0), as every Mii CRC on the Wii. */
uint16_t mii_crc16(const void *data, size_t size);
/* raw (74 bytes) -> 76 bytes with its CRC. */
void mii_store(const uint8_t raw[MII_SIZE], uint8_t out[MII_STORE_SIZE]);
/* A file of 74 bytes (.mii) or 76 (with the CRC, which must match:
 * MII_E_CRC); MII_E_ARG for any other size. The Mii is not checked. */
int mii_import(const void *file, size_t size, mii *m);

/* ---- The Mii database: /shared2/menu/FaceLib/RFL_DB.dat ------------------------
 * 100 slots in the Mii Channel's order (its plaza), and the hidden database:
 * up to 10,000 Miis this Wii has met (on letters, from Remotes, by
 * mingling), oldest first, that the Mii Channel never shows but games can
 * see. A program that only reads can use mii_get. */
#define MII_DB_SLOTS 100
#define MII_HIDDEN_SLOTS 10000
#define MII_DB_FILE_SIZE 0xBE6C0u   /* 779,968 bytes, as the Wii Menu makes it */

typedef struct {
	uint8_t *file;           /* the whole file, MII_DB_FILE_SIZE bytes */
	uint32_t dirty_lo;       /* hidden Mii data changed since loading, for */
	uint32_t dirty_hi;       /*   mii_db_save: [lo, hi) */
	uint16_t loaded_crc;     /* the file's CRC when loaded */
	uint8_t owned;           /* mii_db_load allocated file */
	uint32_t home;           /* this Wii's mii_id_home(), set by mii_db_load;
	                            0 (attach): no Mii is taken as another Wii's */
} mii_db;

/* Use a file already in memory (MII_DB_FILE_SIZE bytes, kept by the caller):
 * MII_E_FORMAT unless it is a database the Wii can read. */
int mii_db_attach(mii_db *db, uint8_t *file, size_t size);

int mii_db_get(const mii_db *db, int slot, mii *m);     /* MII_E_EMPTY for none */
int mii_db_count(const mii_db *db);
/* The slot holding that creation ID, or MII_E_EMPTY. */
int mii_db_find(const mii_db *db, const uint8_t id[MII_ID_SIZE]);
/* Put a Mii in a slot, replacing what is there. It must pass mii_check, not
 * be temporary, and not be in another slot (MII_E_EXISTS); it leaves the
 * hidden database, as RFL does. A Mii made on another Wii (db->home set and
 * not matching) is set local only, as RFL sets it. */
int mii_db_put(mii_db *db, int slot, const mii *m);
/* The same in the first free slot; returns the slot, or MII_E_FULL. */
int mii_db_add(mii_db *db, const mii *m);
int mii_db_remove(mii_db *db, int slot);
/* 1 if the Mii Channel's mingling is off ("isolation"). */
int mii_db_isolated(const mii_db *db);

/* The hidden database, oldest first: for (i = mii_hidden_first(db); i >= 0;
 * i = mii_hidden_next(db, i)). An index stays the Mii's until it is removed. */
int mii_hidden_count(const mii_db *db);
int mii_hidden_first(const mii_db *db);
int mii_hidden_next(const mii_db *db, int index);
/* Hidden Miis keep no birthday or creator's name. */
int mii_hidden_get(const mii_db *db, int index, mii *m);
int mii_hidden_find(const mii_db *db, const uint8_t id[MII_ID_SIZE]);
/* Add or update a Mii, as RFL does: in its own place if there, else in a
 * free one, else in place of the oldest. Not a local-only Mii or one in the
 * 100 slots (MII_E_EXISTS). Returns the index. */
int mii_hidden_put(mii_db *db, const mii *m);
int mii_hidden_remove(mii_db *db, int index);

/* ---- The Miis on a Wii Remote ------------------------------------------------------
 * Ten slots, in the Remote's own memory (EEPROM): the Mii Channel's "Wii
 * Remote" page. A slot is empty when its creation ID is 0. */
#define MII_REMOTE_SLOTS 10
#define MII_REMOTE_ADDR 0x0FCA      /* the first copy; the second follows it */
#define MII_REMOTE_BLOCK 0x2F0      /* one copy: "RNCD", the slots, a CRC */

typedef struct {
	uint8_t raw[MII_REMOTE_SLOTS][MII_SIZE];
	uint16_t secret;         /* bit i: slot i came from the hidden database */
	int8_t copy;             /* the copy that was read: 0 or 1; -1 if neither
	                            was whole (a new Remote): an empty set */
} mii_remote;

/* Both copies, as read from the Remote (2 x MII_REMOTE_BLOCK bytes). */
int mii_remote_parse(const uint8_t *mem, size_t size, mii_remote *r);
/* One copy for writing. */
void mii_remote_build(const mii_remote *r, uint8_t block[MII_REMOTE_BLOCK]);
int mii_remote_get(const mii_remote *r, int slot, mii *m);  /* MII_E_EMPTY for none */
/* Not favourite on a Remote (RFL clears it); MII_E_INVALID unless it passes
 * mii_check. */
int mii_remote_put(mii_remote *r, int slot, const mii *m);
int mii_remote_remove(mii_remote *r, int slot);

#ifndef MII_NO_OGC
/* ---- On the Wii -------------------------------------------------------------- */

/* Read RFL_DB.dat (about 780 KB, in MEM2) and check it. Any title may. */
int mii_db_load(mii_db *db);
/* Write the changes back. MII_E_CHANGED if someone else wrote the file since
 * it was loaded. The Wii Menu, the Mii Channel and any game using RFL keep
 * their own copy while they run and would write over it, so save from your
 * own program, not alongside one of those. */
int mii_db_save(mii_db *db);
void mii_db_free(mii_db *db);

/* One Mii from the 100 slots, without keeping the database. */
int mii_get(int slot, mii *m);

/* This Wii's Wi-Fi MAC address, which RFL builds its IDs from. Just after
 * an IOS reload (an app launched from HBC) IOS's network module takes a few
 * seconds to start; this waits for it, up to 8 s, once. */
int mii_wifi_mac(uint8_t mac[6]);
/* A new creation ID on m, from this Wii's clock and Wi-Fi MAC. */
int mii_make_id(mii *m);
/* 1 if m was made on this Wii. */
int mii_is_home(const mii *m);

/* chan 0-3. Each call waits for the Remote, at most a few seconds. */
int mii_remote_read(int chan, mii_remote *r);
/* Both copies as the Remote holds them (2 x MII_REMOTE_BLOCK bytes), for a
 * backup; mii_remote_parse reads them. */
int mii_remote_read_raw(int chan, uint8_t mem[2 * MII_REMOTE_BLOCK]);
int mii_remote_write(int chan, const mii_remote *r);
#endif

#ifdef __cplusplus
}
#endif

#endif
