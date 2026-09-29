// Launch an installed title, e.g. to start the HBC channel on a bench Wii.
// usage (argv from Wiiload): launch_title.dol 000100014f484243
// If the launch fails, the app exits back to the loader that started it.

#include <stdlib.h>
#include <ogcsys.h>
#include <ogc/wiilaunch.h>

int main(int argc, char **argv) {
	u64 title;
	char *end;

	if (argc < 2)
		return 1;
	title = strtoull(argv[1], &end, 16);
	if (*end || !title)
		return 1;

	if (WII_Initialize() < 0)
		return 1;
	WII_LaunchTitle(title);

	// Only reached when ES refused the title.
	return 1;
}
