#include <string.h>
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <tuxedo/ppc/exception.h>
#include <tuxedo/thread.h>
#include <ogc/lwp_watchdog.h>

#include "gfx.h"
#include "devnet.h"
#include "../config.h"
#include "../../../sdk/hbc_agent.h"
#include "panic_core.h"

#include "panic.h"

// steal from libogc
extern unsigned char console_font_8x16[];

// The return stub, as main.c installs it.
extern const u8 stub_bin[];
extern const u32 stub_bin_size;

#define BLACK 0x00800080
#define RED 0x4C544CFF

static vu32 *fb;

static void pchar(u32 cx, u32 cy, char c)
{
	int x,y;
	unsigned char *p = &console_font_8x16[16*c];

	for (y=0; y<16; y++) {
		char v = *p++;
		for (x=0; x<4; x++) {
			u32 c = RED;
			switch (v&0xc0) {
			case 0x00:
				c = BLACK;
				break;
			case 0x40:
				c = (RED & 0x0000FF00) | (BLACK & 0xFFFF00FF);
				break;
			case 0x80:
				c = (RED & 0xFFFF00FF) | (BLACK & 0x0000FF00);
				break;
			case 0xc0:
				c = RED;
				break;
			}
			fb[320*(cy+y)+cx+x] = c;
			v<<=2;
		}
	}
}

static void putsc(u32 y, char *s) {
	u32 x = (320-strlen(s)*4)/2;
	while(*s) {
		pchar(x, y, *s++);
		x += 4;
	}
}

#define		HW_REG_BASE		0xd800000
#define		HW_RESETS		(HW_REG_BASE + 0x194)
#define		HW_AHBPROT		(HW_REG_BASE + 0x64)
#define		HW_VERSION		(HW_REG_BASE + 0x5a0)

// ---- Leaving the crash screen --------------------------------------------------
//
// Never __reload() or SYS_ResetSystem() from here: libogc's __reload() calls
// SYS_ResetSystem() when no stub is installed, and that runs IOS and thread
// code from inside an exception. A crash before main() has installed HBC's
// stub would end there. The stub is HBC's own code with no libogc.

// The hardware reset writes a register that needs AHBPROT, and a vWii has a
// different one: only where it is known to work.
static int panic_hw_reset_ok(void)
{
	return read32(HW_AHBPROT) == 0xffffffff && (read32(HW_VERSION) >> 16) != 0xcafe;
}

static void panic_hw_reset(void)
{
	if (panic_hw_reset_ok())
		write32(HW_RESETS, read32(HW_RESETS) & ~1);
}

static void __attribute__((noreturn)) panic_spin(void)
{
	for (;;);
}

// Where the stub goes: 0 for the title it already has (HBC), else the System
// Menu when HBC crashes on every start (panic_crash_loop).
static u64 leave_title;

// Start the return stub, installing this build's own if none is there.
static void __attribute__((noreturn)) panic_leave(void)
{
	volatile u8 *dst = (volatile u8 *) 0x80001800;
	u32 i;

	mtmsr((mfmsr() & ~MSR_EE) | MSR_FP | MSR_RI);
	if (*(volatile u32 *) 0x80001804 != 0x53545542 || *(volatile u32 *) 0x80001808 != 0x48415858) {
		for (i = 0; i < stub_bin_size; i++)
			dst[i] = stub_bin[i];
		*STUB_ADDR_MAGIC = STUB_MAGIC;
		*STUB_ADDR_TITLE = MY_TITLEID;
	}
	if (leave_title) {
		*STUB_ADDR_MAGIC = STUB_MAGIC;
		*STUB_ADDR_TITLE = leave_title;
	}
	DCFlushRange((void *) STUB_ADDR_MAGIC, 16);
	DCFlushRange((void *) 0x80001800, stub_bin_size);
	ICInvalidateRange((void *) 0x80001800, stub_bin_size);
	((void (*)(void)) 0x80001800)();
	// the stub came back: nothing else is left
	panic_hw_reset();
	panic_spin();
}

// ---- _panic: a failed allocation -----------------------------------------------

#define PANIC_RELOAD_S 20
#define TB_PER_S 60750000ull

static void panic_vsync(void)
{
	// hacky waitvsync, bounded: a VI that was never set up never moves
	u32 n;

	for (n = 0; n < 2000000 && read16(0xc00202c) >= 200; n++);
	for (n = 0; n < 2000000 && read16(0xc00202c) <  200; n++);
}

