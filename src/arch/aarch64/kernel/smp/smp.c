/****************************************************************************
 * aarch64 SMP: starting the secondary cores.
 *
 *
 * Shape follows src/arch/riscv/kernel/smp/smp.c, which is the newest of the
 * two SMP ports in the tree and the closer one: a primary that hands each
 * secondary a stack and a way in, and a secondary that gives itself an idle
 * thread, tells the scheduler it exists, and parks.
 *
 * What is specific to aarch64 is the state a freshly powered-on core is in.
 * PSCI hands it to us at the highest non-secure EL with the MMU off, the
 * caches off, and nothing but the entry address and one register of context.
 * Three consequences run through everything below:
 *
 *   * it cannot use the primary's page tables until it programs its own
 *     TTBR/TCR/MAIR -- those registers are per-PE, and the `initialized` flag
 *     in mmu_on() hides that;
 *   * until the MMU is on, Normal memory is not cacheable and not shareable,
 *     so LDXR/STXR do not work. No spinlock, no atomic, nothing that Embox
 *     builds on them, until aarch64_ap_start() has finished. Plain loads and
 *     stores only;
 *   * and it reads what the primary wrote while the primary had caches on,
 *     so the primary has to clean that to the point of coherency first. QEMU
 *     does not model caches and would forgive this; a Cortex-A72 would not.
 ****************************************************************************/

#include <stdint.h>

#include <drivers/irqctrl.h>
#include <embox/unit.h>
#include <framework/mod/options.h>
#include <hal/cache.h>
#include <hal/cpu.h>
#include <hal/cpu_idle.h>
#include <hal/ipl.h>
#include <hal/mem_barriers.h>
#include <hal/reg.h>
#include <kernel/cpu/cpu.h>
#include <kernel/cpu/cpudata.h>
#include <kernel/critical.h>
#include <kernel/irq.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/printk.h>
#include <kernel/task/kernel_task.h>
#if OPTION_GET(NUMBER, spintable_base) != 0
#include <sys/mman.h>

#include <hal/mmu.h>
#include <mem/vmem.h>
#include <mem/vmem_device_memory.h>
#endif
#include <kernel/thread.h>
#include <kernel/time/clock_source.h>
#include <util/log.h>

#include <module/embox/kernel/thread/core.h>

#include <aarch64/cpu_id.h>
#include <aarch64/psci.h>
#include <aarch64/smp.h>
#include <util/atomic_rmw.h>

#define THREAD_STACK_SIZE \
	OPTION_MODULE_GET(embox__kernel__thread__core, NUMBER, thread_stack_size)

/* 0 on a PSCI board; 0xd8 on a Pi. See the option in Mybuild. */
#define SPINTABLE_BASE OPTION_GET(NUMBER, spintable_base)

EMBOX_UNIT_INIT(aarch64_smp_init);

/**
 * Everything a secondary needs before it can use the kernel's own memory
 * abstractions, which is to say before it has an MMU.
 *
 * Read with the MMU and the caches off, so the primary cleans it to the point
 * of coherency before every CPU_ON. `sp` is first on purpose: the reset
 * handler loads it from offset 0 of this symbol, in assembly, before there is
 * a C environment to ask.
 */
/* One entry per core, not one for all of them.
 *
 * It used to be a single structure, rewritten before every CPU_ON. That is
 * correct only while every core answers before the next one is asked for, and
 * the wait below is bounded on purpose -- an image that hangs there is worse
 * than one that comes up with fewer cores and says so. When the wait runs out,
 * the primary goes on to the next core and overwrites `sp`; the core it gave
 * up on then wakes, reads the entry, and takes SOMEBODY ELSE'S STACK.
 *
 * Measured on the hour of acceptance, 2026-09-07, one boot in 499:
 *
 *   [error] (smp) aarch64 smp: cpu 1 did not answer after CPU_ON
 *   [info]  (smp) aarch64 smp: cpu 2 up ... cpu 3 up ... 3 of 4 cpus up
 *   [info]  (smp) aarch64 smp: cpu 1 up            <- arrives anyway, late
 *   Instruction abort: pc = lr = 0x3c0, sp inside cpu1's stack, cpu3 aborted
 *
 * -- two cores on one stack, and the return address one of them read back was
 * the other's saved DAIF. On a board a slow core is likelier than in an
 * emulator, not less.
 *
 * `claim` is the other half: on giving up, the primary revokes the entry, and
 * a core that arrives late reads that and parks instead of joining a system
 * that has already counted itself. Padded to a cache line, so that cleaning
 * one core's entry to the point of coherency cannot disturb another's.
 */
