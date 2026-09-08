/**
 * @brief Interrupt handler
 *
 * Upstream re-enables IRQs around irq_dispatch (ipl_enable). Nested IRQs
 * then take another 800-byte FPSIMD frame and may context_switch with the
 * outer frame still live. Keep IRQs masked for the whole handler; the
 * timer still ticks, just without nesting.
 *
 * The stop IPI is served here rather than through irq_dispatch, and that is the
 * whole reason it is in this file: critical_enter() below takes the Big Kernel
 * Lock, so a core that parks inside an ordinary handler parks holding it. The
 * first core to stop then wedges every other core trying to reach its own stop
 * handler -- measured: two cores reported "did not answer" and one of them
 * went on to trip the deadlock detector. A stop-the-world
 * interrupt must not depend on the world's locks. So it is served before any
 * are taken, and carries the interrupted context straight to the reporter.
 */
#include <assert.h>
#include <stddef.h>

#include <drivers/irqctrl.h>
#include <hal/cpu.h>
#include <kernel/cpu/bkl.h>
#include <kernel/critical.h>
#include <kernel/irq.h>

#include "exception.h"

#ifdef SMP
#include <aarch64/smp.h>
#endif

void aarch64_irq_handler(struct excpt_context *ctx) {
	unsigned int irq;

	irq = irqctrl_get_intid();
	if (irq == -1) {
		return;
	}

	assert(irq_nr_valid(irq));
	assert(!critical_inside(CRITICAL_IRQ_LOCK));

	irqctrl_disable(irq);
	irqctrl_eoi(irq);

#ifdef SMP
	if (irq == AARCH64_SGI_STOP) {
		/* Does not return, and takes no lock on the way. */
		aarch64_smp_stop_self(ctx->pc, ctx->lr, ctx->sp, ctx->psr);
	}
#endif

	/* This is where the Big Kernel Lock's window would show: an interrupt that
	 * arrives on a non-zero critical count takes the nested path in
	 * critical_enter(), which does NOT acquire the Big Kernel Lock, because
	 * the count says this CPU already holds it.
	 *
	 * bkl_assert_owned() has guarded that promise since the W series and has
	 * never fired -- which says nothing on its own, because an assertion that
	 * never runs and one that always passes look identical from outside. So
	 * count all four states, and let a run say which it was. Read here, with
	 * interrupts still masked by the entry and before the count is raised, so
	 * the pair is this CPU's own. */
	bkl_irq_total++;
	if (critical_count() & __CRITICAL_BKL_MASK) {
		bkl_irq_nested++;
		if (!bkl_owned()) {
			bkl_irq_unowned++;
		}
	}
	else {
		bkl_irq_zero++;
	}

	critical_enter(CRITICAL_IRQ_HANDLER);
	{
		irq_dispatch(irq);
	}
	irqctrl_enable(irq);
	critical_leave(CRITICAL_IRQ_HANDLER);
	critical_dispatch_pending();
}