// written to be as generic and reliable as possible
void _panic(u32 file, u32 line)
{
	u32 level;
	u32 fbr;
	int count = 30;
	int x,y;
	u32 bcolor = BLACK;
	u16 vtr;
	int lines, draw;
	char guru[] = "Guru Meditation #00000000.00000000";
	char left[32];
	u64 end = gettime() + PANIC_RELOAD_S * TB_PER_S;
	u32 shown = 0;
	_CPU_ISR_Disable(level);
	(void) level; // panic never restores interrupts

	fbr = read32(0xc00201c) & 0x1fffffff;
	if (fbr&0x10000000)
		fbr <<= 5;

	fb = (vu32*)(fbr | 0xC0000000);

	vtr = read16(0xc002000);
	if ((vtr & 0xf) > 10)
		lines = vtr >> 4;
	else
		lines = 2 * (vtr >> 4);

	// A VI that was never set up reads as 0: drawing at address 0 would
	// overwrite low memory, with the stub in it.
	fbr &= 0x1fffffff;
	draw = lines >= 240 && lines <= 600 &&
		((fbr >= 0x00100000 && fbr < 0x01800000 - 640 * 2 * 600) ||
		 (fbr >= 0x10000000 && fbr < 0x14000000 - 640 * 2 * 600));

	if (draw) {
		for(y=(lines-1); y>=116; y--)
			for(x=0; x<320; x++)
				fb[y*320+x] = fb[(y-116)*320+x];
		for(y=0; y<116; y++)
			for(x=0; x<320; x++)
				fb[y*320+x] = BLACK;
	}

	panic_hex(guru + 17, file);
	panic_hex(guru + 26, line);

	// wait for reset button, for at most PANIC_RELOAD_S seconds
	while(read32(0xcc003000)&0x10000) {
		u64 now = gettime();
		u32 secs;

		if (now >= end)
			break;
		secs = (u32) ((end - now) / TB_PER_S) + 1;

		if (draw) {
			// blink
			if (count >= 25) {
				if (bcolor == RED)
					bcolor = BLACK;
				else
					bcolor = RED;

				for(y=28; y<34; y++)
					for(x=14; x<306; x++)
						fb[y*320+x] = bcolor;
				for(y=34; y<84; y++) {
					for(x=14; x<17; x++)
						fb[y*320+x] = bcolor;
					for(x=303; x<306; x++)
						fb[y*320+x] = bcolor;
				}
				for(y=84; y<90; y++)
					for(x=14; x<306; x++)
						fb[y*320+x] = bcolor;

				// with the box: a copy the GPU had queued can land on this
				// framebuffer after the first drawing and wipe the words
				putsc(42, "Software Failure.   Press reset button to continue.");
				putsc(62, guru);
				shown = 0;

				count = 0;
			}
			if (shown != secs) {
				char *d = panic_str(left, "Reloading in ");
				d = panic_dec(d, secs);
				d = panic_str(d, " s");
				while (d < left + 18)
					*d++ = ' ';
				*d = 0;
				putsc(96, left);
				shown = secs;
			}
		}

		panic_vsync();
		count++;
	}

	// The reset button: the hardware reset where it works, else the stub.
	if (!(read32(0xcc003000)&0x10000))
		panic_hw_reset();
	panic_leave();
}

// ---- The crash screen for an exception in HBC itself -----------------------
//
// libogc 3's own screen (default_panic.c) draws on a fixed XFB at 0x81700000,
// "above ArenaHi" for a normal app. HBC's image ends at 0x81777900 and its
// ArenaHi is 0x817FEFF0 (newrvl.ld), so that XFB lies over HBC's own .bss and
// stacks, among them the exception stack the handler is running on: the
// clearing of the XFB wiped the handler's own frames and the text never
// appeared. This is the same screen on the framebuffer HBC already shows.
//
// It uses no console, no printf and no heap: __console_init takes newlib's
// stream lock and may allocate, and an exception can come from inside either.

// Sets the VI registers at once; VIDEO_SetNextFramebuffer waits for a
// retrace interrupt, and there is none in an exception.
extern void VIDEO_SetFramebuffer(void *fb);

static const char *const exc_names[] = {
	[PPC_EXCPT_RESET] = "System Reset", [PPC_EXCPT_MCHK] = "Machine Check",
	[PPC_EXCPT_DSI] = "DSI", [PPC_EXCPT_ISI] = "ISI", [PPC_EXCPT_IRQ] = "Interrupt",
	[PPC_EXCPT_ALIGN] = "Alignment", [PPC_EXCPT_UNDEF] = "Program",
	[PPC_EXCPT_FPU] = "Floating Point", [PPC_EXCPT_DECR] = "Decrementer",
	[PPC_EXCPT_SYSCALL] = "System Call", [PPC_EXCPT_TRACE] = "Trace",
	[PPC_EXCPT_PM] = "Performance", [PPC_EXCPT_BKPT] = "IABR",
};

