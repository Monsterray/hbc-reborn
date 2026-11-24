#ifndef _NANDPERM_H_
#define _NANDPERM_H_

#include <gctypes.h>

// IOS's NAND permission check, lifted only around HBC's own reads and writes
// of the Wii Menu's files (the Message Board's play log, the Mii faces).
// Needs AHBPROT (HBC's IOS has it). Calls nest: the check comes back when
// the last nand_unlock() is matched by nand_relock().

bool nand_ahbprot(void);

// IOS's signature check, lifted only around the install of an update
// (update.c), so that ES takes a release WAD's fakesigned ticket and TMD.
// The WAD's own signature (a detached one, checked against a key built into
// HBC) is checked before this. Returns how many places in IOS were patched;
// 0 without AHBPROT or the patch sites, and then nothing needs undoing.
// Otherwise match it with nand_sigcheck_restore(). Shares nand_unlock()'s
// lock and its MEM2_PROT window, so the two can overlap safely.
int nand_sigcheck_lift(void);
void nand_sigcheck_restore(void);

// True when the check is lifted (now or already); false without AHBPROT or
// the patch site. Match every true with nand_relock().
bool nand_unlock(void);
void nand_relock(void);

#endif
