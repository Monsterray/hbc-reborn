// Test app for the HBC network log: prints its arguments to the PC and exits.

#include <stdio.h>
#include <stdlib.h>

#include "hbc_netlog.h"

int main(int argc, char **argv) {
	int i;

	if (hbc_netlog_init() < 0)
		return 1;

	printf("netlog_app: argc=%d\n", argc);
	for (i = 0; i < argc; ++i)
		printf("netlog_app: argv[%d]=%s\n", i, argv[i]);
	fprintf(stderr, "netlog_app: done\n");

	exit(0);
}
