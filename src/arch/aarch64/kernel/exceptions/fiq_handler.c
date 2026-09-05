/**
 * @brief Fast interrupt handler
 *
 * @date 15.12.22
 * @author Aleksey Zhmulin
 */
#include <compiler.h>

#include <util/log.h>

#include "exception.h"

/* Xenolith: write this report to the SD card before the machine stops, so a
 * board with no serial adapter still gets its post-mortem. See
 * board/embox-rpi4/patches/aarch64-fault-flush.py. Weak: no log, no call. */
extern void xenolith_fault_flush(void) __attribute__((weak));

void _NORETURN aarch64_fiq_handler(struct excpt_context *ctx) {
	log_raw(LOG_EMERG, "\nUnexpected fiq interrupt!\n");
	aarch64_print_excpt_context(ctx);
	if (xenolith_fault_flush) {
		xenolith_fault_flush();
	}

	while (1) {};
}
