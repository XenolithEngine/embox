/**
 * @file
 * @brief
 *
 * @author Aleksey Zhmulin
 * @date 20.10.23
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/common/memory.h>
#include <drivers/irqctrl.h>
#include <framework/mod/options.h>
#include <hal/mem_barriers.h>
#include <hal/reg.h>
#include <kernel/critical.h>
#include <kernel/irq.h>
#include <util/field.h>
#include <util/log.h>

#include "gicv3.h"

#define PPI_PRIOR          0xa0U
#define SPI_PRIOR          0xa0U
#define CPU_PRIOR_MASK_LVL 0xffU

enum gic_irq_t {
	GIC_SGI,
	GIC_PPI,
	GIC_SPI,
	GIC_INVALID_TYPE,
};

/* This loop had no bound -- `delay` paced the polling, it never ended it.
 * Every caller waits on a bit the GIC clears in microseconds, and every one of
 * them runs before there is a console, so a controller that does not clear it
 * stopped the board with no output and nothing to read. On the RK3588 that is
 * not hypothetical: its redistributors come up powered down, and
 * Children-asleep stays set until they are powered (see gic_redist_init). A
 * bound turns a silent hang into a line in the log, and where the bit does
 * clear -- every board in tree -- the loop still ends on the first read. */
#define GIC_WAIT_SPINS 1000000UL

static void gic_wait_for_rwp(uintptr_t reg32, uint32_t rwp_mask) {
	unsigned long spins;

	for (spins = GIC_WAIT_SPINS; spins; spins--) {
		if (!(REG32_LOAD(reg32) & rwp_mask)) {
			return;
		}
	}

	log_error("gicv3: %#lx kept %#010x set, going on without it",
	    (unsigned long)reg32, rwp_mask);
}

static enum gic_irq_t gic_irq_type(unsigned int irq_nr) {
	switch (irq_nr) {
	case 0 ... 15:
		return GIC_SGI;
	case 16 ... 31:
		return GIC_PPI;
	case 32 ... 1019:
		return GIC_SPI;
	default:
		return GIC_INVALID_TYPE;
	}
}

/* Affinity as the GIC spells it -- Aff3.Aff2.Aff1.Aff0
 * packed into 32 bits, which is not how MPIDR_EL1 spells it (Aff3 lives at
 * [39:32]). This is what GICD_IROUTER takes and what GICR_TYPER reports. */
static inline uint32_t gic_affinity(uint64_t mpidr) {
	return (uint32_t)((mpidr & 0x00ffffff) | ((mpidr >> 8) & 0xff000000));
}

#ifdef SMP

/* Offsets inside a redistributor frame, for the walk below. It cannot use the
 * GICR_* macros: those already mean "this CPU's frame", which is the answer we
 * are looking for. */
#define GICR_TYPER_OFF       0x0008
#define GICR_TYPER_LAST      (1ULL << 4)
#define GICR_TYPER_AFF_SHIFT 32

/* Which frame serves which logical CPU, and the affinity that frame reported.
 * Written once by each CPU, in irqctrl_init_cpu(), before that CPU enables
 * interrupts; read afterwards by anyone. A zero base means "that CPU has not
 * come up", which is the only state in which it must not be addressed. */
static uintptr_t gic_rd_base[NCPU];
static uint32_t gic_rd_affinity[NCPU];

/**
 * Find the redistributor frame of the PE that calls this.
 *
 * The frames are contiguous and the last one sets GICR_TYPER.Last, but their
 * order is an implementation choice: frame N is not required to serve PE N.
 * Every frame names the PE it serves, so match on that.
 */
static uintptr_t gic_rd_base_find(uint32_t affinity) {
	uintptr_t frame;
	uint64_t typer;
	unsigned int i;

	frame = GICR_BASE;

	/* Bounded by NCPU because that is how far PERIPH_MEMORY_DEFINE below
	 * maps. A GIC with more PEs than the image knows about would run this
	 * walk off the end of its own mapping. */
	for (i = 0; i < NCPU; i++) {
		typer = REG64_LOAD(frame + GICR_TYPER_OFF);
		if ((uint32_t)(typer >> GICR_TYPER_AFF_SHIFT) == affinity) {
			return frame;
		}
		if (typer & GICR_TYPER_LAST) {
			break;
		}
		frame += GICR_STRIDE;
	}

	return 0;
}

