// Host test for channel/channelapp/source/keymap.c: prints the buttons for
// the keys given as USB usage codes (hex) on the command line, as hex.
// usage: test 28 2c   (Enter, Space)

#include <stdio.h>
#include <stdlib.h>

#include "../../../channel/channelapp/source/keymap.c"

int main (int argc, char **argv) {
	u8 keys[KEYMAP_BYTES] = { 0 };
	int i;

	for(i=1;i<argc;i++)
		KEYMAP_SET (keys, (u8) strtoul (argv[i], NULL, 16));
	printf ("%04x\n", (unsigned) keymap_buttons (keys));
	return 0;
}
