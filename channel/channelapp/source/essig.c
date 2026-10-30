// IOS's signature check, lifted around HBC's update install (essig.h).
//
// ES compares the hash a signature decrypts to with the blob's own using
// strncmp, the "trucha" bug, and two IOS routines return its result as an
// error when they differ: `movs r0, #7` before each. Patching that 7 to 0
// makes them report a match, so ES takes the fakesigned ticket and TMD of
// a release WAD; the content hashes in the TMD are still checked as each
// content is added. The same two sites are patched by other homebrew
// (libruntimeiospatch). Only the bytes changed here are put back, so a
// patch someone else made stays.

#include <string.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>

#include "nandperm.h"
#include "essig.h"

#define MEM2_PROT 0x0D8B420A
#define MAX_SITES 8

static const u8 sites[][4] = {
	{ 0x20, 0x07, 0x23, 0xA2 },	// movs r0, #7; movs r3, #0xA2
	{ 0x20, 0x07, 0x4B, 0x0B },	// movs r0, #7; ldr r3, [pc, #44]
};

static u8 *patched[MAX_SITES];
static int count;
static u16 mem2_prot;

static void poke(u8 *p, u8 v) {
	p[1] = v;
	DCFlushRange((void *) ((u32) p & ~31), 64);
	ICInvalidateRange((void *) ((u32) p & ~31), 64);
}

int es_sigcheck_lift(void) {
	u8 *p = (u8 *) *(u32 *) 0x80003134, *end = (u8 *) 0x94000000;
	size_t i;

	if (count || !nand_ahbprot() || (u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return count;
	mem2_prot = read16(MEM2_PROT);
	write16(MEM2_PROT, 2);
	for (; p < end - 4 && count < MAX_SITES; p += 2) {
		for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
			if (!memcmp(p, sites[i], 4)) {
				poke(p, 0x00);
				patched[count++] = p;
				break;
			}
	}
	if (!count)
		write16(MEM2_PROT, mem2_prot);
	return count;
}

void es_sigcheck_restore(void) {
	int i;

	if (!count)
		return;
	for (i = 0; i < count; ++i)
		poke(patched[i], 0x07);
	count = 0;
	write16(MEM2_PROT, mem2_prot);
}