uintptr_t gicv3_rd_base(void) {
	unsigned int cpu;
	uint32_t affinity;
	uintptr_t base;

	cpu = cpu_get_id();

	base = gic_rd_base[cpu];
	if (!base) {
		affinity = gic_affinity(ARCH_REG_LOAD(MPIDR_EL1));
		base = gic_rd_base_find(affinity);
		if (!base) {
			/* There is no safe value to return -- every GICR_* access made
			 * through this goes somewhere. Take the frame this CPU's id
			 * would name, which is at least inside the mapped window, and
			 * say so: a board that reaches this needs its redistributor
			 * layout read out, not guessed at. */
			base = GICR_BASE + GICR_STRIDE * cpu;
			log_error("gicv3: cpu %u (affinity %#010x) owns no redistributor, "
			          "assuming frame %u",
			    cpu, affinity, cpu);
		}
		gic_rd_affinity[cpu] = affinity;
		gic_rd_base[cpu] = base;
	}

	return base;
}

void irqctrl_send_ipi(unsigned int cpu_id, unsigned int irq) {
	uint32_t affinity;
	uint64_t sgi;

	assert(irq < 16);

	if (cpu_id >= NCPU || !gic_rd_base[cpu_id]) {
		/* Not a CPU that has run irqctrl_init_cpu(). Nothing to address. */
		return;
	}
	affinity = gic_rd_affinity[cpu_id];

	if ((affinity & 0xff) > 15) {
		/* Aff0 above 15 needs ICC_SGI1R_EL1.RS, which is GICv3.1 and which
		 * no board here has. Better to say so than to signal the wrong PE. */
		log_error("gicv3: cannot address affinity %#010x with one range",
		    affinity);
		return;
	}

	sgi = ((uint64_t)(affinity >> 24) & 0xff) << 48  /* Aff3 */
	      | ((uint64_t)(affinity >> 16) & 0xff) << 32 /* Aff2 */
	      | ((uint64_t)irq & 0xf) << 24               /* INTID */
	      | ((uint64_t)(affinity >> 8) & 0xff) << 16  /* Aff1 */
	      | (1ULL << (affinity & 0xf));               /* TargetList: Aff0 */

	/* Whatever the target is being woken to look at has to be visible to it
	 * before the interrupt is. */
	dsb(sy);
	ARCH_REG_STORE(ICC_SGI1R_EL1, sgi);
}

#endif /* SMP */

static void gic_dist_init(void) {
	size_t itlines;
	uint32_t affinity;
	int i, j;

	itlines = FIELD_GET(REG32_LOAD(GICD_TYPER), GICD_TYPER_ITLINES);

	/* Disable the distributor */
	REG32_STORE(GICD_CTLR, 0);
	gic_wait_for_rwp(GICD_CTLR, GICD_CTLR_RWP);

	for (i = 1; i <= itlines; i++) {
		/* Configure SPIs as non-secure Group-1 */
		REG32_STORE(GICD_IGROUPR(i), ~(uint32_t)0);

		/* Set SPIs to be level triggered */
		for (j = 0; j < 2; j++) {
			REG32_STORE(GICD_ICFGR(2 * i + j), 0);
		}

		/* Set priority for SPIs */
		for (j = 0; j < 8; j++) {
			REG32_STORE(GICD_IPRIORITYR(8 * i + j),
			    (SPI_PRIOR << 24) | (SPI_PRIOR << 16) | (SPI_PRIOR << 8)
			        | SPI_PRIOR);
		}
	}

	for (i = 0; i <= itlines; i++) {
		/* Deactivate and disable SGIs/PPIs/SPIs */
		REG32_STORE(GICD_ICACTIVER(i), ~(uint32_t)0);
		REG32_STORE(GICD_ICENABLER(i), ~(uint32_t)0);
	}

	/* Enable distributor */
	REG32_STORE(GICD_CTLR,
	    GICD_CTLR_GRP0 | GICD_CTLR_GRP1_NS | GICD_CTLR_ARE_NS);
	gic_wait_for_rwp(GICD_CTLR, GICD_CTLR_RWP);

	/* Set SPIs to current CPU only */
	affinity = gic_affinity(ARCH_REG_LOAD(MPIDR_EL1));
	for (i = 0; i <= itlines; i++) {
		for (j = 0; j < 32; j++) {
			REG64_STORE(GICD_IROUTER(32 * i + j), affinity);
		}
	}
}

