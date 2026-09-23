/**
 * @file
 *
 * @date 10.11.10
 * @author Anton Bondarev
 */

#include <assert.h>
#include <drivers/diag.h>
#include <stdarg.h>

#include <hal/cpu.h>
#include <util/atomic_rmw.h>

#include <module/embox/compat/libc/stdio/print_impl.h>

static int printk_printchar(struct printchar_handler_data *d, int c) {
	diag_putc(c);
	return c;
}

#ifdef SMP
/* One message at a time on the console; the owner is a core.
 *
 * Interrupts are left alone: at 115200 baud a hundred-character message is
 * 9 ms of polling the UART, too long to hold off the timer and the network.
 * So an interrupt handler or a fault that prints on the core holding the lock
 * prints straight through rather than wait for itself, and after
 * PRINTK_SPIN_LIMIT turns a message goes out unserialised: a holder stopped
 * mid-message by smp_stop_others() on a panic must not silence the rest. */
#define PRINTK_NOBODY     (-1)
#define PRINTK_SPIN_LIMIT 10000000UL

static int printk_owner = PRINTK_NOBODY;

static int printk_lock(void) {
	int self = (int)cpu_get_id();
	unsigned long spins;

	if (atomic_rmw_load(&printk_owner, __ATOMIC_RELAXED) == self) {
		return 0; /* nested on this core: already ours */
	}
	for (spins = 0; spins < PRINTK_SPIN_LIMIT; spins++) {
		if (atomic_rmw_try_lock(&printk_owner, PRINTK_NOBODY, self)) {
			return 1;
		}
	}
	return 0;
}

static void printk_unlock(int taken) {
	if (taken) {
		atomic_rmw_store(&printk_owner, PRINTK_NOBODY, __ATOMIC_RELEASE);
	}
}
#endif /* SMP */

int vprintk(const char *format, va_list args) {
	int ret;
#ifdef SMP
	int taken = printk_lock();
#endif

	ret = __print(printk_printchar, NULL, format, args);

#ifdef SMP
	printk_unlock(taken);
#endif
	return ret;
}

int printk(const char *format, ...) {
	int ret;
	va_list args;

	assert(format != NULL);

	va_start(args, format);
	ret = vprintk(format, args);
	va_end(args);

	return ret;
}