struct aarch64_ap_boot {
	uint64_t sp;
	uint64_t tcr;
	uint64_t mair;
	uint64_t ttbr0;
	uint64_t ttbr1;
	volatile uint64_t ack;
	volatile uint64_t claim;
	uint64_t pad;
};

/* The reset handler indexes this with the core's own id and a shift of 6. */
_Static_assert(sizeof(struct aarch64_ap_boot) == 64,
    "reset_handler.S indexes aarch64_ap_boot with lsl #6");

struct aarch64_ap_boot aarch64_ap_boot[NCPU] __attribute__((aligned(64)));

/* The reset handler's secondary entry, which PSCI is told to start at. */
extern char aarch64_ap_entry;

/* The same descent for a core released off a spin table, which arrives with
 * its registers zeroed and so has to read its own id out of MPIDR. */
extern char aarch64_ap_entry_mpidr;

/* One stack per secondary. It has to be at least thread_stack_size: the idle
 * thread below is initialised on it, and thread_init_stack() puts the thread
 * structure at the top of the region it is given. Undersize it and the idle
 * thread's control block lands before the start of the array -- which is why
 * mods.conf sets ap_stack_size to match. */
static char ap_stack[NCPU - 1][KERNEL_AP_STACK_SZ] __attribute__((aligned(16)));

/* Secondaries come up one at a time. Nothing below is written to be
 * concurrent, and there is no reason for it to be. */
static spinlock_t startup_lock = SPIN_STATIC_UNLOCKED;

static void *ap_idle_run(void *arg) {
	(void)arg;
	panic("aarch64 smp: the idle thread of a secondary returned\n");
}

extern void thread_set_current(struct thread *t);

void aarch64_startup_ap(unsigned int cpu_id);

/* One per CPU, in its own copy of the cpudata block. Nothing else reads or
 * writes another CPU's copy, so the increment needs no lock -- and it happens
 * in an interrupt handler, where a lock would be the wrong shape anyway. */
static unsigned long ipi_count __cpudata__;

unsigned long aarch64_smp_ipi_count(unsigned int cpu_id) {
	if (cpu_id >= NCPU) {
		return 0;
	}
	/* Volatile rather than an atomic load. What both sides need is that the
	 * compiler not keep the counter in a register -- one writes it from an
	 * interrupt handler, the other reads it in a polling loop from a
	 * different CPU. The hardware side is already settled: an aligned
	 * word-sized load on aarch64 is single-copy atomic. And the atomic
	 * builtins cannot see that alignment anyway, because cpudata_cpu_ptr()
	 * reaches the right block through char arithmetic. */
	return *(volatile unsigned long *)cpudata_cpu_ptr(cpu_id, &ipi_count);
}

/**
 * What arrives on the other end of smp_send_resched().
 *
 * There is no message to decode: the SGI number *is* the message, so unlike
 * the RISC-V port -- which shares one `ipi_message` variable behind a spinlock
 * because the CLINT's IPI carries no payload -- there is nothing here to
 * serialise. Sixteen SGIs, one meaning each, and the GIC does the delivery.
 *
 * sched_post_switch_noyield() and not sched_post_switch(): the latter also
 * raises the yield flag, which makes the *current* schedee give up its slice.
 * We were asked to look at the run queue, not to step aside; x86 came to the
 * same conclusion and left the yielding variant commented out.
 */
