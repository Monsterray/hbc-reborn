#ifndef _UPDATE_H_
#define _UPDATE_H_

#include <gctypes.h>

#include "view.h"

// Starts the check for a newer release (update.c); false if none started.
bool update_signal(void);
// True while the check runs; then *update_available says what it found.
bool update_busy(bool *update_available);
// Offers the update found, and installs it if the user says yes. True when
// the new version is installed: HBC should then start again.
bool update_execute(view *sub_view);

#endif

