// Which libogc the agent is built against, for C and assembly alike.
//
// AGENT_TUXEDO is 1 for libogc 3.x (devkitPro's current libogc, built on
// tuxedo), and 0 for libogc2 (Extrems' fork) and libogc 1.x. The last two
// share the older exception design: a table of assembly entry points that
// the vector code jumps to (see ogc_exc.S).

#ifndef AGENT_OGC_FLAVOR_H
#define AGENT_OGC_FLAVOR_H

#if __has_include(<tuxedo/ppc/exception.h>)
#define AGENT_TUXEDO 1
#else
#define AGENT_TUXEDO 0
#endif

// The older design's exception frame (ogc/machine/asm.h there): the
// exception number, SRR0, SRR1, r0-r31, GQR0-7, then CR, LR, CTR, XER, MSR.
// The same in libogc2 and libogc 1.8.23; agent.c checks it against their
// frame_context. The C view of it starts at AGENT_EXC_NUMBER.
#define AGENT_EXC_FRAME   728
#define AGENT_EXC_NUMBER  8
#define AGENT_EXC_SRR0    12
#define AGENT_EXC_SRR1    16
#define AGENT_EXC_GPR(n)  (20 + 4 * (n))
#define AGENT_EXC_GQR(n)  (148 + 4 * (n))
#define AGENT_EXC_CR      180
#define AGENT_EXC_LR      184
#define AGENT_EXC_CTR     188
#define AGENT_EXC_XER     192

#endif