static irq_return_t resched_ipi_handler(unsigned int irq_nr, void *data) {
	extern void sched_post_switch_noyield(void);
	volatile unsigned long *count;

	(void)irq_nr;
	(void)data;

	count = cpudata_ptr(&ipi_count);
	*count += 1;

	sched_post_switch_noyield();

	return IRQ_HANDLED;
}

/**
 * Wake a CPU up to reschedule.
 *
 * Called by sched.c when the run queue holds something this CPU's affinity
 * mask will not let it run, and by sched_ticker.c on every tick. Both call it
 * from inside an interrupt handler, so it has to stay this small.
 *
 * A CPU that has not come up yet is dropped by the driver rather than checked
 * for here: whether a target is addressable is a fact the interrupt controller
 * holds, not one this file should keep a second copy of.
 */
void smp_send_resched(int cpu_id) {
	irqctrl_send_ipi((unsigned int)cpu_id, AARCH64_SGI_RESCHED);
}

/**
 * Give this core its own timer tick.
 *
 * Not a nicety: the timer strategy keeps one list of timers per CPU, so a
 * thread that sleeps while running here queues its timer on this core's list,
 * and nobody else will ever walk it. A core that schedules threads must be a
 * core that ticks.
 *
 * Nothing about the generic timer is spelled out here. CNTP_CTL/CNTP_TVAL are
 * per-PE, so the driver's own set_periodic() programs whichever core calls it,
 * and the interrupt is a PPI, so enabling it speaks for this core only. The
 * handler was attached once by the boot CPU -- irq_table is global -- which is
 * why there is nothing to attach.
 */
static void ap_clock_start(void) {
	extern const struct clock_source *cs_jiffies;
	const struct time_event_device *ed;

	if (!cs_jiffies || !cs_jiffies->event_device) {
		log_warning("aarch64 smp: no clock source, cpu %u will not tick",
		    cpu_get_id());
		return;
	}

	ed = cs_jiffies->event_device;
	if (!ed->set_periodic) {
		log_warning("aarch64 smp: clock source %s has no periodic mode",
		    ed->name);
		return;
	}

	irqctrl_enable(ed->irq_nr);
	ed->set_periodic((struct clock_source *)cs_jiffies);
}

/****************************************************************************
 * Stopping the other cores when one of them is about to die.
 *
 * The value of this is not that the cores stop -- it is what they say on the
 * way. Three times in phases F4 and F5 the question that mattered was "what
 * were the OTHER cores doing", and three times the answer had to be dug out of
 * QEMU by hand, from a board that had kept running and overwritten half of it.
 ****************************************************************************/

struct aarch64_cpu_report {
	volatile int state; /* 0 not asked, 1 asked, 2 answered */
	unsigned long pc;
	unsigned long lr;
	unsigned long sp;
	unsigned long psr;
	unsigned long crit;
	void *schedee;
	int is_idle;
};

static struct aarch64_cpu_report cpu_report[NCPU];
static volatile int smp_stopping;
static volatile int smp_printed;

/**
 * What a core says before it stops.
 *
 * Reached straight from the interrupt entry, before it has taken the BKL, and
 * never returns: this core is done. Masking interrupts first is what makes
 * "stopped" mean stopped -- otherwise the timer would keep this core walking
 * its own timer list while the other one prints.
 */
void aarch64_smp_stop_self(unsigned long pc, unsigned long lr, unsigned long sp,
    unsigned long psr) {
	struct aarch64_cpu_report *r;
	struct schedee *cur;
	struct thread *idle;
	unsigned int me;

	ipl_disable();

	me = cpu_get_id();
	r = &cpu_report[me];

	r->pc = pc;
	r->lr = lr;
	r->sp = sp;
	r->psr = psr;
	r->crit = (unsigned long)critical_count();

	cur = schedee_get_current();
	idle = cpu_get_idle(me);
	r->schedee = cur;
	r->is_idle = (cur && idle && cur == &idle->schedee);

	/* Release: everything above must be visible to the core that reads
	 * state, and it reads state to decide whether the rest is there. */
	atomic_store(&r->state, 2, __ATOMIC_RELEASE);

	while (1) {
		arch_cpu_idle();
	}
}

