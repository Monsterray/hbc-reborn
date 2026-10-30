#ifndef _ESSIG_H_
#define _ESSIG_H_

#include <gctypes.h>

// IOS's signature check for tickets and TMDs, lifted only while HBC
// installs its own update (update.c): the release WAD is fakesigned, as
// every WAD of this channel is. Needs AHBPROT, like nandperm.h.

// The number of check sites patched (0 when there is no AHBPROT or no
// site, as in Dolphin, whose ES is not IOS code). Match a non-zero result
// with es_sigcheck_restore().
int es_sigcheck_lift(void);
void es_sigcheck_restore(void);

#endif
