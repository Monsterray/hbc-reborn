// SPDX-FileCopyrightText: 2014 FIX94 (libwupc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Wii U Pro Controller, after FIX94's libwupc (github.com/FIX94/libwupc;
// the controller's protocol documented by TeHaxor69), brought to libogc
// 3.1: wiiuse_register became wiiuse_accept, and a connection is taken
// with bte_listenasync and its second step instead of
// bte_registerdeviceasync, as libwiiinput's build of it does.
//
// The controller pairs with the Wii like a Wii Remote (SYSCONF lists it
// with a name ending "-UC"). As it connects, libogc's wiiuse_accept is
// asked to take it; for a Pro Controller this file (linked with
// -Wl,--wrap=wiiuse_accept) takes the connection itself and reads its
// reports: mode 0x3D, 22 bytes, sticks then buttons laid out like a Classic
// Controller's. The first report gives the sticks' centres.

#include <string.h>

#include <gccore.h>
#include <ogc/conf.h>
#include <ogc/machine/processor.h>
#include <bte/bte.h>
#include <wiiuse/wiiuse.h>

#include "wiiinput.h"

#define CHANS 4
#define NO_CHAN CHANS

typedef struct {
	u32 chan;
	bool calibrated;
	s16 lx_mid, rx_mid, ly_mid, ry_mid;
	struct bte_pcb *sock;
	struct bd_addr bdaddr;
} pro;

static bool inited;
static conf_pads devices;
static pro pros[CONF_PAD_MAX_REGISTERED];
static pro *connected[CHANS];
static volatile u32 used;
static wiiinput_pro_pad pads[CHANS];

// The Wii Remote's player LEDs, one per channel.
static const u8 leds[CHANS] = { 0x10, 0x20, 0x40, 0x80 };

static void set_leds(struct bte_pcb *sock, u8 bits) {
	u8 buf[2] = { 0x11, bits };

	bte_senddata(sock, buf, sizeof(buf));
}

static u16 be16(const u8 *p) {
	return (u16) (p[0] << 8 | p[1]);
}

// Bluetooth's thread, as each report comes in.
static s32 on_data(void *arg, void *buffer, u16 len) {
	pro *p = arg;
	const u8 *b = buffer;
	wiiinput_pro_pad *pad;

	if (len != 22 || b[0] != 0x3d || p->chan >= CHANS)
		return ERR_OK;
	if (!p->calibrated) {
		p->lx_mid = be16(b + 1);
		p->rx_mid = be16(b + 3);
		p->ly_mid = be16(b + 5);
		p->ry_mid = be16(b + 7);
		p->calibrated = true;
	}
	pad = &pads[p->chan];
	pad->lx = be16(b + 1) - p->lx_mid;
	pad->rx = be16(b + 3) - p->rx_mid;
	pad->ly = be16(b + 5) - p->ly_mid;
	pad->ry = be16(b + 7) - p->ry_mid;
	// Active low, in the Classic Controller's order.
	pad->buttons = (u32) (u16) ~be16(b + 9) << 16;
	pad->battery = (~b[11] >> 4) & 7;
	return ERR_OK;
}

static s32 on_connect(void *arg, struct bte_pcb *pcb, u8 err) {
	pro *p = arg;
	u8 mode[3] = { 0x12, 0x00, 0x3d };

	if (used >= CHANS) {
		bte_disconnect(pcb);
		return err;
	}
	p->chan = used;
	p->calibrated = false;
	memset(&pads[p->chan], 0, sizeof(pads[p->chan]));
	set_leds(pcb, leds[p->chan]);
	// Report mode 0x3D, sent continuously.
	bte_senddata(pcb, mode, sizeof(mode));
	connected[used++] = p;
	return err;
}

static s32 on_listen(void *arg, struct bte_pcb *pcb, u8 err) {
	if (err != ERR_OK) {
		bte_disconnect(pcb);
		return err;
	}
	return bte_listenasync_step2(pcb, on_connect);
}

// The controllers after this one move down a channel, and their LEDs too.
static s32 on_disconnect(void *arg, struct bte_pcb *pcb, u8 err) {
	pro *p = arg;
	u32 i;

	if (p->chan >= CHANS)
		return err;
	if (used)
		used--;
	for (i = p->chan; i < CHANS - 1; ++i) {
		connected[i] = connected[i + 1];
		pads[i] = pads[i + 1];
	}
	connected[CHANS - 1] = NULL;
	memset(&pads[CHANS - 1], 0, sizeof(pads[CHANS - 1]));
	for (i = 0; i < CONF_PAD_MAX_REGISTERED; ++i)
		if (pros[i].chan != NO_CHAN && pros[i].chan > p->chan) {
			pros[i].chan--;
			set_leds(pros[i].sock, leds[pros[i].chan]);
		}
	p->chan = NO_CHAN;
	return err;
}

static int take(pro *p, struct bd_addr *bdaddr) {
	p->chan = NO_CHAN;
	p->bdaddr = *bdaddr;
	if (!p->sock && !(p->sock = bte_new()))
		return ERR_OK;
	bte_arg(p->sock, p);
	bte_received(p->sock, on_data);
	bte_disconnected(p->sock, on_disconnect);
	bte_listenasync(p->sock, bdaddr, on_listen);
	return ERR_OK;
}

int __real_wiiuse_accept(struct wiimote_listen_t *wml, struct bd_addr *bdaddr, const u8 *name,
						 struct wiimote_t *(*assign_cb)(wiimote_listen *wml, u8 err));

int __wrap_wiiuse_accept(struct wiimote_listen_t *wml, struct bd_addr *bdaddr, const u8 *name,
						 struct wiimote_t *(*assign_cb)(wiimote_listen *wml, u8 err)) {
	if (inited) {
		// SYSCONF keeps addresses in the opposite byte order.
		const u8 addr[6] = { bdaddr->addr[5], bdaddr->addr[4], bdaddr->addr[3],
							 bdaddr->addr[2], bdaddr->addr[1], bdaddr->addr[0] };
		u32 i;

		for (i = 0; i < devices.num_registered && i < CONF_PAD_MAX_REGISTERED; ++i)
			if (strstr(devices.registered[i].name, "-UC") &&
					!memcmp(devices.registered[i].bdaddr, addr, 6))
				return take(&pros[i], bdaddr);
	}
	return __real_wiiuse_accept(wml, bdaddr, name, assign_cb);
}

void wiiinput_wupc_init(void) {
	u32 i;

	if (inited || CONF_GetPadDevices(&devices) < 0)
		return;
	for (i = 0; i < CHANS; ++i)
		connected[i] = NULL;
	for (i = 0; i < CONF_PAD_MAX_REGISTERED; ++i)
		pros[i].chan = NO_CHAN;
	memset(pads, 0, sizeof(pads));
	used = 0;
	inited = true;
}

void wiiinput_wupc_shutdown(void) {
	u32 i;

	if (!inited)
		return;
	inited = false;
	for (i = 0; i < CONF_PAD_MAX_REGISTERED; ++i)
		if (pros[i].chan != NO_CHAN)
			bte_disconnect(pros[i].sock);
}

bool wiiinput_wupc_pad(int chan, wiiinput_pro_pad *out) {
	u32 level;

	if (chan < 0 || chan >= CHANS || !connected[chan])
		return false;
	_CPU_ISR_Disable(level);
	*out = pads[chan];
	_CPU_ISR_Restore(level);
	return true;
}

void wiiinput_wupc_rumble(int chan, bool on) {
	if (chan < 0 || chan >= CHANS || !connected[chan])
		return;
	set_leds(connected[chan]->sock, leds[chan] | (on ? 1 : 0));
}
