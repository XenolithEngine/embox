/**
 * @brief Synchronous exception handler (Xenolith overlay).
 *
 * An exception taken from EL0 does not stop the kernel:
 *
 *   * SVC from AArch64 (EC 0x15) is a system call. exit/exit_group are
 *     handled here, being the way out of EL0; everything else goes to
 *     aarch64_syscall_dispatch().
 *   * Anything else from EL0, decided on the saved PSR rather than on the
 *     exception class, kills the thread. BRK is the common case: abort(), a
 *     failed assert and an uncaught C++ exception all arrive as one.
 *
 * The thread is not killed from inside the exception frame, which would run
 * the scheduler on the exception stack: the frame is rewritten so the eret in
 * excpt_exit lands at EL1 in aarch64_usermode_dead(), on the thread's own
 * kernel stack, with the reason in x0.
 *
 * Only a fault fatal to the kernel stops the other cores (SMP), before the
 * first character of the report. Stopping them on the EL0 paths would park
 * them for good on the first system call, so that stop stays below them.
 */
#include <compiler.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>

#include <hal/cpu.h>
#include <hal/platform.h>
#include <hal/reg.h>
#include <util/field.h>
#include <util/log.h>

#include <hal/fault_log.h>

#include "exception.h"

/* Xenolith: on a board with no UART the screen is the only console. Weak, so
 * an Embox without the board's drivers/fault_screen links unchanged. */
extern void fault_screen_show(const char *what, uint64_t esr, uint64_t far,
    uint64_t pc, uint64_t lr) __attribute__((weak));

/* docs/EMBOX-SYSCALL-ABI.md section 6.1; spelled out so arch code needs no
 * libc header. */
#define XL_NR_EXIT       93
#define XL_NR_EXIT_GROUP 94

/* Reason handed to aarch64_usermode_dead(), and through it to thread_exit().
 * The tag is above the 32-bit payload so a caller can tell an exit status from
 * an ESR without a second channel. */
#define EL0_DEAD_EXIT    (1ULL << 32)
#define EL0_DEAD_FAULT   (2ULL << 32)

/**
 * Provided by embox.arch.aarch64.usermode. A build without user mode has no
 * EL0 exceptions; weak so this module does not depend on the thread layer.
 */
__attribute__((weak)) void _NORETURN aarch64_usermode_dead(uint64_t reason) {
	log_raw(LOG_EMERG, "\nEL0 exception with no usermode support (reason %#" PRIx64 ")\n",
	    reason);
	/* Thread context: the frame was rewritten so this runs at EL1 on the
	 * thread's own kernel stack, where the filesystem may be used. The flush
	 * that needs no lock goes first, so a report survives even if the other
	 * one cannot finish. */
	if (fault_log_flush) {
		fault_log_flush();
	}
	if (fault_log_flush_thread) {
		fault_log_flush_thread();
	}

	while (1) {};
}

/**
 * The syscall table. The weak version answers -ENOSYS.
 */
__attribute__((weak)) long aarch64_syscall_dispatch(struct excpt_context *ctx) {
	log_error("EL0 syscall %" PRIu64 " has no implementation yet (K3)", ctx->x[8]);

	return -ENOSYS;
}

/**
 * Ends the whole task rather than this thread. Weak for the same reason as
 * aarch64_usermode_dead: an image without user mode has no task to end.
 */
__attribute__((weak)) void _NORETURN aarch64_usermode_group_dead(uint64_t status) {
	aarch64_usermode_dead(EL0_DEAD_EXIT | (status & 0xff));
}

/**
 * drivers/xlsyscall zeroes the thread's word and wakes its futex, so whoever
 * reclaims the stack learns it is free. Weak: no syscall layer, no EL0 threads.
 */
__attribute__((weak)) void xl_thread_exiting(void) {
}

/* Rewrite the frame so excpt_exit returns to EL1 in aarch64_usermode_dead(). */
static void usermode_leave(struct excpt_context *ctx, uint64_t reason) {
	ctx->x[0] = reason;
	ctx->pc = (uint64_t)(uintptr_t)aarch64_usermode_dead;
	ctx->psr = SPSR_ELn_M_EL1h;
}

/* The same, for the landing that ends the task instead of the thread. */
static void usermode_leave_group(struct excpt_context *ctx, uint64_t status) {
	ctx->x[0] = status;
	ctx->pc = (uint64_t)(uintptr_t)aarch64_usermode_group_dead;
	ctx->psr = SPSR_ELn_M_EL1h;
}

/* Which EL the exception was taken from, read off the saved PSR. Only the
 * aborts have separate classes per EL (0x20/0x24 vs 0x21/0x25); BRK, an
 * illegal instruction or an SP alignment fault has one class for both. */
static int from_el0(const struct excpt_context *ctx) {
	return (ctx->psr & SPSR_ELn_M_MASK) == SPSR_ELn_M_EL0t;
}