void smp_stop_others(void) {
	unsigned int self, i;
	unsigned long spins;
	int asked = 0;

	/* Two cores panicking at once must not wait for each other. */
	if (atomic_exchange(&smp_stopping, 1, __ATOMIC_ACQ_REL)) {
		return;
	}

	self = cpu_get_id();

	for (i = 0; i < NCPU; i++) {
		if (i == self || cpu_get_idle(i) == NULL) {
			continue;
		}
		cpu_report[i].state = 1;
		irqctrl_send_ipi(i, AARCH64_SGI_STOP);
		asked++;
	}

	if (!asked) {
		return;
	}

	/* Bounded, and short: a core that has interrupts masked will never
	 * answer, and this is the abort path -- saying so beats hanging. */
	for (spins = 0; spins < 200000000UL; spins++) {
		int pending = 0;

		for (i = 0; i < NCPU; i++) {
			if (cpu_report[i].state == 1) {
				pending = 1;
			}
		}
		if (!pending) {
			break;
		}
		__barrier();
	}
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
}

/**
 * Print what the stopped cores said.
 *
 * Separate from stopping them because the two want different moments: the
 * stop should happen before anything is printed -- so the failing core owns the
 * console and the others are frozen where the failure found them -- and the
 * report should come after the failure itself, where a reader expects it.
 */
void smp_print_stopped(void) {
	unsigned int self, i;

	if (!smp_stopping || atomic_exchange(&smp_printed, 1, __ATOMIC_ACQ_REL)) {
		return;
	}

	self = cpu_get_id();

	printk("\n== other cpus at the time of the abort ==\n");
	for (i = 0; i < NCPU; i++) {
		struct aarch64_cpu_report *r = &cpu_report[i];

		if (i == self) {
			printk("  cpu%u  <- the one that aborted\n", i);
			continue;
		}
		if (cpu_get_idle(i) == NULL) {
			printk("  cpu%u  never started\n", i);
			continue;
		}
		if (r->state != 2) {
			printk("  cpu%u  did not answer (interrupts masked, or wedged)\n",
			    i);
			continue;
		}
		printk("  cpu%u  pc %#018lx  lr %#018lx\n", i, r->pc, r->lr);
		/* The schedee pointer is printed rather than described because the
		 * interesting case is two cores naming the same one -- a thread on
		 * two CPUs at once is the failure the TW_SMP_WAKING protocol exists
		 * to prevent, and it is invisible in any per-core description. */
		printk("        sp %#018lx  psr %#010lx  crit %#lx  schedee %p  %s\n",
		    r->sp, r->psr, r->crit, r->schedee,
		    r->is_idle ? "(idle)" : "(running)");
	}
	printk("\n");
}

/**
 * The first C a secondary runs, with the MMU still off.
 *
 * Nothing here may touch a lock, and nothing here may call into the kernel
 * proper -- both would end in an exclusive access to memory that is neither
 * cacheable nor shareable yet. It programs the translation registers the
 * primary was using, switches the MMU on, and only then is this an ordinary
 * CPU running ordinary kernel code.
 */
void aarch64_ap_start(unsigned int cpu_id) {
	struct aarch64_ap_boot *slot = &aarch64_ap_boot[cpu_id];

	ARCH_REG_STORE(TCR_EL1, slot->tcr);
	ARCH_REG_STORE(MAIR_EL1, slot->mair);
	ARCH_REG_STORE(TTBR0_EL1, slot->ttbr0);
	ARCH_REG_STORE(TTBR1_EL1, slot->ttbr1);
	isb();

	__asm__ __volatile__("tlbi vmalle1" : : : "memory");
	__asm__ __volatile__("ic iallu" : : : "memory");
	dsb(sy);
	isb();

	ARCH_REG_ORIN(SCTLR_EL1, SCTLR_ELn_M | SCTLR_ELn_C | SCTLR_ELn_I);
	isb();

	aarch64_startup_ap(cpu_id);
}

