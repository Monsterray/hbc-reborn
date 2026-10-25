#ifndef _NANDPERM_H_
#define _NANDPERM_H_

#include <gctypes.h>

// IOS's NAND permission check, lifted only around HBC's own reads and writes
// of the Wii Menu's files (the Message Board's play log, the Mii faces).
// Needs AHBPROT (HBC's IOS has it). Calls nest: the check comes back when
// the last nand_unlock() is matched by nand_relock().

bool nand_ahbprot(void);

// True when the check is lifted (now or already); false without AHBPROT or
// the patch site. Match every true with nand_relock().
bool nand_unlock(void);
void nand_relock(void);

#endif
