// sdk/mii's example: a whole program, built in CI (make -C sdk/mii/example).
//
// It lists this Wii's Miis and the Miis on Wii Remote 1. Press A to add a
// random Mii named "Example" to the Mii Channel's plaza (it asks first), and
// HOME to leave. Public domain, like the library.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>   // gettime()
#include <wiiuse/wpad.h>

#include "mii.h"

static void list_database(void) {
	mii_db db;
	mii m;
	char name[MII_NAME_UTF8_MAX];
	int r, i;

	memset(&db, 0, sizeof(db));
	r = mii_db_load(&db);
	if (r < 0) {
		printf("Mii database: %s\n", mii_strerror(r));   // MII_E_NO_FILE: never opened
		return;
	}
	printf("%d Miis in the plaza, %d in the hidden database%s\n", mii_db_count(&db),
		   mii_hidden_count(&db), mii_db_isolated(&db) ? " (mingling off)" : "");
	for (i = 0; i < MII_DB_SLOTS; ++i) {
		if (mii_db_get(&db, i, &m) != MII_OK)
			continue;
		mii_get_name(m.name, name, sizeof(name));
		printf("  %2d %-10s %s, height %3u, build %3u, likes %s%s\n", i, name,
			   m.sex == MII_FEMALE ? "F" : "M", m.height, m.build, mii_color_name(m.fav_color),
			   m.favorite ? ", a favourite" : "");
	}
	mii_db_free(&db);
}

static void list_remote(void) {
	mii_remote rm;
	mii m;
	char name[MII_NAME_UTF8_MAX];
	u64 end = gettime() + secs_to_ticks(10);
	int r, i;

	// WPAD_Init() was called in main(); a Remote connects a few seconds later.
	while (WPAD_Probe(0, NULL) != WPAD_ERR_NONE && gettime() < end)
		usleep(100 * 1000);
	r = mii_remote_read(0, &rm);
	if (r < 0) {
		printf("Wii Remote 1: %s\n", mii_strerror(r));
		return;
	}
	printf("Wii Remote 1: %s\n", rm.copy < 0 ? "no Miis yet" : "");
	for (i = 0; i < MII_REMOTE_SLOTS; ++i)
		if (mii_remote_get(&rm, i, &m) == MII_OK) {
			mii_get_name(m.name, name, sizeof(name));
			printf("  %2d %s\n", i, name);
		}
}

static void add_example(void) {
	mii_db db;
	mii m;
	const char *why;
	uint32_t seed = (uint32_t) gettime();
	int r, slot;

	mii_random(&m, MII_ANY, MII_ADULT, &seed);
	mii_set_name(m.name, "Example");
	mii_set_name(m.creator, "sdk/mii");
	m.height = 90;                 // 0 shortest .. 128 tallest
	m.build = 40;                  // 0 thinnest .. 128 heaviest
	m.birth_month = 10;
	m.birth_day = 24;
	m.fav_color = MII_GREEN;
	m.favorite = 1;
	m.local_only = 0;              // Mingle on
	if ((r = mii_make_id(&m)) < 0) {
		printf("no creation ID: %s\n", mii_strerror(r));
		return;
	}
	if (mii_check(&m, &why) != MII_OK) {
		printf("the Wii would refuse it: %s\n", why);
		return;
	}
	memset(&db, 0, sizeof(db));
	if ((r = mii_db_load(&db)) < 0) {
		printf("Mii database: %s\n", mii_strerror(r));
		return;
	}
	slot = mii_db_add(&db, &m);
	r = slot < 0 ? slot : mii_db_save(&db);
	if (r == MII_E_IO || r == MII_E_VERIFY)
		r = mii_db_save(&db);      // a save that did not finish may be retried
	if (r < 0)
		printf("not added: %s\n", mii_strerror(r));   // MII_E_CHANGED: load again, add again
	else
		printf("added to slot %d\n", slot);
	mii_db_free(&db);
}

static u32 wait_button(void) {
	u32 down;

	for (;;) {
		WPAD_ScanPads();
		down = WPAD_ButtonsDown(0);
		if (down)
			return down;
		VIDEO_WaitVSync();
	}
}

int main(void) {
	GXRModeObj *mode;
	void *xfb;

	VIDEO_Init();
	WPAD_Init();
	mode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
	console_init(xfb, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(mode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	printf("\n sdk/mii %s example\n\n", MII_VERSION);
	list_database();
	list_remote();
	printf("\nA: add a Mii named \"Example\" to the plaza.  HOME: leave.\n");
	for (;;) {
		u32 down = wait_button();

		if (down & WPAD_BUTTON_HOME)
			break;
		if (down & WPAD_BUTTON_A) {
			printf("Add it? A: yes, B: no\n");
			if (wait_button() & WPAD_BUTTON_A) {
				add_example();
				list_database();
			}
		}
	}
	WPAD_Shutdown();
	return 0;
}
