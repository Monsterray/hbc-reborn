#ifndef HBC_NETRECOVERY_H
#define HBC_NETRECOVERY_H

#include <stdbool.h>

// libogc's net_deinit() waits for an IOS callback that may never come. The
// TCP worker reports each cleanup here without taking its own mutex, so the
// menu can see one that has hung and reload the channel with a fresh IOS.
#define HBC_NET_CLEANUP_TIMEOUT_MS 5000u

void netrecovery_reset(void);
// A restart or a failure is about to clean up: watch that cleanup.
void netrecovery_request(void);
// The listener is up again: forget any earlier request.
void netrecovery_ready(void);
void netrecovery_cleanup_begin(void);
// The same, for a cleanup that has to wait out an IOS step a cancelled start
// left in flight (net_deinit() waits for it): such a step can legitimately
// take as long as a start (HBC_NET_INIT_TIMEOUT_MS, 30 s), so a reload before
// that would restart HBC for a slow IOS, not a hung one.
#define HBC_NET_CLEANUP_AFTER_START_TIMEOUT_MS 30000u
void netrecovery_cleanup_begin_after_start(void);
void netrecovery_cleanup_complete(void);
// True once when a watched cleanup has not returned for the timeout.
bool netrecovery_take_reload(void);

#endif
