/**
 * @brief System error exception handler
 *
 * @date 15.12.22
 * @author Aleksey Zhmulin
 */
#include <compiler.h>
#include <inttypes.h>

#include <util/log.h>

#include <hal/reg.h>

#include "exception.h"


void _NORETURN aarch64_serror_handler(struct excpt_context *ctx) {
	uint32_t esr = ARCH_REG_LOAD(ESR_EL1);

	log_raw(LOG_EMERG, "\nSError exception!\n");

	/* An SError is ASYNCHRONOUS -- the pc in the context below is where the
	 * abort was taken, not where it came from, and reading it as the culprit
	 * sends whoever is holding the board after the wrong function. (It sent
	 * this project after __spin_trylock_smp once.) What actually says
	 * something is the syndrome, and it was not printed.
	 *
	 * EC is bits 31:26 and is 0x2f for an SError. Of the ISS below it:
	 *   IDS  (bit 24)  1 = the rest is implementation defined, read the TRM
	 *   AET  (12:10)   0 uncontainable, 1 unrecoverable, 2 restartable,
	 *                  3 recoverable, 6 corrected
	 *   EA   (bit 9)   external abort type, implementation defined
	 *   DFSC (5:0)     0x11 = "asynchronous SError", i.e. no more detail
	 *
	 * On a Cortex-A72 the common cause of an uncontainable one is an access
	 * that the interconnect refused: a peripheral at an offset that is not
	 * there, or DRAM past what the SKU actually has. */
	log_raw(LOG_EMERG, "ESR_EL1 = %08" PRIx32 " (EC %02" PRIx32 ", IDS %u, "
	                   "AET %u, EA %u, DFSC %02" PRIx32 ")\n",
	    esr, (esr >> 26) & 0x3f, (unsigned)((esr >> 24) & 1),
	    (unsigned)((esr >> 10) & 7), (unsigned)((esr >> 9) & 1), esr & 0x3f);
	log_raw(LOG_EMERG, "FAR_EL1 = %016" PRIx64 " (often UNKNOWN for an "
	                   "SError)\n",
	    (uint64_t)ARCH_REG_LOAD(FAR_EL1));

	aarch64_print_excpt_context(ctx);
	while (1) {};
}
