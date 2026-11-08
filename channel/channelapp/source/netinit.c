#include <errno.h>
#include <unistd.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include "netinit.h"

s32 netinit_wait(u64 started, const volatile bool *stop, const volatile bool *restart) {
	s32 status;

	for (;;) {
		if (*stop || *restart)
			return -ECANCELED;
		if (ticks_to_millisecs(gettime() - started) >= HBC_NET_INIT_TIMEOUT_MS)
			return -ETIMEDOUT;
		status = net_get_status();
		if (status != -EBUSY)
			return status;
		usleep(50 * 1000);
	}
}
