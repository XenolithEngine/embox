/**
 * @file
 * @brief Does one core actually reach another, and does work reach the cores?
 *
 * Two properties that fail silently, which is why they are worth a suite:
 *
 *   - a reschedule IPI that is dropped looks exactly like one that was never
 *     needed. Nothing reports it; the system simply schedules a little worse.
 *   - a core that never picks up work looks exactly like a core with nothing
 *     to do. A scheduler that never spreads unpinned threads went a phase and
 *     a half unnoticed for that reason, with a green test suite the whole
 *     time -- because the test bound its workers to cores before measuring
 *     whether the scheduler would have.
 *
 * The IPI case has one complication and it decides the shape: the scheduler
 * tick already sends reschedule IPIs whenever it fires, so the counters are
 * moving before this touches anything. A case that only looked at "did the
 * counter go up" would pass on that alone. Hence the control interval -- the
 * same wait with nothing sent -- so the tick's own traffic shows up next to
 * the result rather than inside it.
 *
 * The per-CPU IPI counter is architecture-specific and declared weak: on a
 * port that does not have one the case reports that it could not look rather
 * than failing.
 */

#include <embox/test.h>

#include <hal/cpu.h>
#include <kernel/cpu/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched/affinity.h>
#include <kernel/thread.h>
#include <time.h>
#include <util/err.h>

EMBOX_TEST_SUITE("SMP: cores reached, and work spread over them");

extern void smp_send_resched(int cpu_id);

/* Aarch64 has one; another port may not. */
extern unsigned long aarch64_smp_ipi_count(unsigned int cpu_id)
    __attribute__((weak));

/* Long enough that a tick, and therefore its drift, has a fair chance of
 * landing inside the control interval. */
#define WAIT_MS 50

static void wait_ms(unsigned int ms) {
	clock_t deadline = clock() + (clock_t)ms;

	/* Spinning, not sleeping: sleeping hands this core to the idle thread and
	 * invites exactly the scheduler activity being measured. */
	while (clock() < deadline) {
	}
}

static int cpu_is_up(unsigned int i) {
	/* An idle thread is what cpu_init() leaves behind, so it is the one thing
	 * that tells a core the kernel has met from a slot in an array. */
	return (i != cpu_get_id()) && (cpu_get_idle(i) != NULL);
}

TEST_CASE("a reschedule IPI reaches every other CPU") {
	unsigned long base[NCPU], mid[NCPU], after[NCPU];
	unsigned long drift = 0;
	unsigned int i, targets = 0, answered = 0;
	clock_t deadline;

	if (&aarch64_smp_ipi_count == NULL) {
		printk("smp: no per-CPU IPI counter on this port; not looked at\n");
		return;
	}

	for (i = 0; i < NCPU; i++) {
		if (cpu_is_up(i)) {
			targets++;
		}
	}
	if (targets == 0) {
		return; /* alone: nothing to reach */
	}

	/* Control: the same interval, nothing sent. */
	for (i = 0; i < NCPU; i++) {
		base[i] = cpu_is_up(i) ? aarch64_smp_ipi_count(i) : 0;
	}
	wait_ms(WAIT_MS);
	for (i = 0; i < NCPU; i++) {
		mid[i] = cpu_is_up(i) ? aarch64_smp_ipi_count(i) : 0;
	}

	for (i = 0; i < NCPU; i++) {
		if (cpu_is_up(i)) {
			smp_send_resched((int)i);
		}
	}

	/* Wait for every counter to move, not for a fixed interval to pass and
	 * then compare totals: the tick delivers its own IPIs at a rate that
	 * varies between two windows of the same length, so "rose by more than
	 * the control did" is a coin flip and not a measurement. Waiting for the
	 * increment answers the question actually being asked -- did anything
	 * reach that core -- and the control interval is still reported, because
	 * a large drift is what would make this weak. */
	deadline = clock() + (clock_t)WAIT_MS;
	while (clock() < deadline) {
		int pending = 0;

		for (i = 0; i < NCPU; i++) {
			if (cpu_is_up(i) && (aarch64_smp_ipi_count(i) == mid[i])) {
				pending = 1;
			}
		}
		if (!pending) {
			break;
		}
	}

	for (i = 0; i < NCPU; i++) {
		after[i] = cpu_is_up(i) ? aarch64_smp_ipi_count(i) : 0;
	}

	for (i = 0; i < NCPU; i++) {
		if (!cpu_is_up(i)) {
			continue;
		}
		if (after[i] != mid[i]) {
			answered++;
		}
		drift += mid[i] - base[i];
	}

	printk("smp: %u of %u cpus answered a reschedule IPI; the tick delivered "
	       "%lu over the same interval with nothing sent\n",
	    answered, targets, drift);

	test_assert_equal(targets, answered);
}

/* ------------------------------------------------------------------ */

#define SPREAD_THREADS 4
#define SPREAD_ROUNDS  20000

static unsigned int spread_seen[SPREAD_THREADS];
static volatile int spread_go;

static void *spread_run(void *arg) {
	unsigned int *seen = arg;
	volatile unsigned long x = 0;
	int i;

	while (!spread_go) {
	}

	/* Counting work, no kernel calls in the loop: the question is where the
	 * scheduler puts a thread that is willing to run anywhere, and entering
	 * the kernel is what would move it. */
	for (i = 0; i < SPREAD_ROUNDS; i++) {
		x = x * 6364136223846793005UL + 1442695040888963407UL;
		*seen |= 1u << cpu_get_id();
	}
	(void)x;

	return NULL;
}

TEST_CASE("threads run only where their affinity allows, and are counted") {
	struct thread *t[SPREAD_THREADS];
	unsigned int used = 0, allowed;
	int i, cores = 0;

	spread_go = 0;
	for (i = 0; i < SPREAD_THREADS; i++) {
		spread_seen[i] = 0;
		t[i] = thread_create(THREAD_FLAG_SUSPENDED, spread_run,
		    &spread_seen[i]);
		test_assert_zero(ptr2err(t[i]));
	}

	allowed = (unsigned int)sched_affinity_get(&t[0]->schedee.affinity);

	for (i = 0; i < SPREAD_THREADS; i++) {
		thread_launch(t[i]);
	}
	spread_go = 1;

	for (i = 0; i < SPREAD_THREADS; i++) {
		test_assert_zero(thread_join(t[i], NULL));
		used |= spread_seen[i];
	}

	/* The property that holds in every configuration: a thread ran, and it
	 * ran where it was allowed to. Asserting that it ran on SEVERAL cores
	 * would be asserting a scheduling policy -- and on a kernel fenced to the
	 * boot core by affinity.smp(default_mask=1) it would be asserting the
	 * fence is off. */
	test_assert_not_zero(used);
	test_assert_zero(used & ~allowed);

	for (i = 0; i < NCPU; i++) {
		if (used & (1u << i)) {
			cores++;
		}
	}

	/* Reported, because "one core" is the answer that means the scheduler
	 * never spread anything -- and that was a real defect that a green suite
	 * hid for a phase and a half. */
	printk("smp: %d thread(s) ran on %d core(s), mask %#x of allowed %#x\n",
	    SPREAD_THREADS, cores, used, allowed);
}
