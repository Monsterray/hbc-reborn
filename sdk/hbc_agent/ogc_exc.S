// The crash recorder's exception entry for libogc2 and libogc 1.x.
//
// Those libraries' vector code (exception_handler.S there) saves r0-r5, CR,
// LR, CTR, XER, MSR, SRR0 and SRR1 into a frame 728 bytes below the
// interrupted stack pointer, writes the exception number into it, turns
// address translation back on and jumps to _exceptionhandlertable[n]. It
// arrives with
//   r1  the interrupted stack pointer, not yet moved
//   r3  the exception number, r4 the frame's physical address
// so an entry has to claim the frame itself and save the rest of the
// registers before any C runs. This one does what libogc's own
// default_exceptionhandler does, then calls agent_exc(frame + 8), whose C
// view of the frame is libogc's frame_context. agent_exc records the crash
// and hands the frame to libogc's crash screen, which does not come back;
// the return path below is there in case it ever does.
//
// Floating point is off in an exception (MSR[FP] = 0), so the C side must not
// touch an FPR.

#include "ogc_flavor.h"

#if !AGENT_TUXEDO

	.text
	.align	2
	.globl	agent_exc_entry
agent_exc_entry:
	// The stack guard's breakpoint (safety.c) off first: this frame may
	// cover its doubleword, and translation is back on. r0 is saved.
	li	%r0,0
	mtspr	1013,%r0
	isync
	stwu	%r1,-AGENT_EXC_FRAME(%r1)	// the frame the vector code filled
	mfspr	%r0,912				// GQR0-7
	stw	%r0,AGENT_EXC_GQR(0)(%r1)
	mfspr	%r0,913
	stw	%r0,AGENT_EXC_GQR(1)(%r1)
	mfspr	%r0,914
	stw	%r0,AGENT_EXC_GQR(2)(%r1)
	mfspr	%r0,915
	stw	%r0,AGENT_EXC_GQR(3)(%r1)
	mfspr	%r0,916
	stw	%r0,AGENT_EXC_GQR(4)(%r1)
	mfspr	%r0,917
	stw	%r0,AGENT_EXC_GQR(5)(%r1)
	mfspr	%r0,918
	stw	%r0,AGENT_EXC_GQR(6)(%r1)
	mfspr	%r0,919
	stw	%r0,AGENT_EXC_GQR(7)(%r1)
	stmw	%r6,AGENT_EXC_GPR(6)(%r1)	// r6-r31; the vector saved r0-r5

	addi	%r3,%r1,AGENT_EXC_NUMBER
	bl	agent_exc

	lwz	%r4,AGENT_EXC_CR(%r1)
	mtcr	%r4
	lwz	%r4,AGENT_EXC_LR(%r1)
	mtlr	%r4
	lwz	%r4,AGENT_EXC_CTR(%r1)
	mtctr	%r4
	lwz	%r4,AGENT_EXC_XER(%r1)
	mtxer	%r4
	lwz	%r4,AGENT_EXC_GQR(0)(%r1)
	mtspr	912,%r4
	lwz	%r4,AGENT_EXC_GQR(1)(%r1)
	mtspr	913,%r4
	lwz	%r4,AGENT_EXC_GQR(2)(%r1)
	mtspr	914,%r4
	lwz	%r4,AGENT_EXC_GQR(3)(%r1)
	mtspr	915,%r4
	lwz	%r4,AGENT_EXC_GQR(4)(%r1)
	mtspr	916,%r4
	lwz	%r4,AGENT_EXC_GQR(5)(%r1)
	mtspr	917,%r4
	lwz	%r4,AGENT_EXC_GQR(6)(%r1)
	mtspr	918,%r4
	lwz	%r4,AGENT_EXC_GQR(7)(%r1)
	mtspr	919,%r4
	lmw	%r5,AGENT_EXC_GPR(5)(%r1)	// r5-r31
	lwz	%r2,AGENT_EXC_GPR(2)(%r1)
	lwz	%r0,AGENT_EXC_GPR(0)(%r1)
	lwz	%r4,AGENT_EXC_SRR0(%r1)
	mtsrr0	%r4
	lwz	%r4,AGENT_EXC_SRR1(%r1)
	mtsrr1	%r4
	lwz	%r4,AGENT_EXC_GPR(4)(%r1)
	lwz	%r3,AGENT_EXC_GPR(3)(%r1)
	addi	%r1,%r1,AGENT_EXC_FRAME
	rfi

#endif
