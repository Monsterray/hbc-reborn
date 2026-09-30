/* Shared between agent.c and overlay.c. */

#ifndef AGENT_INT_H
#define AGENT_INT_H

#include <gctypes.h>

#include "../hbc_agent.h"

const hbc_agent_config *agent_cfg(void);
u32 agent_uptime_ms(void);

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

/* Keys the PC sent (HBCK): one of "udlrabh", or 0. */
int agent_key_pop(void);
/* The framebuffer size HBCP reports; the overlay sets it from its mode. */
void agent_set_screen_size(u16 w, u16 h);

/* VIDEO_GetCurrentFramebuffer() gives the VI's physical address; this is the
 * same memory, uncached, whatever form the address took. */
static inline void *agent_uncached(void *p) {
	return (void *) (((u32) p & 0x1fffffff) | 0xc0000000);
}

#endif
