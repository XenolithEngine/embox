/**
 * @file
 *
 * @date 26 march 2016
 * @author: Anton Bondarev
 */
#ifndef DRIVERS_INTERRUPT_GIC_GIC_H_
#define DRIVERS_INTERRUPT_GIC_GIC_H_

/* How many interrupt numbers the kernel keeps a table for. The GIC itself
 * allows up to 1020 SPIs; 256 covers every board here but one, and the
 * exception is not exotic -- the RK3588's UART2 is SPI 333, so irq_attach()
 * refuses it with -EINVAL and the console never opens, with no diagnosis
 * beyond an empty tty name. Raising it for everyone spends .bss on boards
 * that will never see an interrupt above 255, so a board that needs more
 * asks for it in its build.conf:
 *
 *     CFLAGS += -D__IRQCTRL_IRQS_TOTAL=512
 */
#ifndef __IRQCTRL_IRQS_TOTAL
#define __IRQCTRL_IRQS_TOTAL 256
#endif

#endif /* DRIVERS_INTERRUPT_GIC_GIC_H_ */