static void gic_redist_init(void) {
	int i;

	/* A GIC-600 keeps each redistributor powered down until GICR_PWRR.RDPD is
	 * cleared, and refuses to drop Children-asleep before that. The register
	 * does not exist on a GIC-500 or under QEMU, and writing it there is not
	 * harmless, so which controller this is stays a board statement rather
	 * than a probe. */
	if (GIC600_PWRR) {
		REG32_STORE(GICR_PWRR, 0);
		gic_wait_for_rwp(GICR_PWRR, GICR_PWRR_RDPD);
	}

	/* Wake up this CPU redistributor */
	REG32_CLEAR(GICR_WAKER, GICR_WAKER_PS);
	gic_wait_for_rwp(GICR_WAKER, GICR_WAKER_CA);

	/* Configure SGIs/PPIs as non-secure Group-1 */
	REG32_STORE(GICR_IGROUPR0, ~(uint32_t)0);

	/* Set SGIs/PPIs to be level triggered */
	for (i = 0; i < 2; i++) {
		REG32_STORE(GICR_ICFGR0, 0);
	}

	/* Set priority for SGIs/PPIs */
	for (i = 0; i < 8; i++) {
		REG32_STORE(GICR_IPRIORITYR(i), (PPI_PRIOR << 24) | (PPI_PRIOR << 16)
		                                    | (PPI_PRIOR << 8) | PPI_PRIOR);
	}

	/* Deactivate and disable SPIs */
	REG32_STORE(GICR_ICACTIVER0, ~(uint32_t)0);
	REG32_STORE(GICR_ICENABLER0, ~(uint32_t)0);

	gic_wait_for_rwp(GICR_CTLR, GICR_CTLR_RWP);
}

static void gic_cpu_init(void) {
	uint64_t reg;

	/* CPU interface configuration */
	reg = ARCH_REG_LOAD(ICC_SRE_EL1);
	if (!(reg & ICC_SRE_SRE)) {
		ARCH_REG_STORE(ICC_SRE_EL1, reg | ICC_SRE_SRE);
	}

	ARCH_REG_STORE(ICC_PMR_EL1, CPU_PRIOR_MASK_LVL);
	/* EOI deactivates interrupt (mode 0) */
	ARCH_REG_CLEAR(ICC_CTLR_EL1, ICC_CTLR_EL1_EOImode);
	ARCH_REG_STORE(ICC_IGRPEN1_EL1, ICC_IGRPEN1_EL1_EN);
}

/* The half of the controller that belongs to one PE. The
 * boot CPU reaches it through gic_irqctrl_init(); every other CPU calls it on
 * itself once it has an MMU, and is deaf until it does. */
void irqctrl_init_cpu(void) {
	gic_redist_init();
	gic_cpu_init();
}

static int gic_irqctrl_init(void) {
	gic_dist_init();
	irqctrl_init_cpu();

	return 0;
}

void irqctrl_enable(unsigned int irq) {
	unsigned int reg_nr;
	uint32_t value;

	assert(irq_nr_valid(irq));

	reg_nr = irq >> 5;
	value = 1U << (irq & 0x1f);

	if (gic_irq_type(irq) == GIC_SPI) {
		REG32_STORE(GICD_ISENABLER(reg_nr), value);
	}
	else {
		REG32_STORE(GICR_ISENABLER0, value);
	}
}

void irqctrl_disable(unsigned int irq) {
	unsigned int reg_nr;
	uint32_t value;

	assert(irq_nr_valid(irq));

	reg_nr = irq >> 5;
	value = 1U << (irq & 0x1f);

	if (gic_irq_type(irq) == GIC_SPI) {
		REG32_STORE(GICD_ICENABLER(reg_nr), value);
	}
	else {
		REG32_STORE(GICR_ICENABLER0, value);
	}
}

void irqctrl_force(unsigned int irq) {
}

int irqctrl_pending(unsigned int irq) {
	return 0;
}

/* Sends an EOI (end of interrupt) signal to the PICs. */
void irqctrl_eoi(unsigned int irq) {
	assert(irq_nr_valid(irq));

	ARCH_REG_STORE(ICC_EOIR1_EL1, irq);
}

int irqctrl_get_intid(void) {
	return ARCH_REG_LOAD(ICC_IAR1_EL1) & ICC_IAR1_EL1_INTID_MASK;
}

void gicv3_init_el3(void) {
	REG32_STORE(GICD_CTLR, GICD_CTLR_DS);
	ARCH_REG_STORE(ICC_IGRPEN1_EL3, ICC_IGRPEN1_EL3_EN1NS);
}

IRQCTRL_DEF(gicv3, gic_irqctrl_init);

PERIPH_MEMORY_DEFINE(gicd, GICD_BASE, 0x10000);
/* One frame pair per PE, not just the boot CPU's. A
 * secondary faults on the first access to its own redistributor otherwise. */
PERIPH_MEMORY_DEFINE(gicr, GICR_BASE, GICR_STRIDE * NCPU);
