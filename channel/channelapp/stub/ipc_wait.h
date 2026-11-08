// What the stub's IPC wait does on each look at the control register, as a pure
// function (tests/host/panic_host/test.c runs it on the PC). The stub used to spin
// with no limit: with a request of the crashed app still in flight, IOS's late
// reply sat in ARMMSG (Y1), IOS then acked nothing more, and the stub waited
// for an ack that could not come (sdk/hbc_agent/way_out.c has the same logic).
//
// Needs u32 (gctypes.h) before it.

#ifndef IPC_WAIT_H
#define IPC_WAIT_H

// PPCCTRL: X1 1, Y2 2, Y1 4, X2 8, IY1 0x10, IY2 0x20.
#define IPC_ACK 0x22     // Y2 and IY2: IOS acks the request
#define IPC_REPLY 0x14   // Y1 and IY1: a reply waits in ARMMSG

enum { IPC_WAIT_AGAIN, IPC_WAIT_DONE, IPC_WAIT_TAKE_REPLY, IPC_WAIT_TIMEOUT };

// ctrl: PPCCTRL as just read; want: IPC_ACK or IPC_REPLY; elapsed, limit:
// time base ticks (wrap-safe unsigned difference).
static inline int ipc_wait_step(u32 ctrl, u32 want, u32 elapsed, u32 limit) {
	if ((ctrl & want) == want)
		return IPC_WAIT_DONE;
	// Waiting for an ack, but a reply is there: take it, or IOS acks nothing.
	if (want == IPC_ACK && (ctrl & IPC_REPLY) == IPC_REPLY)
		return IPC_WAIT_TAKE_REPLY;
	if (elapsed > limit)
		return IPC_WAIT_TIMEOUT;
	return IPC_WAIT_AGAIN;
}

#endif
