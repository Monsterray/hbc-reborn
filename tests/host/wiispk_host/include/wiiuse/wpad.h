#ifndef HOST_WPAD_H
#define HOST_WPAD_H
#include "../gctypes.h"
typedef u8 ubyte;
struct bd_addr {
	u8 addr[6];
};
struct wiimote_t {
	int unid;
	int state;
	void *cmd_head;
	struct bd_addr bdaddr;
};
typedef void (*cmd_blk_cb)(struct wiimote_t *wm, ubyte *data, u16 len);
#define WPAD_ERR_NONE 0
int wiiuse_write_data(struct wiimote_t *wm, u32 addr, ubyte *data, u8 len, cmd_blk_cb cb);
void wiiuse_status(struct wiimote_t *wm, cmd_blk_cb cb);
s32 WPAD_Rumble(s32 chan, int status);
s32 WPAD_Probe(s32 chan, u32 *type);
s32 WPAD_IsSpeakerEnabled(s32 chan);
#endif
