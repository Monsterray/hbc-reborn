// sdk/wiiupdate/release.c on the host: reads a /releases reply from a file
// and prints what it picks, then compares the versions given after it.
//   test REPLY SUFFIX [A B]...

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wiiupdate.h"

int main(int argc, char **argv) {
	static char json[1 << 20];
	wiiupdate_release r;
	FILE *f;
	size_t n;
	int i;

	if (argc < 3 || !(f = fopen(argv[1], "rb")))
		return 2;
	n = fread(json, 1, sizeof(json) - 1, f);
	fclose(f);
	json[n] = 0;

	printf("result=%d\n", wiiupdate_newest_release(json, argv[2], &r));
	printf("tag=%s\nurl=%s\nnotes=", r.tag, r.wad_url);
	fwrite(r.notes, 1, strlen(r.notes), stdout);
	printf("\n");
	for (i = 3; i + 1 < argc; i += 2)
		printf("cmp %s %s %d\n", argv[i], argv[i + 1],
			   (wiiupdate_compare_versions(argv[i], argv[i + 1]) > 0) -
			   (wiiupdate_compare_versions(argv[i], argv[i + 1]) < 0));
	return 0;
}
