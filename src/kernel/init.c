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
#include <kernel/boot_crumb.h>

static void kernel_init(void);
static int init(void);
extern int system_start(void);
extern int system_abs_time_init(void);

/* Records where the boot is, on a board that can keep it (kernel/boot_crumb.h).
 * Weak, and empty on the boards that cannot. */
void __attribute__((weak)) board_boot_crumb(unsigned int stage, const char *what) {
	(void)stage;
	(void)what;
}

/* Turns translation on where an architecture needs it before the first
 * spinlock. Weak, and empty on the ports that do not. */
void __attribute__((weak)) arch_mmu_early_on(void) {
}

/**
 * The setup of the system, the run level and execution of the idle function.
 */
void kernel_start(void) {
	board_boot_crumb(BOOT_CRUMB_KERNEL_START, "kernel_start");

	/* First: everything below this line can take a lock */
	arch_mmu_early_on();
	board_boot_crumb(BOOT_CRUMB_MMU_EARLY_ON, "arch_mmu_early_on");

	kernel_init();
	board_boot_crumb(BOOT_CRUMB_KERNEL_INIT, "kernel_init");

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
	board_boot_crumb(BOOT_CRUMB_RUNLEVELS, "runlevel_set");

	ret = runlevel_set(target_level);

	return ret;
}
