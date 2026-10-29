/* libogc 3.1.0 (tuxedo) switches the FPU lazily: SPRG3 names the thread
 * context that owns the floating-point registers, and the first FP
 * instruction on another thread saves them into that context. A thread
 * that exits while it owns the FPU is never taken off SPRG3, so once
 * LWP_JoinThread has freed its block, that save lands in whatever the
 * heap has since put there. Each thread that uses floats calls this as
 * its last act (interrupts off: no FP instruction may follow), from a
 * wrapper round a noinline body, so that no restore of the body's FP
 * registers comes after it.
 *
 * It reads tuxedo's own use of SPRG2 and SPRG3, so it is for 3.1 only:
 * look again, and at whether KThreadExit now clears the owner, before
 * moving past it (REVIEW.md). */

#ifndef _FPURELEASE_H_
#define _FPURELEASE_H_

#include <ogc/libversion.h>
#include <ogc/machine/processor.h>

#if _V_MAJOR_ != 3 || _V_MINOR_ != 1
#error "fpu_release reads libogc 3.1's FPU owner (SPRG3): check it for this libogc"
#endif

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
