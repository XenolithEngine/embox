/**
 * @brief Synchronous exception handler (Xenolith overlay).
 *
 * Two things beyond upstream, and they meet in one function.
 *
 * K2 (docs/EMBOX-USERSPACE.md): exceptions taken from EL0 are routed instead
 * of being fatal. Upstream treats every synchronous exception the same way --
 * print and spin -- which for a system with user mode means a syscall hangs
 * the kernel and a bad pointer in an application takes the OS down with it.
 *
 *   * SVC from AArch64 (EC 0x15) is a system call. exit/exit_group are handled
 *     here because they are the way *out* of EL0 rather than a service the
 *     kernel renders; everything else goes to aarch64_syscall_dispatch(),
 *     which is weak until K3 supplies the table.
 *   * Everything else taken from EL0 kills the thread and leaves the kernel
 *     running. The rule is stated once, on the saved PSR, rather than as a list
 *     of exception classes: an abort has its own diagnostic because the fault
 *     address is worth printing, but nothing an application can do to itself
 *     may take the OS with it. BRK (EC 0x3C) is what made that worth stating --
 *     __builtin_trap() emits one, so abort(), a failed assert and an uncaught
 *     C++ exception all arrive as a BRK from EL0, and before this they printed
 *     a register dump and spun (U3).
 *
 * Killing the thread is not done from inside the exception frame: scheduling
 * from there would run the scheduler on the exception stack with the frame
 * still live. Instead the frame is rewritten so the eret in excpt_exit lands
 * at EL1 in aarch64_usermode_dead(), on the thread's own kernel stack, with
 * the reason in x0. From there it is an ordinary kernel function call.
 *
 * SMP (phase F7): the kernel-fatal path stops the other cores before it prints
 * a character. On one core `while (1) {}` was a fine way to stop; on four the
 * other three keep running over the console, and if the cause is not local to
 * one core they are on their way into this same handler -- three cores faulting
 * at once produced a register dump with every third character belonging to
 * someone else. So that path goes where every other abort goes: stop the
 * others, print, report them, shut down.
 *
 * WHERE THAT STOP LIVES IS PART OF THE DESIGN, not a detail. It stops the
 * machine, so it may only run once the exception is known to be fatal TO THE
 * KERNEL. Above it, in the EL0 cases, it would have meant that the first
 * system call any application made parked the other three cores for good --
 * silently, because that path returns to EL0 and never prints. K2 and F7
 * arrived on separate branches and each is right on its own; the order below
 * is what makes them right together.
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

/* Xenolith: on a board with no UART the screen is the only console, and this
 * handler spins with interrupts masked -- so printk above goes to a serial
 * port nobody is listening to and to a RAM ring whose writer is a thread
 * that will never run again. Weak: an Embox without the board's
 * drivers/fault_screen links and behaves exactly as before.
 *
 * This lives in the overlay rather than in a patch because the overlay is
 * copied over the Embox tree on every build; a patch applied underneath it
 * is thrown away, silently, which is how the first attempt was lost. */
extern void fault_screen_show(const char *what, uint64_t esr, uint64_t far,
    uint64_t pc, uint64_t lr) __attribute__((weak));

/* docs/EMBOX-SYSCALL-ABI.md section 6.1. Spelled out rather than included:
 * this file is arch code and must not grow a dependency on the libc headers
 * to name two integers. */
#define XL_NR_EXIT       93
#define XL_NR_EXIT_GROUP 94

/* Reason handed to aarch64_usermode_dead(), and through it to thread_exit().
 * The tag is above the 32-bit payload so a caller can tell an exit status from
 * an ESR without a second channel. */
#define EL0_DEAD_EXIT    (1ULL << 32)
#define EL0_DEAD_FAULT   (2ULL << 32)

/**
 * Provided by embox.arch.aarch64.usermode, which may legitimately not be in
 * the image -- a build with no user mode cannot produce an EL0 exception, so
 * reaching the weak version means the hardware did something impossible.
 * Keeping it weak is what lets this module stay free of a dependency on the
 * thread layer.
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
 * The syscall table. Weak here so the kernel links and reports honestly
 * before K3 exists; K3 replaces it.
 */
