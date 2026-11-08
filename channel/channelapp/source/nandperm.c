// IOS patches HBC makes, all here so that they share one lock and one
// MEM2_PROT window:
//  - IOS's NAND permission check, lifted around HBC's own Wii Menu file
//    access. playtime.c (the Message Board's play log) and miires.c (the Mii
//    faces) both use it, from different threads, so it counts: the first
//    unlock patches, the last relock puts the check back.
//  - IOS's signature check, lifted around the install of an update
//    (update.c), by nand_sigcheck_lift().
// MEM2_PROT is opened for the first patch of either and put back, to what
// it was, when the last one is undone.

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
// MEM2_PROT as it was before the first patch: put back with the last one.
// Left at 2 (the PPC may write IOS's memory), it outlived HBC until IOS
// reloaded, and on the bench Wii HBC 1.10.22 with its avatar then hung 20 to
// 50 s into the next run (twice, right after Save Mii faces; never after a
// power cycle).
static u16 mem2_prot;
// How many patches (a scan, or a lift held) have MEM2_PROT open; under lock.
static int prot_holds;

static void prot_open(void) {
	if (prot_holds++ == 0)
		mem2_prot = read16(MEM2_PROT);
	write16(MEM2_PROT, 2);
}

static void prot_close(void) {
	if (--prot_holds == 0)
		write16(MEM2_PROT, mem2_prot);
}

static void flush(u8 *p) {
	DCFlushRange((void *) ((u32) p & ~31), 64);
	ICInvalidateRange((void *) ((u32) p & ~31), 64);
}

// IOS's FS and ES code is somewhere in MEM2 below IOS's heap; the start of
// the range to search, or NULL when it can not be found.
static u8 *ios_start(void) {
	u8 *p = (u8 *) *(u32 *) 0x80003134;

	if (!nand_ahbprot() || (u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return NULL;
	return p;
}

bool nand_ahbprot(void) {
	return read32(0x0d800064) == 0xffffffff;
}

// Returns how many sites changed. Lifting leaves MEM2_PROT open when it
// found any: the relock opens nothing and closes it.
static int patch(bool lifted) {
	u8 *p = ios_start(), *end = (u8 *) 0x94000000;
	u8 from = lifted ? 0xD0 : 0xE0, to = lifted ? 0xE0 : 0xD0;
	int n = 0;

	if (!p) {
		if (!lifted)
			prot_close();
		return 0;
	}
	if (lifted)
		prot_open();
	for (; p < end - sizeof(perm_check); ++p) {
		if (p[0] == perm_check[0] && p[1] == perm_check[1] && p[2] == from &&
				!memcmp(p + 3, perm_check + 3, sizeof(perm_check) - 3)) {
			p[2] = to;
			flush(p);
			++n;
		}
	}
	// Put back with the check, or now if the unlock found nothing to patch
	// (no nand_relock follows a failed nand_unlock).
	if (!lifted || !n)
		prot_close();
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

// ---- The signature check, for an update's install ----
//
// ES compares the hash a signature decrypts to with the blob's own using
// strncmp, the "trucha" bug, and two IOS routines return its result as an
// error when they differ: `movs r0, #7` before each. Patching that 7 to 0
// makes them report a match, so ES takes the fakesigned ticket and TMD of
// a release WAD; the content hashes in the TMD are still checked as each
// content is added. The same two sites are patched by other homebrew
// (libruntimeiospatch). Only the bytes changed here are put back, so a
// patch someone else made stays.

#define MAX_SIG_SITES 8

static const u8 sig_sites[][4] = {
	{ 0x20, 0x07, 0x23, 0xA2 },	// movs r0, #7; movs r3, #0xA2
	{ 0x20, 0x07, 0x4B, 0x0B },	// movs r0, #7; ldr r3, [pc, #44]
};

static u8 *sig_patched[MAX_SIG_SITES];
static int sig_count;
static int sig_users;

int nand_sigcheck_lift(void) {
	u8 *p, *end = (u8 *) 0x94000000;
	size_t i;
	int n;
	u32 level;

	_CPU_ISR_Disable(level);
	if (lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&lock, false);
	_CPU_ISR_Restore(level);
	LWP_MutexLock(lock);
	if (sig_users == 0 && (p = ios_start())) {
		prot_open();
		for (; p < end - 4 && sig_count < MAX_SIG_SITES; p += 2) {
			for (i = 0; i < sizeof(sig_sites) / sizeof(sig_sites[0]); ++i)
				if (!memcmp(p, sig_sites[i], 4)) {
					p[1] = 0x00;
					flush(p);
					sig_patched[sig_count++] = p;
					break;
				}
		}
		if (!sig_count)
			prot_close();
	}
	if (sig_count)
		++sig_users;
	n = sig_count;
	LWP_MutexUnlock(lock);
	return n;
}

void nand_sigcheck_restore(void) {
	int i;

	LWP_MutexLock(lock);
	if (sig_users > 0 && --sig_users == 0) {
		for (i = 0; i < sig_count; ++i) {
			sig_patched[i][1] = 0x07;
			flush(sig_patched[i]);
		}
		sig_count = 0;
		prot_close();
	}
	LWP_MutexUnlock(lock);
}
