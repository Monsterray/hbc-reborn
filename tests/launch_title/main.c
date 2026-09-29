// Launch an installed title through the reload stub of the loader that
// started this app: set the stub's return-title words, then exit().
// usage (argv from Wiiload): launch_title.dol 000100014f484243
//
// This takes the same path as any app exiting back to HBC, which works on
// real hardware. An earlier version called libogc's WII_LaunchTitle, which
// hung with a black screen on the bench Wii when sent from the stock HBC.
// If the title is missing, HBC 1.4.0's stub falls back to the system menu.

#include <stdlib.h>
#include <string.h>
#include <ogcsys.h>

// The HBC reload stub's layout (channel/channelapp/config.h).
#define STUB_ADDR 0x80001800
#define STUB_MAGIC 0x4c4f41444b544858ull   // "LOADKTHX"
#define STUB_ADDR_MAGIC ((volatile u64 *) 0x80002f00)
#define STUB_ADDR_TITLE ((volatile u64 *) 0x80002f08)

int main(int argc, char **argv) {
	u64 title;
	char *end;

#ifdef LAUNCH_TITLE
	// Built-in title for loaders that pass no arguments, such as Dolphin -e.
	title = LAUNCH_TITLE;
	end = "";
	(void) argc;
	(void) argv;
#else
	if (argc < 2)
		return 1;
	title = strtoull(argv[1], &end, 16);
#endif
	if (*end || !title)
		return 1;

	// Without an HBC reload stub, exit() would reset the console instead.
	if (memcmp((const void *) (STUB_ADDR + 4), "STUBHAXX", 8))
		return 1;

	*STUB_ADDR_MAGIC = STUB_MAGIC;
	*STUB_ADDR_TITLE = title;
	DCFlushRange((void *) STUB_ADDR_MAGIC, 16);

	exit(0);
}