__attribute__((weak)) long aarch64_syscall_dispatch(struct excpt_context *ctx) {
	log_error("EL0 syscall %" PRIu64 " has no implementation yet (K3)", ctx->x[8]);

	return -ENOSYS;
}

/* Rewrite the frame so excpt_exit returns to EL1 in aarch64_usermode_dead(). */
static void usermode_leave(struct excpt_context *ctx, uint64_t reason) {
	ctx->x[0] = reason;
	ctx->pc = (uint64_t)(uintptr_t)aarch64_usermode_dead;
	ctx->psr = SPSR_ELn_M_EL1h;
}

/* Which EL the exception was taken FROM, read off the saved PSR.
 *
 * The exception class answers this only for the aborts, which have separate
 * numbers for the same-EL and lower-EL cases (0x20/0x24 vs 0x21/0x25). BRK,
 * an illegal instruction, a trapped MSR and an SP alignment fault do not --
 * one class each, whichever EL they came from -- so the frame is the only
 * place the answer is written down. */
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
		if ((ctx->x[8] == XL_NR_EXIT) || (ctx->x[8] == XL_NR_EXIT_GROUP)) {
			usermode_leave(ctx, EL0_DEAD_EXIT | (ctx->x[0] & 0xff));
		}
		else {
			ctx->x[0] = (uint64_t)aarch64_syscall_dispatch(ctx);
		}
		return;

	case ESR_ELn_EC_INST_ABT_LOW:
	case ESR_ELn_EC_DATA_ABT_LOW:
		/* LOG_EMERG, not LOG_ERR: the syndrome line below prints at EMERG and
		 * this one did not, so every EL0 fault reported the KIND of fault
		 * without the address it happened at -- which is the half that says
		 * where to look. */
		/* TTBR0 is in the line because it is the first thing to suspect when
		 * an EL0 fault makes no sense: this kernel installs a task's root once,
		 * in usermode_trampoline, and the scheduler does not switch it (K2,
		 * deferred to K6 with ASIDs). A fault whose FAR IS mapped in the task's
		 * own tables and whose TTBR0 belongs to someone else is that bug, and
		 * without printing the root there is nothing to tell them apart. */
		log_raw(LOG_EMERG,
		    "\nEL0 %s abort at PC %#" PRIx64 ", FAR %#" PRIx64 ", TTBR0 %#" PRIx64
		    "\n",
		    (class == ESR_ELn_EC_INST_ABT_LOW) ? "instruction" : "data",
		    (uint64_t)ctx->pc, (uint64_t)ARCH_REG_LOAD(FAR_EL1),
		    (uint64_t)ARCH_REG_LOAD(TTBR0_EL1));
		print_abort_syndrome(syndrome);
		usermode_leave(ctx, EL0_DEAD_FAULT | esr);
		return;

	default:
		/* Not a syscall and not a lower-EL abort. If the frame says EL0 the
		   application dies here and the kernel keeps running -- the two cases
		   above are instances of that rule, not exceptions to it. BRK is the
		   one worth naming: __builtin_trap() emits one, so abort(), a failed
		   assert and an uncaught C++ exception all arrive this way. */
		if (from_el0(ctx)) {
			log_raw(LOG_EMERG, "\nEL0 %s at PC %#" PRIx64 ", ESR %#" PRIx32 "\n",
			    (class == ESR_ELn_EC_BRK) ? "trap" : "exception",
			    (uint64_t)ctx->pc, esr);
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

	/* Xenolith: and put it where someone can see it -- before the shutdown
	 * path, which does not return. On a board whose only console is the
	 * screen, everything printed above this line is invisible, so this is
	 * the whole diagnosis as far as anyone standing at the board is
	 * concerned. Bounded and re-entrant-safe by construction (it paints
	 * once and returns on a second entry), so it cannot cost the report
	 * below. */
	if (fault_screen_show) {
		fault_screen_show(what, esr, (uint64_t)ARCH_REG_LOAD(FAR_EL1),
		    ctx->pc, ctx->lr);
	}

	/* Reports what the stopped cores were doing, then parks this one. */
	platform_shutdown(SHUTDOWN_MODE_ABORT);
}
