// Launch an installed title through libogc's WII_LaunchTitle.
// usage (argv from Wiiload): launch_title.dol 000100014f484243
// If the launch fails, the app exits back to the loader that started it.
//
// This works in Dolphin, but on the bench Wii (IOS58) it hung with a black
// screen when sent from the stock Homebrew Channel; Reset returned to that
// HBC. Start an installed channel from the Wii Menu on real hardware.

#include <stdlib.h>
#include <ogcsys.h>
#include <ogc/wiilaunch.h>

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

	if (WII_Initialize() < 0)
		return 1;
	WII_LaunchTitle(title);

	// Only reached when ES refused the title.
	return 1;
}