static u64 start_time;

static u32 rd32(u32 addr) {
	return *(volatile u32 *) addr;
}

#define ROWS 22
#define COLS 128
#define MAX_FRAMES 18
static char rows[ROWS][COLS];

// YUY2: Y0 Cb Y1 Cr
static u32 yuy2_pair(int on0, int on1, u32 bg) {
	u32 y0 = on0 ? 235 : bg >> 24, y1 = on1 ? 235 : (bg >> 8) & 0xff;
	u32 cb = on0 || on1 ? 128 : (bg >> 16) & 0xff, cr = on0 || on1 ? 128 : bg & 0xff;

	return (y0 << 24) | (cb << 16) | (y1 << 8) | cr;
}

// Row r of text, 2 columns in, on a framebuffer with `stride` words a line.
static void draw_row(vu32 *xfb, u32 stride, u32 top, u32 h, u32 r, u32 bg) {
	const char *s = rows[r];
	u32 y0 = top + r * 16, c, y, x;

	if (y0 + 16 > h)
		return;
	for (c = 0; c < 76; c++) {
		unsigned char ch = *s ? (unsigned char) *s++ : ' ';
		const unsigned char *p = &console_font_8x16[16 * ch];

		for (y = 0; y < 16; y++) {
			unsigned char v = *p++;

			for (x = 0; x < 4; x++, v <<= 2)
				xfb[(y0 + y) * stride + 8 + c * 4 + x] = yuy2_pair(v & 0x80, v & 0x40, bg);
		}
	}
}

static char *put_gpr(char *d, u32 n, u32 v) {
	*d++ = 'G'; *d++ = 'P'; *d++ = 'R';
	*d++ = '0' + n / 10;
	*d++ = '0' + n % 10;
	*d++ = ' ';
	d = panic_hex(d, v);
	*d++ = ' ';
	return d;
}

static char *put_reg(char *d, const char *name, u32 v) {
	d = panic_str(d, name);
	*d++ = ' ';
	d = panic_hex(d, v);
	return panic_str(d, "   ");
}

