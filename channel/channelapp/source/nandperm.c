// IOS's NAND permission check, lifted around HBC's own Wii Menu file access.
// playtime.c (the Message Board's play log) and miires.c (the Mii faces) both
// use it, from different threads, so it counts: the first unlock patches, the
// last relock puts the check back.

#include <string.h>

#include <ogcsys.h>
#include <ogc/machine/processor.h>

#include "nandperm.h"

#define MEM2_PROT 0x0D8B420A

// In IOS's FS module: `cmp r3, r1; beq allowed`. The patch makes the branch
// unconditional (beq -> b). The same signature is patched by other homebrew
// (libruntimeiospatch).
static const u8 perm_check[] = { 0x42, 0x8B, 0xD0, 0x01, 0x25, 0x66 };

static mutex_t lock = LWP_MUTEX_NULL;
static int users;
// MEM2_PROT as it was before the first unlock: put back with the check. Left
// at 2 (the PPC may write IOS's memory), it outlived HBC until IOS reloaded,
// and on the bench Wii HBC 1.10.22 with its avatar then hung 20 to 50 s into
// the next run (twice, right after Save Mii faces; never after a power cycle).
static u16 mem2_prot;

bool nand_ahbprot(void) {
	return read32(0x0d800064) == 0xffffffff;
}

// Returns how many sites changed.
static int patch(bool lifted) {
	u8 *p = (u8 *) *(u32 *) 0x80003134, *end = (u8 *) 0x94000000;
	u8 from = lifted ? 0xD0 : 0xE0, to = lifted ? 0xE0 : 0xD0;
	int n = 0;

	if (!nand_ahbprot() || (u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return 0;
	if (lifted)
		mem2_prot = read16(MEM2_PROT);
	write16(MEM2_PROT, 2);
	for (; p < end - sizeof(perm_check); ++p) {
		if (p[0] == perm_check[0] && p[1] == perm_check[1] && p[2] == from &&
				!memcmp(p + 3, perm_check + 3, sizeof(perm_check) - 3)) {
			p[2] = to;
			DCFlushRange((void *) ((u32) p & ~31), 64);
			ICInvalidateRange((void *) ((u32) p & ~31), 64);
			++n;
		}
	}
	// Put back with the check, or now if the unlock found nothing to patch
	// (no nand_relock follows a failed nand_unlock).
	if (!lifted || !n)
		write16(MEM2_PROT, mem2_prot);
	return n;
}

bool nand_unlock(void) {
	bool ok;
	u32 level;

	_CPU_ISR_Disable(level);
	if (lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&lock, false);
	_CPU_ISR_Restore(level);
	LWP_MutexLock(lock);
	ok = users > 0 || patch(true) > 0;
	if (ok)
		++users;
	LWP_MutexUnlock(lock);
	return ok;
}

void nand_relock(void) {
	LWP_MutexLock(lock);
	if (users > 0 && --users == 0)
		patch(false);
	LWP_MutexUnlock(lock);
}
