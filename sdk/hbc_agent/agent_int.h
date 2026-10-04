/* Shared between agent.c and overlay.c. */

#ifndef AGENT_INT_H
#define AGENT_INT_H

#include <stddef.h>
#include <gctypes.h>

#include "../hbc_agent.h"
#include "ogc_flavor.h"

const hbc_agent_config *agent_cfg(void);
u32 agent_uptime_ms(void);
// Set while the HOME overlay runs: the hang watchdog pauses.
extern volatile bool agent_overlay_open;
// Restarts the watchdog's count, if it is armed (after the overlay).
void hbc_agent_alive_reset(void);

/* The DEV menu's switches. */
extern volatile int hbc_agent_log_muted;
#define agent_log_to_pc (!hbc_agent_log_muted)
extern volatile bool agent_crash_stay;
extern volatile bool agent_listen_enabled;

/* The app's recent stdout and stderr, oldest first. */
const char *agent_log_text(void);

/* App slots beside Exit: 0 left, 1 right ("Shot" unless the app set it). */
const char *agent_slot_label(int slot);
void agent_slot_press(int slot);
/* The slot's menu, if it has one (count 0 otherwise). */
const hbc_agent_item *agent_slot_menu(int slot, const char **title, int *count);

/* Keys the PC sent (HBCK): one of "udlrabh12w", or 0. */
int agent_key_pop(void);
/* The framebuffer size HBCP reports; the overlay sets it from its mode. */
void agent_set_screen_size(u16 w, u16 h);

/* For safety.c: the log ring (no stdout; safe in an exception), a stop
 * recorded like a fatal, the kept log, and the ways out. */
void agent_log_raw(const char *s, u32 len);
void agent_stop_record(u32 kind, u32 code, const char *reason, u32 pc, u32 lr, u32 sp);
void agent_lastlog(u32 why);
void agent_request_exit(void) __attribute__((noreturn));
void agent_power_off(void) __attribute__((noreturn));
extern void __reload(void) __attribute__((noreturn));
/* The overlay is open, or the app holds the hang watchdog (a load). */
bool agent_paused(void);
/* The 1 s monitor thread (the hang watchdog's): starts it if it is not
 * running and returns the bytes that took (its stack), else 0. */
u32 agent_monitor_start(void);
bool agent_monitor_running(void);

/* safety.c */
void safety_init(const hbc_agent_config *cfg);
/* From the agent thread as it wakes, and from the monitor each second. */
void safety_poll(bool from_monitor);
/* The app is stopping (exception, fatal, hang, abort): guards down, and the
 * stack, memory and thread notes into the log. Integer code only. */
void safety_on_death(void);
/* A DSI from the stack guard's breakpoint. */
bool safety_stack_hit(u32 exid, u32 dsisr, u32 dar);
/* HBCS: ,"safety":{...} */
s32 safety_json(char *buf, size_t size);

/* info.c: DEV > Info's pages (rmode NULL for the preferred mode), and the
 * same as JSON for HBCH. One caller at a time: busy returns false / -EBUSY. */
struct _gx_rmodeobj;
bool agent_info_gather(void *pages, const struct _gx_rmodeobj *rmode, u32 mask);
s32 agent_info_json(char *buf, size_t size, u32 mask);

/* alloc_wrap.c, linked only with -Wl,--wrap=malloc,... */
typedef struct {
	u32 count, last_size, last_from;
} agent_alloc_stats;

/* VIDEO_GetCurrentFramebuffer() gives the VI's physical address; this is the
 * same memory, uncached, whatever form the address took. */
static inline void *agent_uncached(void *p) {
	return (void *) (((u32) p & 0x1fffffff) | 0xc0000000);
}

#endif