static void hbc_exception_screen(unsigned exid, PPCContext *ctx)
{
	static int entered;
	GXRModeObj *rm = gfx_video_mode();
	void *xfb = gfx_xfb();
	u32 frames[MAX_FRAMES];
	u32 nframes, i, w, h, top, stride, bg;
	u32 dar = 0, dsisr = 0;
	char cause[PANIC_CAUSE_MAX];
	char *d;
	panic_info info;
	u64 end;
	int shown = -1;

	// A fault while recording or drawing: leave by the stub. A third
	// entry means the stub's own start faulted: hardware reset, or spin.
	if (entered++) {
		if (entered == 2)
			panic_leave();
		panic_hw_reset();
		panic_spin();
	}

	if (exid == PPC_EXCPT_DSI || exid == PPC_EXCPT_ALIGN) {
		dar = mfspr(DAR);
		dsisr = mfspr(DSISR);
	}
	frames[0] = ctx->lr;
	nframes = 1 + panic_backtrace(ctx->gpr[1], frames + 1, MAX_FRAMES - 1, rd32);
	panic_decode(exid, ctx->msr, dsisr, cause);

	// What `hbc.py crash` shows after the reload, as for an app's crash.
	info.exid = exid; info.pc = ctx->pc; info.msr = ctx->msr; info.lr = ctx->lr;
	info.cr = ctx->cr; info.ctr = ctx->ctr; info.sp = ctx->gpr[1];
	info.dar = dar; info.dsisr = dsisr;
	info.uptime_ms = (u32) ((gettime() - start_time) / (TB_PER_S / 1000));
	info.thread = (u32) KThreadGetSelf();
	info.app = "HBC " CHANNEL_VERSION_STR;
	info.frames = frames; info.nframes = nframes;
	// HBC crashed soon after its start last time too: the System Menu, not
	// another reload of the same crash. Read before the block is replaced.
	if (panic_crash_loop(panic_block_valid((hbc_crash_block *) HBC_CRASH_ADDR) ?
			(hbc_crash_block *) HBC_CRASH_ADDR : devnet_crash_taken(), info.uptime_ms))
		leave_title = 0x0000000100000002ull;
	panic_fill_block((hbc_crash_block *) HBC_CRASH_ADDR, &info, cause);
	DCFlushRange((void *) HBC_CRASH_ADDR, sizeof(hbc_crash_block));

	// no sound from a half-dead program: stop the audio DMA (AUDIO_StopDMA)
	write16(0xc005036, read16(0xc005036) & ~0x8000);

	// before the video is up (or after it was blanked) there is nothing to draw on
	if (!rm || !xfb)
		panic_leave();
#if defined(HBC_TEST_CRASH) && HBC_TEST_CRASH == 3
	// Test builds only: the screen itself faults.
	{ vu32 *volatile bad = (vu32 *) 10; *bad = 0; }
#endif

	w = rm->fbWidth;
	h = rm->xfbHeight;
	top = h > 500 ? 64 : 32;
	stride = w / 2;
	bg = COLOR_MAROON;

	// The text, a row each.
	for (i = 0; i < ROWS; i++)
		rows[i][0] = 0;
	d = panic_str(rows[0], "Exception (");
	d = panic_str(d, exid < sizeof exc_names / sizeof *exc_names && exc_names[exid] ? exc_names[exid] : "?");
	d = panic_str(d, ") occurred!");
	*d = 0;
	d = panic_str(rows[1], info.app);
	d = panic_str(d, "   thread ");
	d = panic_hex(d, info.thread);
	d = panic_str(d, "   up ");
	d = panic_dec(d, info.uptime_ms / 1000);
	d = panic_str(d, " s");
	*d = 0;
	if (cause[0]) {
		d = panic_str(rows[2], "Cause: ");
		d = panic_str(d, cause);
		*d = 0;
	}
	for (i = 0; i < 8; i++) {
		d = put_gpr(rows[3 + i], i, ctx->gpr[i]);
		d = put_gpr(d, 8 + i, ctx->gpr[8 + i]);
		d = put_gpr(d, 16 + i, ctx->gpr[16 + i]);
		d = put_gpr(d, 24 + i, ctx->gpr[24 + i]);
		*d = 0;
	}
	d = put_reg(rows[11], "   PC", ctx->pc);
	d = put_reg(d, " LR", ctx->lr);
	d = put_reg(d, "MSR", ctx->msr);
	*d = 0;
	d = put_reg(rows[12], "   CR", ctx->cr);
	d = put_reg(d, "CTR", ctx->ctr);
	d = put_reg(d, "XER", ctx->xer);
	*d = 0;
	if (exid == PPC_EXCPT_DSI || exid == PPC_EXCPT_ALIGN) {
		d = put_reg(rows[13], "  DAR", dar);
		d = put_reg(d, "DSISR", dsisr);
		*d = 0;
	}
	d = panic_str(rows[15], "STACK (LR, then the callers):");
	*d = 0;
	for (i = 0; i < nframes; i++) {
		d = rows[16 + i / 6];
		d += strlen(d);
		d = panic_hex(d, frames[i]);
		*d++ = ' ';
		*d = 0;
	}
	d = panic_str(rows[19], leave_title ?
		"HBC crashed at its last start too. Press RESET, or Z, for the Wii Menu" :
		"Press RESET, or Z on a GameCube Controller, to reload");
	*d = 0;

	GX_AbortFrame();
	VIDEO_ClearFrameBuffer(rm, xfb, bg);
	VIDEO_SetFramebuffer(xfb);

	PAD_Init();
	end = gettime() + PANIC_RELOAD_S * TB_PER_S;
	for (;;) {
		u64 now = gettime(), next = now + TB_PER_S / 50;
		int secs;

		PAD_ScanPads();
		if (now >= end || SYS_ResetButtonDown() || (PAD_ButtonsDown(0) & PAD_TRIGGER_Z))
			panic_leave();
		secs = (int) ((end - now) / TB_PER_S) + 1;
		if (secs != shown) {
			// redrawn every second: a copy the GPU had queued can land on the
			// framebuffer after the first drawing
			d = panic_str(rows[21], leave_title ? "Wii Menu in " : "Reloading in ");
			d = panic_dec(d, secs);
			d = panic_str(d, " seconds");
			*d = 0;
			for (i = 0; i < ROWS; i++)
				draw_row(xfb, stride, top, h, i, bg);
			shown = secs;
		}
		while (gettime() < next);
	}
}

void panic_install_exception_screen(void)
{
	start_time = gettime();
	PPCExcptCurPanicFn = hbc_exception_screen;
}

// libogc's weak one installs libogc's own screen. A crash before main() gets here.
void __SYS_InstallPanicHandler(void)
{
	start_time = gettime();
	PPCExcptCurPanicFn = hbc_exception_screen;
}
