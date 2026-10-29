/* libogc 3.1.0 (tuxedo) switches the FPU lazily: SPRG3 names the thread
 * context that owns the floating-point registers, and the first FP
 * instruction on another thread saves them into that context. A thread
 * that exits while it owns the FPU is never taken off SPRG3, so once
 * LWP_JoinThread has freed its block, that save lands in whatever the
 * heap has since put there. Each thread that uses floats calls this as
 * its last act (interrupts off: no FP instruction may follow). */

#ifndef _FPURELEASE_H_
#define _FPURELEASE_H_

#include <ogc/machine/processor.h>

static inline void fpu_release(void) {
	u32 level, cur, own;

	_CPU_ISR_Disable(level);
	__asm__ volatile("mfspr %0,274" : "=r" (cur));   /* SPRG2: this thread's context */
	__asm__ volatile("mfspr %0,275" : "=r" (own));   /* SPRG3: the FPU's owner */
	if (own && own == cur)
		__asm__ volatile("mtspr 275,%0" : : "r" (0));
	_CPU_ISR_Restore(level);
}

#endif