void aarch64_startup_ap(unsigned int cpu_id) {
	struct thread *idle;

	/* The MMU and the caches are on now, so this reads
	 * what the primary last wrote. If it gave up waiting for this core, the
	 * system has already counted itself without it and said so in the log;
	 * joining now would make that line a lie and put a core into a scheduler
	 * that never expected it. Park instead. */
	if (!atomic_load(&aarch64_ap_boot[cpu_id].claim, __ATOMIC_ACQUIRE)) {
		while (1) {
			arch_cpu_idle();
		}
	}

	__spin_lock(&startup_lock);

	/* TPIDR_EL1 already holds cpu_id -- the reset handler set it from the
	 * context id PSCI delivered. This records which MPIDR that id belongs
	 * to, which is what `cpuinfo` and the log line below report. Addressing
	 * an IPI does not go through here: the GIC driver asks each core's own
	 * redistributor which PE it serves, so it never has to be told. */
	aarch64_cpu_id_register(cpu_id);

	/* The redistributor and the CPU interface belong to this core and are
	 * still asleep; until they are up, ipl_enable() below unmasks nothing.
	 * Then the two interrupts this core is meant to take. */
	irqctrl_init_cpu();
	irqctrl_enable(AARCH64_SGI_RESCHED);
	irqctrl_enable(AARCH64_SGI_STOP);
	ap_clock_start();

	idle = thread_init_stack((char *)(uintptr_t)aarch64_ap_boot[cpu_id].sp
	                             - THREAD_STACK_SIZE,
	    THREAD_STACK_SIZE, SCHED_PRIORITY_MIN, ap_idle_run, NULL);
	cpu_init(cpu_id, idle);
	task_thread_register(task_kernel_task(), idle);
	thread_set_current(idle);
	sched_set_current(&idle->schedee);

	log_info("aarch64 smp: cpu %u up, MPIDR %#010llx, EL%llu", cpu_id,
	    (unsigned long long)aarch64_cpu_mpidr(cpu_id),
	    (unsigned long long)((ARCH_REG_LOAD(CurrentEL) >> 2) & 0x3));

	aarch64_ap_boot[cpu_id].ack = 1;
	__spin_unlock(&startup_lock);

	ipl_enable();

	while (1) {
		arch_cpu_idle();
	}
}

/**
 * MPIDR of a secondary we have not met yet.
 *
 * Chicken and egg: the affinity of a core is something the core reports about
 * itself, and this one is not running. The device tree would say (/cpus/cpu@N
 * reg) on a board that hands the kernel one; where there is no tree to read,
 * this assumes what the machines tested so far do -- logical id N is
 * Aff0 = N in the primary's cluster. Wrong on anything with more than one
 * cluster, and that is the day this reads the tree instead.
 */
static uint64_t ap_mpidr(unsigned int cpu_id) {
	return (aarch64_cpu_mpidr(0) & ~0xffULL) | cpu_id;
}

/**
 * Release a core that the firmware parked on a spin table. The Pi's way,
 * because a Pi has no PSCI.
 *
 * armstub8 leaves cores 1..3 in this loop, and QEMU's raspi model installs it
 * verbatim (hw/arm/raspi.c, write_smpboot64), so board and emulator are one
 * mechanism rather than two:
 *
 *     spin:  wfe
 *            ldr  x4, [0xd8 + 8 * (mpidr & 3)]
 *            cbz  x4, spin
 *            mov  x0, #0 ... mov x3, #0
 *            br   x4
 *
 * Three things follow from those six instructions, and they are the whole of
 * this function.
 *
 * The parked core polls with its MMU and caches OFF, so the address has to be
 * in memory and not in a cache line this core owns: dcache_flush, not a
 * barrier. A barrier orders our stores against each other; it does not
 * publish them to a reader that is not looking through the same cache.
 *
 * It sleeps on WFE between reads, so something must be the event. SEV.
 *
 * And it jumps with x0..x3 zeroed, so nothing carries the logical id the way
 * PSCI's context argument does -- the core has to read its own out of MPIDR,
 * which is what the second entry point is for.
 */
