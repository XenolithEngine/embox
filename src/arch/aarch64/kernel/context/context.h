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
	 * restored at offsets 120 and 128 by context_switch (the Xenolith FPSIMD
	 * overlay); zero for a thread that never leaves EL1. */
	uint64_t sp_el0;
	uint64_t tpidr_el0;
};

#endif /* __ASSEMBLER__ */

#endif /* ARCH_AARCH64_CONTEXT_H_ */
