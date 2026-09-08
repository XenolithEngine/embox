/****************************************************************************
 * PSCI: the only way to start a secondary core on QEMU virt.
 *
 *
 * Which instruction carries the call is not a property of the CPU, it is a
 * property of what sits above the kernel, and on QEMU it follows the flags we
 * pass ourselves: `virt,virtualization=on` boots the image at EL2 and puts
 * PSCI behind SMC, plain `virt` boots it at EL1 and puts PSCI behind HVC. Both
 * were read out of `-machine dumpdtb`.
 *
 * Normally the answer comes from the device tree, /psci/method. We cannot ask:
 * QEMU sets x0 to the device tree only when it boots the image as Linux, and
 * ours is a plain ELF, so the guest never sees the tree QEMU built for it
 * as a plain ELF. Hence a module option, and hence it is only correct as long
 * as it agrees with the command line the machine was started with.
 *
 * Some boards have no PSCI at all: a Raspberry Pi 4's armstub parks the
 * secondaries polling a mailbox instead, and psci_method="" is how such a
 * board says so.
 ****************************************************************************/

#include <stdint.h>
#include <string.h>

#include <framework/mod/options.h>
#include <util/log.h>

#include <aarch64/psci.h>

#define PSCI_METHOD OPTION_STRING_GET(psci_method)

/* PSCI 0.2+, SMC Calling Convention: bit 30 selects the 64-bit variant. */
#define PSCI_FN_VERSION 0x84000000UL
#define PSCI_FN_CPU_ON  0xc4000003UL

enum psci_conduit {
	PSCI_CONDUIT_NONE,
	PSCI_CONDUIT_SMC,
	PSCI_CONDUIT_HVC,
};

static enum psci_conduit conduit(void) {
	if (!strcmp(PSCI_METHOD, "smc")) {
		return PSCI_CONDUIT_SMC;
	}
	if (!strcmp(PSCI_METHOD, "hvc")) {
		return PSCI_CONDUIT_HVC;
	}
	return PSCI_CONDUIT_NONE;
}

static long psci_call(unsigned long fn, unsigned long a1, unsigned long a2,
    unsigned long a3) {
	/* The conduit is decided BEFORE the argument registers are named, and
	 * nothing may be called after: a register asm variable only has to hold
	 * its value where the asm reads it, and x0..x3 are the very registers a
	 * call is free to destroy.  conduit() calls strcmp(), so with the switch
	 * the other way round GCC emitted
	 *
	 *     ldr x0, [sp, #40]     <- the function id
	 *     bl  conduit           <- returns its enum in x0
	 *     smc #0                <- ... and that enum is what PSCI got
	 *
	 * and every call answered NOT_SUPPORTED. Clang happened to rematerialise
	 * the value after the call and hid it. */
	const enum psci_conduit c = conduit();

	if (c == PSCI_CONDUIT_NONE) {
		return PSCI_NOT_SUPPORTED;
	}

	{
		register unsigned long x0 __asm__("x0") = fn;
		register unsigned long x1 __asm__("x1") = a1;
		register unsigned long x2 __asm__("x2") = a2;
		register unsigned long x3 __asm__("x3") = a3;

		if (c == PSCI_CONDUIT_SMC) {
			__asm__ __volatile__("smc #0"
			                     : "+r"(x0)
			                     : "r"(x1), "r"(x2), "r"(x3)
			                     : "memory");
		}
		else {
			__asm__ __volatile__("hvc #0"
			                     : "+r"(x0)
			                     : "r"(x1), "r"(x2), "r"(x3)
			                     : "memory");
		}

		return (long)x0;
	}
}

bool psci_available(void) {
	return conduit() != PSCI_CONDUIT_NONE;
}

const char *psci_method(void) {
	return PSCI_METHOD[0] ? PSCI_METHOD : "(none)";
}

long psci_version(void) {
	return psci_call(PSCI_FN_VERSION, 0, 0, 0);
}

long psci_cpu_on(uint64_t mpidr, uintptr_t entry, uint64_t context_id) {
	return psci_call(PSCI_FN_CPU_ON, mpidr, entry, context_id);
}
