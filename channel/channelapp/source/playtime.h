#ifndef _PLAYTIME_H_
#define _PLAYTIME_H_

#include <gctypes.h>

typedef union {
	struct {
		u32 checksum;
		u16 name[0x28];
		u32 _pad1;
		u64 ticks_boot;
		u64 ticks_last;
		u32 title_id;
		u32 _pad2[5];
	};
	struct {
		u32 _checksum;
		u32 data[0x1f];
	};
} __attribute__((packed)) playtime_t;

// At startup, on a thread of its own: spoils the Wii Menu's play record
// (play_rec.dat, about 120 ms of NAND write), and logs the app HBC launched
// last, if it came back through HBC's reload stub. `started` is when HBC
// started (gettime()).
void playtime_destroy(u64 started);
// Blocks until that thread is done.
void playtime_wait(void);
// Leaving HBC: logs HBC's own session to the Message Board, and when an app
// is being launched (app_name not NULL), remembers it for the next start.
// Call before reloading IOS.
void playtime_leave(const char *app_name, const char *app_dir);

#endif