static void cpu_release_spintable(unsigned int cpu_id) {
	volatile uint64_t *slot;

	slot = (volatile uint64_t *)(uintptr_t)(SPINTABLE_BASE + 8 * cpu_id);
	*slot = (uint64_t)(uintptr_t)&aarch64_ap_entry_mpidr;
	dcache_flush((void *)slot, sizeof(*slot));
	dsb(sy);
	__asm__ __volatile__("sev" ::: "memory");
}

#if SPINTABLE_BASE != 0
/* The release words live at physical 0xd8..0xf0, in
 * the first page of RAM, and nothing maps that page.
 *
 * It did not matter while this board ran with the MMU off. Since
 * turned translation on, the write below is a translation
 * fault: measured on the board as `Data abort ... DFSC=0x7, FAR_EL1 = 0xe0'
 * -- 0xe0 being the release word of cpu 1 -- with pc in
 * cpu_release_spintable().
 *
 * Mapped around the release and unmapped after it, rather than left in place:
 * page zero mapped for the life of the system would make every null
 * dereference a silent write into the spin table, and this board has a fault
 * screen precisely so that such a thing is seen. The cores read their word
 * with their caches off, which cpu_release_spintable() already handles with a
 * dcache_flush; this only has to make the address reachable from here. */
static uintptr_t spintable_page(void) {
	return (uintptr_t)SPINTABLE_BASE & ~(uintptr_t)MMU_PAGE_MASK;
}

static void spintable_map(void) {
	mmap_device_memory((void *)spintable_page(), MMU_PAGE_SIZE,
	    PROT_READ | PROT_WRITE, MAP_FIXED, spintable_page());
}

static void spintable_unmap(void) {
	vmem_unmap_region(vmem_current_context(), spintable_page(),
	    MMU_PAGE_SIZE);
	mmu_flush_tlb();
}
#endif /* SPINTABLE_BASE */

static int cpu_start(unsigned int cpu_id) {
	unsigned long spins;
	long ret;

	struct aarch64_ap_boot *slot = &aarch64_ap_boot[cpu_id];

	slot->sp = (uint64_t)(uintptr_t)&ap_stack[cpu_id - 1][KERNEL_AP_STACK_SZ];
	slot->ack = 0;
	slot->claim = 1;

	/* The core about to read this has no caches on. */
	dcache_flush(slot, sizeof(*slot));
	dsb(sy);

	if (SPINTABLE_BASE != 0) {
		cpu_release_spintable(cpu_id);
	}
	else {
		ret = psci_cpu_on(ap_mpidr(cpu_id), (uintptr_t)&aarch64_ap_entry,
		    cpu_id);
		if (ret != PSCI_SUCCESS) {
			log_error("aarch64 smp: CPU_ON(cpu %u, MPIDR %#010llx) = %ld",
			    cpu_id, (unsigned long long)ap_mpidr(cpu_id), ret);
			return -1;
		}
	}

	/* Bounded, because an image that hangs here is worse than one that comes
	 * up with fewer cores and says so.
	 *
	 * The bound is a spin count, which means different things on different
	 * hosts: under an emulator whose vCPU thread is not scheduled, it can run
	 * out while the core is merely late. Measured over the 499 boots of the
	 * acceptance hour, 2 lost a core that way; with the machine also busy
	 * building, within a handful. Ten times the patience costs a board with a
	 * genuinely dead core a few more seconds of boot, and a busy host
	 * nothing. */
	for (spins = 0; spins < 1000000000UL; spins++) {
		if (slot->ack) {
			/* The ack is a plain volatile store on the other side, which
			 * orders nothing. Everything that core published before it --
			 * its redistributor frame, its idle thread -- is read by this
			 * one immediately afterwards, so pair the two here. */
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			return 0;
		}
		__barrier();
	}

	/* Give up, and say so to the core as well as to the log. Its own entry
	 * keeps its own stack, so a core that is merely late comes up on memory
	 * nobody else is using and then reads this and parks. */
	atomic_store(&slot->claim, 0, __ATOMIC_RELEASE);
	dcache_flush(slot, sizeof(*slot));
	dsb(sy);

	log_error("aarch64 smp: cpu %u did not answer after %s; revoked, and it"
	          " will park if it arrives later",
	    cpu_id, SPINTABLE_BASE ? "the spin-table release" : "CPU_ON");
	return -1;
}

