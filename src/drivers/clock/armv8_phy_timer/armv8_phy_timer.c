/**
 * @file
 *
 * @date Nov 24, 2020
 * @author Anton Bondarev
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/common/memory.h>
#include <framework/mod/options.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/reg.h>
#include <kernel/irq.h>
#include <kernel/time/clock_source.h>
#include <kernel/time/time_device.h>

#define IRQ_NUM OPTION_GET(NUMBER, irq_num)

#define PHY_TIMER_HZ 1000

/* Each core's next tick, in counter cycles. The timer registers are per core,
 * and so is this. */
static uint64_t phy_timer_deadline[NCPU];

/* The system counter the timer counts against. Without it the clock had the
 * tick's resolution: clock_gettime() moved in whole milliseconds, and so did
 * everything measured with it. CNTPCT_EL0 is one counter for all cores, so
 * any core may read it. The ISB keeps the read from being taken ahead of the
 * code before it, which would let a later reading come out smaller. */
static inline uint64_t phy_timer_count(void) {
	__asm__ __volatile__("isb" ::: "memory");
	return ARCH_REG_LOAD(CNTPCT_EL0);
}

static inline uint64_t phy_timer_load(void) {
	return ARCH_REG_LOAD(CNTFRQ_EL0) / PHY_TIMER_HZ;
}

/* The next tick is due a period after the last one was DUE, not a period
 * after this handler got to run: reloading TVAL here lost the interrupt's
 * latency on every tick, and the kernel's ticks fell behind the counter --
 * by 8 % on a busy four-core QEMU (clock_resolution test). Periods that
 * passed with no interrupt at all are counted rather than dropped. */
static irq_return_t phy_timer_handler(unsigned int irq_nr, void *dev_id) {
	uint64_t now, load, *deadline;
	unsigned ticks;

	load = phy_timer_load();
	deadline = &phy_timer_deadline[cpu_get_id()];
	now = phy_timer_count();

	ticks = 1;
	*deadline += load;
	if (now >= *deadline) {
		uint64_t missed = (now - *deadline) / load + 1;

		ticks += missed;
		*deadline += missed * load;
	}
	ARCH_REG_STORE(CNTP_CVAL_EL0, *deadline);

	clock_handle_ticks(dev_id, ticks);
	return IRQ_HANDLED;
}

static int phy_timer_set_periodic(struct clock_source *cs) {
	uint64_t *deadline = &phy_timer_deadline[cpu_get_id()];

	*deadline = phy_timer_count() + phy_timer_load();
	ARCH_REG_STORE(CNTP_CVAL_EL0, *deadline);
	ARCH_REG_STORE(CNTP_CTL_EL0, CNTP_CTL_EL0_EN);

	return ENOERR;
}

static struct time_event_device phy_timer_event_device = {
    .set_periodic = phy_timer_set_periodic,
    .name = "armv8_phy_timer",
    .irq_nr = IRQ_NUM};

static uint64_t phy_timer_get_time(struct clock_source *cs) {
	uint64_t count, hz;

	count = phy_timer_count();
	hz = cs->counter_device->cycle_hz;

	/* In two parts: count * NSEC_PER_SEC overflows 64 bits after a few
	 * minutes at 54 MHz. */
	return (count / hz) * NSEC_PER_SEC + (count % hz) * NSEC_PER_SEC / hz;
}

/* Cycles since this core's last tick: TVAL counts down to the next one. */
static cycle_t phy_timer_get_cycles(struct clock_source *cs) {
	int32_t left, load;

	load = cs->counter_device->cycle_hz / PHY_TIMER_HZ;
	left = (int32_t)ARCH_REG_LOAD(CNTP_TVAL_EL0);
	if (left < 0) {
		return load;
	}
	if (left > load) {
		return 0;
	}
	return load - left;
}

static struct time_counter_device phy_timer_counter_device = {
    .get_cycles = phy_timer_get_cycles,
    .get_time = phy_timer_get_time,
    .mask = UINT64_MAX};

static int phy_timer_init(struct clock_source *cs) {
	phy_timer_counter_device.cycle_hz = ARCH_REG_LOAD(CNTFRQ_EL0);

	return irq_attach(IRQ_NUM, phy_timer_handler, 0, cs, "armv8_phy_timer");
}

CLOCK_SOURCE_DEF(armv8_phy_timer, phy_timer_init, NULL, &phy_timer_event_device,
    &phy_timer_counter_device);
