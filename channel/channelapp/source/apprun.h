#ifndef _APPRUN_H_
#define _APPRUN_H_

#include <stdbool.h>

typedef enum {
	APP_RUN_NEW = 0,
	APP_RUN_UNCHANGED,
	APP_RUN_UPDATED
} app_run_state;

// The marker beside the executable stores the last run's file stamps.
// Legacy markers are upgraded on first scan, preserving cleared badges.
app_run_state app_run_status(const char *boot_path);
bool app_run_record(const char *boot_path);

#endif
