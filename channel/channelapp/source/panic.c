#include <string.h>
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <tuxedo/ppc/exception.h>

#include "gfx.h"

#include "panic.h"

// steal from libogc
extern unsigned char console_font_8x16[];

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

static void hex(u32 v, char *p) {
	int i;
	for (i=0; i<8; i++) {
		if ((v>>28) >= 10)
			*p++ = 'A' + (v>>28) - 10;
		else
			*p++ = '0' + (v>>28);
		v <<= 4;
	}
}

#define		HW_REG_BASE		0xd800000
#define		HW_RESETS		(HW_REG_BASE + 0x194)

// written to be as generic and reliable as possible
void _panic(u32 file, u32 line)
{
	u32 level;
	u32 fbr;
	int count = 30;
	int x,y;
	u32 bcolor = BLACK;
	u16 vtr;
	int lines;
	char guru[] = "Guru Meditation #00000000.00000000";
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

	for(y=(lines-1); y>=116; y--)
		for(x=0; x<320; x++)
			fb[y*320+x] = fb[(y-116)*320+x];
	for(y=0; y<116; y++)
		for(x=0; x<320; x++)
			fb[y*320+x] = BLACK;

	hex(file, &guru[17]);
	hex(line, &guru[26]);

	// wait for reset button
	while(read32(0xcc003000)&0x10000) {
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

			count = 0;
		}
		
		// hacky waitvsync
		while(read16(0xc00202c) >= 200);
		while(read16(0xc00202c) <  200);
		count++;
	}

	// needs AHBPROT, is evil. so sue me.
	write32(HW_RESETS, read32(HW_RESETS)&(~1));
	while(1);
}

// ---- The crash screen for an exception in HBC itself -----------------------
//
// libogc 3's own screen (default_panic.c) draws on a fixed XFB at 0x81700000,
// "above ArenaHi" for a normal app. HBC's image ends at 0x81777900 and its
// ArenaHi is 0x817FEFF0 (newrvl.ld), so that XFB lies over HBC's own .bss and
// stacks, among them the exception stack the handler is running on: the
// clearing of the XFB wiped the handler's own frames and the text never
// appeared. This is the same screen on the framebuffer HBC already shows.

extern void __console_init(void *destbuffer, int xstart, int ystart, int xres, int yres, int stride);
extern void __reload(void);
// Sets the VI registers at once; VIDEO_SetNextFramebuffer waits for a
// retrace interrupt, and there is none in an exception.
extern void VIDEO_SetFramebuffer(void *fb);
extern void udelay(int us);
#define PANIC_RELOAD_S 20

static const char *const exc_names[] = {
	[PPC_EXCPT_RESET] = "System Reset", [PPC_EXCPT_MCHK] = "Machine Check",
	[PPC_EXCPT_DSI] = "DSI", [PPC_EXCPT_ISI] = "ISI", [PPC_EXCPT_IRQ] = "Interrupt",
	[PPC_EXCPT_ALIGN] = "Alignment", [PPC_EXCPT_UNDEF] = "Program",
	[PPC_EXCPT_FPU] = "Floating Point", [PPC_EXCPT_DECR] = "Decrementer",
	[PPC_EXCPT_SYSCALL] = "System Call", [PPC_EXCPT_TRACE] = "Trace",
	[PPC_EXCPT_PM] = "Performance", [PPC_EXCPT_BKPT] = "IABR",
};

static bool in_mem1(u32 p) {
	return p >= 0x80000000 && p < 0x81800000 && !(p & 3);
}

static void hbc_exception_screen(unsigned exid, PPCContext *ctx)
{
	static int entered;
	GXRModeObj *rm = gfx_video_mode();
	void *fb = gfx_xfb();
	u32 w, h, bh;
	int timer = PANIC_RELOAD_S * 50;
	u32 i, sp;

	// a fault while drawing this screen: reset like _panic
	if (entered++)
		for (write32(HW_RESETS, read32(HW_RESETS) & ~1);;);
	// before the video is up there is nothing to draw on
	if (!rm || !fb)
		__reload();
#if defined(HBC_TEST_CRASH) && HBC_TEST_CRASH == 3
	// Test builds only: the screen itself faults.
	{ vu32 *volatile bad = (vu32 *) 10; *bad = 0; }
#endif

	w = rm->fbWidth;
	h = rm->xfbHeight;
	bh = h > 500 ? 96 : 48;
	GX_AbortFrame();
	VIDEO_ClearFrameBuffer(rm, fb, COLOR_MAROON);
	__console_init(fb, 48, bh, w - 96, h - 2 * bh, 2 * w);
	VIDEO_SetFramebuffer(fb);

	kprintf("Exception (%s) occurred!\n", exid < sizeof exc_names / sizeof *exc_names && exc_names[exid] ? exc_names[exid] : "?");
	for (i = 0; i < 8; i++)
		kprintf("GPR%02d %08X GPR%02d %08X GPR%02d %08X GPR%02d %08X\n",
				i, ctx->gpr[i], 8 + i, ctx->gpr[8 + i], 16 + i, ctx->gpr[16 + i], 24 + i, ctx->gpr[24 + i]);
	kprintf("   PC %08X    LR %08X   MSR %08X\n", ctx->pc, ctx->lr, ctx->msr);
	kprintf("   CR %08X   CTR %08X   XER %08X\n", ctx->cr, ctx->ctr, ctx->xer);
	if (exid == PPC_EXCPT_DSI)
		kprintf("  DAR %08X DSISR %08X\n", mfspr(DAR), mfspr(DSISR));

	kprintf("\nSTACK DUMP:\n%08X", ctx->pc);
	for (sp = ctx->gpr[1], i = 1; i < 10 && in_mem1(sp); i++, sp = *(u32 *)sp)
		kprintf(" --> %08X", ((u32 *)sp)[1]);
	kprintf("\n\nPress RESET, or Z on a GameCube Controller, to reload\n\n");

	PAD_Init();
	for (;;) {
		udelay(20000);
		PAD_ScanPads();
		if (!timer || SYS_ResetButtonDown() || (PAD_ButtonsDown(0) & PAD_TRIGGER_Z))
			__reload();
		if (timer > 0 && !(timer % 50))
			kprintf("\x1b[2K\rReloading in %d seconds", timer / 50);
		timer--;
	}
}

void panic_install_exception_screen(void)
{
	PPCExcptCurPanicFn = hbc_exception_screen;
}
