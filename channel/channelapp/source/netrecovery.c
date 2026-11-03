#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "netrecovery.h"

static bool requested, cleaning;
static u64 cleanup_started;
static u32 cleanup_timeout_ms = HBC_NET_CLEANUP_TIMEOUT_MS;

void netrecovery_reset(void) {
	u32 level;
	_CPU_ISR_Disable(level);
	requested = cleaning = false;
	cleanup_started = 0;
	cleanup_timeout_ms = HBC_NET_CLEANUP_TIMEOUT_MS;
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

static void cleanup_begin(u32 timeout_ms) {
	u32 level;
	_CPU_ISR_Disable(level);
	cleanup_started = gettime();
	cleanup_timeout_ms = timeout_ms;
	cleaning = true;
	_CPU_ISR_Restore(level);
}

void netrecovery_cleanup_begin(void) {
	cleanup_begin(HBC_NET_CLEANUP_TIMEOUT_MS);
}

void netrecovery_cleanup_begin_after_start(void) {
	cleanup_begin(HBC_NET_CLEANUP_AFTER_START_TIMEOUT_MS);
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
		ticks_to_millisecs(gettime() - cleanup_started) >= cleanup_timeout_ms;
	if (reload)
		requested = false;
	_CPU_ISR_Restore(level);
	return reload;
}
