#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "netrecovery.h"

static bool requested, cleaning;
static u64 cleanup_started;

void netrecovery_reset(void) {
	u32 level;
	_CPU_ISR_Disable(level);
	requested = cleaning = false;
	cleanup_started = 0;
	_CPU_ISR_Restore(level);
}

void netrecovery_request(void) {
	u32 level;
	_CPU_ISR_Disable(level);
	requested = true;
	_CPU_ISR_Restore(level);
}

void netrecovery_ready(void) {
	u32 level;
	_CPU_ISR_Disable(level);
	requested = false;
	_CPU_ISR_Restore(level);
}

void netrecovery_cleanup_begin(void) {
	u32 level;
	_CPU_ISR_Disable(level);
	cleanup_started = gettime();
	cleaning = true;
	_CPU_ISR_Restore(level);
}

void netrecovery_cleanup_complete(void) {
	u32 level;
	_CPU_ISR_Disable(level);
	cleaning = false;
	_CPU_ISR_Restore(level);
}

bool netrecovery_take_reload(void) {
	u32 level;
	bool reload;
	_CPU_ISR_Disable(level);
	reload = requested && cleaning &&
		ticks_to_millisecs(gettime() - cleanup_started) >= HBC_NET_CLEANUP_TIMEOUT_MS;
	if (reload)
		requested = false;
	_CPU_ISR_Restore(level);
	return reload;
}
