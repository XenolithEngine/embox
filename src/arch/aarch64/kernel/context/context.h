/**
 * @file
 * @brief
 *
 * @author  Anton Kozlov
 * @date    25.10.2012
 */
#ifndef ARCH_AARCH64_CONTEXT_H_
#define ARCH_AARCH64_CONTEXT_H_

#ifndef __ASSEMBLER__

#include <stdint.h>

struct context {
	uint64_t x[10] /* X19-X28 */;
	uint64_t fp;
	uint64_t lr;
	uint64_t sp;
	uint64_t spsr;
	uint64_t daif;
	/* XENOLITH_CONTEXT_EL0: the EL0 half of a thread. The kernel runs on
	 * SP_EL1 and never reads these, but a thread that returns to EL0 does,
	 * and with more than one EL0 thread they differ per thread. Saved and
	 * restored at offsets 120 and 128 by context_switch; zero for a thread
	 * that never leaves EL1. */
	uint64_t sp_el0;
	uint64_t tpidr_el0;
	/* XENOLITH_CONTEXT_FP: FPCR and FPSR of a thread that switched away by
	 * its own call: AAPCS64 has a callee keep FPCR, and C11 gives each thread
	 * its own floating-point environment. A preempted thread has both in its
	 * exception frame. Offsets 136 and 144 in context_switch. */
	uint64_t fpcr;
	uint64_t fpsr;
};

#endif /* __ASSEMBLER__ */

#endif /* ARCH_AARCH64_CONTEXT_H_ */
