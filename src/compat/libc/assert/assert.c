/**
 * @file
 * @brief TODO
 *
 * @date 25.03.11
 * @author Eldar Abusalimov
 */

#include <framework/mod/options.h>
#include <hal/cpu.h>
#include <hal/platform.h>
#include <kernel/cpu/cpudata.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/thread.h>
#include <stdint.h>

#include "assert_impl.h"

#define BANNER_PRINT OPTION_GET(BOOLEAN, banner_print)

#ifndef NDEBUG
/*# error "Compiling assert.c for NDEBUG configuration"*/

char __assertion_message_buff[ASSERT_MESSAGE_BUFF_SZ];
static spinlock_t assert_lock = SPIN_STATIC_UNLOCKED;
static char assert_recursive_lock __cpudata__ = 0;

#if BANNER_PRINT
static const char oops_banner[] =
    "\n  ______"
    "\n |  ____|                                            __          __"
    "\n | |___  _ __ ___            ____  ____  ____  _____/ /   _____ / /"
    "\n |  ___|| \'_ ` _ \\          / __ \\/ __ \\/ __ \\/ ___/ /   |_____| |"
    "\n | |____| | | | | |_ _ _   / /_/ / /_/ / /_/ (__  )_/    |_____| |"
    "\n |______|_| |_| |_(_|_|_)  \\____/\\____/ .___/____(_)           | |"
    "\n                                     /_/                        \\_\\"
    "\n";

static void print_oops(void) {
	printk("\n%s", oops_banner);
}
#endif

#if defined(__aarch64__)
/* Who called the function that failed: whereami() has no aarch64 support,
 * and an assertion that names the line but not the path to it is half a
 * report -- BF-59's said "sched_unlock() without the BKL" twice and not from
 * where. The frame-pointer chain of this CPU: [fp] is the caller's fp,
 * [fp + 8] the return address. Bounded, and only while fp climbs. */
static void assert_backtrace(void) {
	uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
	struct thread *t = thread_self();
	int n;

	printk("\tthread %d (%s), called from:", t ? t->id : -1,
	    t && t->task ? task_get_name(t->task) : "-");
	for (n = 0; n < 16 && fp && !(fp & 7); n++) {
		uintptr_t next = ((uintptr_t *)fp)[0];

		printk(" %#lx", (unsigned long)((uintptr_t *)fp)[1]);
		if (next <= fp || next - fp > 0x100000) {
			break;
		}
		fp = next;
	}
	printk("\n");
}
#else
static inline void assert_backtrace(void) {
}
#endif

void __assertion_handle_failure(const struct __assertion_point *point) {
	if (cpudata_var(assert_recursive_lock)) {
		printk("\nrecursion detected on CPU %d\n", cpu_get_id());
		goto out;
	}
	cpudata_var(assert_recursive_lock) = 1;

#ifdef SMP
	/* Before a single character is printed, as in the abort handler */
	if (smp_stop_others) {
		smp_stop_others();
	}
#endif

	spin_lock_ipl_disable(&assert_lock);

#if BANNER_PRINT
	print_oops();
#endif
	printk(" ASSERTION FAILED on CPU %d\n" LOCATION_FUNC_FMT("\t", "\n") "\n"
	                                                                     "%s\n",
	    cpu_get_id(), LOCATION_FUNC_ARGS(&point->location), point->expression);

	if (*__assertion_message_buff)
		printk("\n\t(%s)\n", __assertion_message_buff);

	whereami();
	assert_backtrace();

#ifdef SMP
	if (smp_print_stopped) {
		smp_print_stopped();
	}
#endif

	spin_unlock(&assert_lock); /* leave IRQs off */

out:
	platform_shutdown(SHUTDOWN_MODE_ABORT);
	/* NOTREACHED */
}

#endif
