/**
 * @file
 * @brief
 *
 * @date 19.07.13
 * @author Ilia Vaprol
 */

#include <errno.h>

#include <hal/cpu.h>
#include <kernel/cpu/cpu.h>
#include <kernel/cpu/cpudata.h>
#include <kernel/sched/affinity.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <time.h>

static struct thread *idle __cpudata__ = NULL;
static clock_t started __cpudata__;

void cpu_init(unsigned int cpu_id, struct thread *idle_) {
	cpu_bind(cpu_id, idle_);
	cpudata_cpu_var(cpu_id, idle) = idle_;
	cpudata_cpu_var(cpu_id, started) = clock();
}

void cpu_bind(unsigned int cpu_id, struct thread *t) {
	//t->affinity = 1 << cpu_id;
	sched_affinity_set(&t->schedee.affinity, 1 << cpu_id);
}

/* The cores that have started: an idle thread is what a started core has. */
static int cpu_started_mask(void) {
	int mask = 0;
	unsigned int i;

	for (i = 0; i < NCPU && i < 8 * sizeof(int); i++) {
		if (cpudata_cpu_var(i, idle) != NULL) {
			mask |= 1 << i;
		}
	}
	return mask ? mask : 1 << cpu_get_id();
}

/* Pin T to the cores in MASK that have started. cpu_bind() for a set, and
 * for a thread that is running: the mask is a field the scheduler reads the
 * next time it picks, so a thread that narrows its own and is on a core it
 * may no longer use gives that core up now instead of at the next tick.
 * A thread on another core moves at that core's next pick. */
int thread_set_affinity(struct thread *t, int mask) {
	mask &= cpu_started_mask();
	if (mask == 0) {
		return -EINVAL;
	}
	sched_affinity_set(&t->schedee.affinity, mask);
	if (t == thread_self() && !(mask & (1 << cpu_get_id()))) {
		schedule();
	}
	return 0;
}

/* The cores T may run on, of those that have started. */
int thread_get_affinity(struct thread *t) {
#ifdef SCHEDEE_AFFINITY_NONE
	return sched_affinity_get(&t->schedee.affinity) & cpu_started_mask();
#else
	(void)t;
	return cpu_started_mask();
#endif
}

struct thread * cpu_get_idle(unsigned int cpu_id) {
	return cpudata_cpu_var(cpu_id, idle);
}

clock_t cpu_get_started(unsigned int cpu_id) {
	return cpudata_cpu_var(cpu_id, started);
}
