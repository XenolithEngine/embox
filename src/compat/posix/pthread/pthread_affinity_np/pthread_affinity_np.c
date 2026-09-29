/**
 * @file
 * @brief pthread_{set,get}affinity_np over the scheduler's own mask.
 *
 * cpu_set_t is one word here, bit i = core i. The stubs next to this answered
 * 0 and pinned nothing, and taskset writes a task resource the scheduler
 * never reads; schedee.affinity is the one mask that is obeyed (BF-44).
 *
 * @date 29.09.2026
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>

#include <kernel/cpu/cpu.h>
#include <kernel/thread.h>

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
    const cpu_set_t *cpuset) {
	if (!thread || !cpuset || cpusetsize < sizeof(*cpuset)) {
		return EINVAL;
	}
	return -thread_set_affinity(thread, (int)*cpuset);
}

int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize,
    cpu_set_t *cpuset) {
	if (!thread || !cpuset || cpusetsize < sizeof(*cpuset)) {
		return EINVAL;
	}
	*cpuset = (cpu_set_t)thread_get_affinity(thread);
	return 0;
}
