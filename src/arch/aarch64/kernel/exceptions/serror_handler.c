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

/* Xenolith: write this report to the SD card before the machine stops, so a
 * board with no serial adapter still gets its post-mortem -- straight to a
 * fixed LBA through the controller, because a handler that runs with
 * interrupts masked may not wait on the filesystem's mutex. See
 * board/embox-rpi4/patches/aarch64-fault-flush.py. Weak: no log, no call. */
extern void xenolith_fault_flush_raw(void) __attribute__((weak));

/* Xenolith: write this report to the SD card before the machine stops, so a
 * board with no serial adapter still gets its post-mortem. See
 * board/embox-rpi4/patches/aarch64-fault-flush.py. Weak: no log, no call. */
extern void xenolith_fault_flush(void) __attribute__((weak));

void _NORETURN aarch64_serror_handler(struct excpt_context *ctx) {
	uint32_t esr = ARCH_REG_LOAD(ESR_EL1);

	log_raw(LOG_EMERG, "\nSError exception!\n");

	/* An SError is asynchronous: the pc in the context below is where it was
	 * taken, not where it came from, so the syndrome is what says something.
	 *   EC   (31:26)  0x2f for an SError
	 *   IDS  (bit 24) 1 = the rest is implementation defined, read the TRM
	 *   AET  (12:10)  0 uncontainable, 1 unrecoverable, 2 restartable,
	 *                 3 recoverable, 6 corrected
	 *   EA   (bit 9)  external abort type, implementation defined
	 *   DFSC (5:0)    0x11 = asynchronous SError, i.e. no more detail
	 * An uncontainable one on a Cortex-A72 is usually an access the
	 * interconnect refused: a peripheral that is not there, or absent DRAM. */
	log_raw(LOG_EMERG, "ESR_EL1 = %08" PRIx32 " (EC %02" PRIx32 ", IDS %u, "
	                   "AET %u, EA %u, DFSC %02" PRIx32 ")\n",
	    esr, (esr >> 26) & 0x3f, (unsigned)((esr >> 24) & 1),
	    (unsigned)((esr >> 10) & 7), (unsigned)((esr >> 9) & 1), esr & 0x3f);
	log_raw(LOG_EMERG, "FAR_EL1 = %016" PRIx64 " (often UNKNOWN for an "
	                   "SError)\n",
	    (uint64_t)ARCH_REG_LOAD(FAR_EL1));

	aarch64_print_excpt_context(ctx);
	if (xenolith_fault_flush) {
		xenolith_fault_flush();
	}

	if (xenolith_fault_flush_raw) {
		xenolith_fault_flush_raw();
	}

	while (1) {};
}
