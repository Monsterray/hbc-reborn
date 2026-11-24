// sdk/wiiupdate/verify.c on the host: checks a WAD's detached signature.
//   verify_test KEY SIG WAD     KEY is the 65-byte point, SIG the 64 bytes
// Prints "result=N" (0 for a good signature).

#include <stdio.h>
#include <stdlib.h>

#include "wiiupdate.h"

static unsigned char *slurp(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	unsigned char *p;

	if (!f)
		exit(2);
	fseek(f, 0, SEEK_END);
	*n = ftell(f);
	fseek(f, 0, SEEK_SET);
	p = malloc(*n + 1);
	if (fread(p, 1, *n, f) != *n)
		exit(2);
	fclose(f);
	return p;
}

int main(int argc, char **argv) {
	size_t kn, sn, wn;
	unsigned char *key, *sig, *wad;

	if (argc != 4)
		return 2;
	key = slurp(argv[1], &kn);
	sig = slurp(argv[2], &sn);
	wad = slurp(argv[3], &wn);
	if (kn != WIIUPDATE_KEY_SIZE)
		return 2;
	printf("result=%d\n", wiiupdate_verify(key, sig, sn, wad, (u32) wn));
	return 0;
}
