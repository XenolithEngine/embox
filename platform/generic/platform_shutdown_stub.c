/**
 * @brief
 *
 * @date 20.12.23
 * @author Aleksey Zhmulin
 */
#include <compiler.h>

#include <hal/cpu.h>
#include <hal/cpu_idle.h>
#include <hal/platform.h>

/* XENOLITH_FAULT_DUMP: write the tail of the log ring to the card, straight
 * through the controller -- no filesystem, no mutex, no sleeping.
 *
 * This is where it belongs and nowhere else. Every abort funnels through here:
 * assert(), panic(), and every fatal exception the aarch64 handlers give up
 * on. Hooking the handlers instead would have covered some of those and none
 * of the others, and hooking them at the point where they still had the report
 * only in RAM was the whole defect.
 *
 * It runs AFTER smp_print_stopped(), so what the other cores reported is in
 * the ring and goes to the card with everything else. Weak: a board with no SD
 * log links and behaves exactly as before. */
extern void xenolith_fault_flush_raw(void) __attribute__((weak));

void _NORETURN platform_shutdown(shutdown_mode_t mode) {
#ifdef SMP
	/* XENOLITH_PANIC_STOP: this core is going down. The others are still
	 * running -- still writing to the same console the diagnosis is being
	 * printed on, and still changing the state that explains it. Stop them,
	 * and have them say where they were. An orderly shutdown has nothing to
	 * report and nothing to race with. */
	if (mode == SHUTDOWN_MODE_ABORT) {
		/* Weak: only some architectures implement them */
		if (smp_stop_others) {
			smp_stop_others();
		}
		if (smp_print_stopped) {
			smp_print_stopped();
		}
	}
#endif /* SMP */

	if ((mode == SHUTDOWN_MODE_ABORT) && xenolith_fault_flush_raw) {
		xenolith_fault_flush_raw();
	}

	while (1) {
		arch_cpu_idle();
	}
}
