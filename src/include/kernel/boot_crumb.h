/**
 * @file
 * @brief Where the boot is, for a board that can keep it across a reset.
 *
 * Called at the steps of kernel_start() and before each unit's init. The
 * default does nothing; a board that has memory a watchdog reset leaves alone
 * records the last step there, so that the next boot can say where this one
 * stopped -- even before a console or a card.
 */
#ifndef KERNEL_BOOT_CRUMB_H_
#define KERNEL_BOOT_CRUMB_H_

#define BOOT_CRUMB_ASM_DATA      6 /* reset handler: .bss zeroed, .data copied */
#define BOOT_CRUMB_ASM_CPUDATA   7 /* reset handler: after cpudata_init() */
#define BOOT_CRUMB_KERNEL_START  2
#define BOOT_CRUMB_MMU_EARLY_ON  3
#define BOOT_CRUMB_KERNEL_INIT   4
#define BOOT_CRUMB_RUNLEVELS     5
#define BOOT_CRUMB_UNIT          0x10000 /* + the unit's ordinal in this boot */

#ifndef __ASSEMBLER__
extern void board_boot_crumb(unsigned int stage, const char *what);
#endif

#endif /* KERNEL_BOOT_CRUMB_H_ */
