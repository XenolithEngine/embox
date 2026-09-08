/**
 * @file
 * @brief The kernel initialization sequence.
 *
 * @date 21.03.09
 * @author Anton Bondarev
 * @author Alexey Fomin
 * @author Eldar Abusalimov
 */

#include <drivers/diag.h>
#include <drivers/irqctrl.h>
#include <framework/mod/runlevel.h>
#include <hal/cpu_idle.h>
#include <hal/ipl.h>
#include <hal/platform.h>
#include <kernel/kgdb.h>
#include <kernel/klog.h>
#include <kernel/printk.h>
#include <util/log.h>

static void kernel_init(void);
static int init(void);
extern int system_start(void);
extern int system_abs_time_init(void);

/**
 * The setup of the system, the run level and execution of the idle function.
 */
/* Some cores cannot take a spinlock until translation is
 * on -- a Cortex-A72 raises an SError on an exclusive to Device memory, and
 * with the MMU off that is all memory. The architecture decides whether it
 * needs this; on the ports that do not, it is an empty call. It has to be
 * FIRST: everything below here can lock. */
/* Weak, so a port that has no such need links without providing one. */
void __attribute__((weak)) arch_mmu_early_on(void) {
}

void kernel_start(void) {
	arch_mmu_early_on();

	kernel_init();

	kgdb_start(init);

	init();

	system_abs_time_init();

	system_start();

	while (1) {
		arch_cpu_idle();
	}
}

/**
 * The initialization functions are called to set up interrupts, perform
 * further memory configuration, initialization of drivers, devices.
 */
static void kernel_init(void) {
	platform_init();

	ipl_init();

	diag_init();

	printk("\n");

	klog_init();

	irqctrl_init();
	log_info("Interrupt controller: %s", irqctrl_get()->name);
}

/**
 * Set the run level to the value of the file system and net level.
 * @return 0
 */
static int init(void) {
	int ret;
	const runlevel_nr_t target_level = RUNLEVEL_NRS_ENABLED - 1;

	log_info("Embox kernel start");

	ret = runlevel_set(target_level);

	return ret;
}