static void print_abort_syndrome(uint32_t syndrome) {
	int el;
	unsigned dfsc;

	el = 1;
	dfsc = syndrome & 0b111111;

	switch (dfsc) {
	case 0b000000:
		el = 0;
	case 0b000001:
		log_raw(LOG_EMERG, "Address Size fault (EL%i)\n", el);
		break;

	case 0b000100:
	case 0b000101:
	case 0b000110:
	case 0b000111:
		log_raw(LOG_EMERG, "Translation fault (level %u)\n", dfsc & 0b11);
		break;

	case 0b001000:
		el = 0;
	case 0b001001:
		log_raw(LOG_EMERG, "Access Flag fault (EL%i)\n", el);
		break;

	/* The low two bits of DFSC are the level, as above */
	case 0b001100:
	case 0b001101:
	case 0b001110:
	case 0b001111:
		log_raw(LOG_EMERG, "Permission fault (level %u)\n", dfsc & 0b11);
		break;

	case 0b010000:
		log_raw(LOG_EMERG, "External abort\n");
		break;

	case 0b011000:
		log_raw(LOG_EMERG, "Parity error on a memory access\n");
		break;

	case 0b010100:
		el = 0;
	case 0b010101:
		log_raw(LOG_EMERG,
		    "External abort on a translation table walk (EL%i)\n", el);
		break;

	case 0b011100:
		el = 0;
	case 0b011101:
		log_raw(LOG_EMERG,
		    "Parity error on a memory access on a translation table walk "
		    "(EL%i)\n",
		    el);
		break;

	case 0b100001:
		log_raw(LOG_EMERG, "Alignment fault\n");
		break;

	case 0b110000:
		log_raw(LOG_EMERG, "TLB Conflict fault\n");
		break;

	default:
		log_raw(LOG_EMERG, "Unknown fault (DFSC = %#x)\n", dfsc);
		break;
	}
}

void aarch64_sync_handler(struct excpt_context *ctx) {
	uint32_t esr;
	uint32_t class;
	uint32_t syndrome;
	/* Xenolith: the same name the log gets, for the screen. */
	const char *what = "Synchronous exception";

	esr = ARCH_REG_LOAD(ESR_EL1);
	class = FIELD_GET(esr, ESR_ELn_EC);
	syndrome = FIELD_GET(esr, ESR_ELn_ISS);

	switch (class) {
	case ESR_ELn_EC_SVC64:
		if (ctx->x[8] == XL_NR_EXIT) {
			/* This thread only: its stack and TLS belong to its creator,
			 * which xl_thread_exiting() tells they are free. */
			xl_thread_exiting();
			usermode_leave(ctx, EL0_DEAD_EXIT | (ctx->x[0] & 0xff));
		}
		else if (ctx->x[8] == XL_NR_EXIT_GROUP) {
			xl_thread_exiting();
			usermode_leave_group(ctx, ctx->x[0] & 0xff);
		}
		else {
			ctx->x[0] = (uint64_t)aarch64_syscall_dispatch(ctx);
		}
		return;

	case ESR_ELn_EC_INST_ABT_LOW:
	case ESR_ELn_EC_DATA_ABT_LOW:
		/* LOG_EMERG, the level of the syndrome line below. TTBR0 is printed
		 * because the scheduler does not switch it: a FAR mapped in the task's
		 * own tables under another task's root is that case. */
		log_raw(LOG_EMERG,
		    "\nEL0 %s abort at PC %#" PRIx64 ", FAR %#" PRIx64 ", TTBR0 %#" PRIx64
		    "\n",
		    (class == ESR_ELn_EC_INST_ABT_LOW) ? "instruction" : "data",
		    (uint64_t)ctx->pc, (uint64_t)ARCH_REG_LOAD(FAR_EL1),
		    (uint64_t)ARCH_REG_LOAD(TTBR0_EL1));
		print_abort_syndrome(syndrome);
		/* Killed, not exited -- and a joiner waiting for this thread's stack
		 * has to be released either way. */
		xl_thread_exiting();
		usermode_leave(ctx, EL0_DEAD_FAULT | esr);
		return;

	default:
		/* From EL0 the application dies and the kernel keeps running; BRK
		   comes from __builtin_trap(), behind abort() and a failed assert. */
		if (from_el0(ctx)) {
			log_raw(LOG_EMERG, "\nEL0 %s at PC %#" PRIx64 ", ESR %#" PRIx32 "\n",
			    (class == ESR_ELn_EC_BRK) ? "trap" : "exception",
			    (uint64_t)ctx->pc, esr);
			xl_thread_exiting();
			usermode_leave(ctx, EL0_DEAD_FAULT | esr);
			return;
		}
		break;
	}

	/* From EL1, and there is nothing this kernel can do about it. */

#ifdef SMP
	/* Before the first character of the dump: the other cores are running
	 * over the console it uses and over the state that explains the fault */
	smp_stop_others();
#endif

	switch (class) {
	case ESR_ELn_EC_INST_ABT:
		what = "Instruction abort";
		log_raw(LOG_EMERG, "\nInstruction abort exception!\n");
		print_abort_syndrome(syndrome);
		log_raw(LOG_EMERG, "FAR_EL1 = %#" PRIx64 "\n",
		    (uint64_t)ARCH_REG_LOAD(FAR_EL1));
		break;

	case ESR_ELn_EC_DATA_ABT:
		what = "Data abort";
		log_raw(LOG_EMERG, "\nData abort exception!\n");
		print_abort_syndrome(syndrome);
		log_raw(LOG_EMERG, "FAR_EL1 = %#" PRIx64 "\n",
		    (uint64_t)ARCH_REG_LOAD(FAR_EL1));
		break;

	default:
		log_raw(LOG_EMERG, "\nSynchronous exception!\n");
		log_raw(LOG_EMERG, "ESR_EL1 = %" PRIx32 "\n", esr);
		break;
	}

	aarch64_print_excpt_context(ctx);

	/* Xenolith: the report on the screen, before the shutdown path, which
	 * does not return. It paints once and returns on a second entry. */
	if (fault_screen_show) {
		fault_screen_show(what, esr, (uint64_t)ARCH_REG_LOAD(FAR_EL1),
		    ctx->pc, ctx->lr);
	}

	/* Reports what the stopped cores were doing, then parks this one. */
	platform_shutdown(SHUTDOWN_MODE_ABORT);
}