static int aarch64_smp_init(void) {
	unsigned int self, i;
	int up = 1;
	int ret;

	aarch64_cpu_id_register(0);

	/* Mark the scheduler tick shared, the way x86 and RISC-V do from their
	 * own smp unit init. This is not bookkeeping for later -- sched.c's SMP
	 * path in sched_ticker_update() reads
	 *
	 *     sched_ticker_get_timer()->timer_sharing->shared_cpu
	 *
	 * and timer_sharing is NULL until this call attaches it. Found the hard
	 * way: the board takes a translation fault at sched.c under network
	 * load, as soon as the run queue holds something this CPU's affinity
	 * mask does not select. */
	sched_ticker_set_shared();

	self = cpu_get_id();

	/* The stop IPI is not attached to anything -- the interrupt entry serves
	 * it directly, before taking a lock -- but the GIC still has to be told
	 * to deliver it, and enabling an SGI speaks for one core only. Each
	 * secondary does the same for itself in aarch64_startup_ap(). */
	irqctrl_enable(AARCH64_SGI_STOP);

	/* Snapshot what a secondary needs to reach the same address space. Taken
	 * here, not written by hand, so it cannot drift from whatever mmu.c
	 * decided -- and this runs at runlevel 2, long after vmem_init() turned
	 * the MMU on. */
	for (i = 1; i < NCPU; i++) {
		aarch64_ap_boot[i].tcr = ARCH_REG_LOAD(TCR_EL1);
		aarch64_ap_boot[i].mair = ARCH_REG_LOAD(MAIR_EL1);
		aarch64_ap_boot[i].ttbr0 = ARCH_REG_LOAD(TTBR0_EL1);
		aarch64_ap_boot[i].ttbr1 = ARCH_REG_LOAD(TTBR1_EL1);
	}

	if (NCPU == 1) {
		log_info("aarch64 smp: single cpu, MPIDR %#010llx",
		    (unsigned long long)aarch64_cpu_mpidr(self));
		return 0;
	}

	if (!psci_available() && SPINTABLE_BASE == 0) {
		log_warning("aarch64 smp: no PSCI conduit and no spin table, %d of %d "
		            "cpus stay down",
		    (int)NCPU - 1, (int)NCPU);
		return 0;
	}

	/* Before any secondary exists, because the handler table is global and a
	 * core that comes up only enables the line -- it does not attach to it. */
	ret = irq_attach(AARCH64_SGI_RESCHED, resched_ipi_handler, 0, NULL,
	    "smp resched");
	if (ret != 0) {
		log_error("aarch64 smp: cannot attach SGI %d: %d", AARCH64_SGI_RESCHED,
		    ret);
		return 0;
	}

	/* Which mechanism, said out loud: the two boards differ here and the
	 * difference is one option, so the log is where a reader finds out
	 * which one this image was built with. */
	if (SPINTABLE_BASE != 0) {
		log_info("aarch64 smp: cpu %u up, MPIDR %#010llx, spin table at %#x",
		    self, (unsigned long long)aarch64_cpu_mpidr(self),
		    (unsigned)SPINTABLE_BASE);
	}
	else {
		log_info("aarch64 smp: cpu %u up, MPIDR %#010llx, PSCI %s version %#lx",
		    self, (unsigned long long)aarch64_cpu_mpidr(self), psci_method(),
		    psci_version());
	}

#if SPINTABLE_BASE != 0
	spintable_map();
#endif

	for (i = 0; i < NCPU; i++) {
		if (i == self) {
			continue;
		}
		if (cpu_start(i) == 0) {
			up++;
		}
	}

#if SPINTABLE_BASE != 0
	/* Every core that was going to answer has answered by now: cpu_start()
	 * waits for its acknowledgement before returning. */
	spintable_unmap();
#endif

	log_info("aarch64 smp: %d of %d cpus up", up, (int)NCPU);

	return 0;
}
