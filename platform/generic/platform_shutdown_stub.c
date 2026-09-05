/**
 * @brief
 *
 * @date 20.12.23
 * @author Aleksey Zhmulin
 */
#include <compiler.h>

#include <hal/cpu.h>
#include <hal/cpu_idle.h>
#include <hal/fault_log.h>
#include <hal/platform.h>

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

	/* Every abort funnels through here -- assert(), panic(), and every fatal
	 * exception the handlers give up on -- and it runs after
	 * smp_print_stopped(), so what the other cores reported is saved too. */
	if ((mode == SHUTDOWN_MODE_ABORT) && fault_log_flush) {
		fault_log_flush();
	}

	while (1) {
		arch_cpu_idle();
	}
}
