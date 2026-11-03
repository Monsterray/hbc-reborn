#ifndef _APPRUN_H_
#define _APPRUN_H_

#include <stdbool.h>

typedef enum {
	APP_RUN_NEW = 0,
	APP_RUN_UNCHANGED,
	APP_RUN_UPDATED
} app_run_state;

// Options > App badges. One text file per device, <root>/hbc/apps_seen.txt
// (root is "sd:", "usb:" and so on), holds that device's apps: a line per app
// folder under <root>/apps, the folder's name and the size and modified time
// of boot.dol, boot.elf, meta.xml and icon.png as of its last run from HBC.
// A missing, unreadable or damaged file or line means "never run". Nothing
// here writes to NAND, and a device that cannot be written just forgets.

// A device scan: begin reads the device's file once, status judges each app
// folder against it, end forgets entries for folders the scan did not find
// and writes the file once if that changed anything. Pass complete = false
// when the scan did not see every folder (device lost, list full): nothing
// is pruned or written then.
void app_run_begin(const char *root);
app_run_state app_run_status(const char *name);
void app_run_end(bool complete);

// One app outside a scan (a newly installed folder).
app_run_state app_run_status_one(const char *root, const char *name);

// A launch through HBC's own path: boot_path is <root>/apps/<name>/boot.dol
// or boot.elf. Stores the app's current stamps, so its badge clears.
bool app_run_record(const char *boot_path);

#endif
