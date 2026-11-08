#define _CPU_ISR_Disable(level) ((level) = 0)
#define _CPU_ISR_Restore(level) ((void) (level))
