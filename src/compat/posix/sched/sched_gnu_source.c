/**
 * @file
 *
 * @date 5 feb. 2013
 * @author Anton Bondarev
 */

#include <errno.h>
#include <sched.h>

#include <kernel/cpu/cpu.h>
#include <kernel/sched.h>
#include <kernel/task.h>
#include <kernel/thread.h>

#include <hal/cpu.h>

int sched_getcpu(void) {
	uint32_t cpuid = cpu_get_id();
	return cpuid;
}

/* pid 0 or the caller's own task is the calling thread: a task here has its
 * threads' ids and not the other way round, and pinning one thread of a
 * task by the task's id would be a guess. They answered 0 and did nothing
 * (BF-44). */
static struct thread *sched_affinity_target(pid_t pid) {
	if (pid == 0 || pid == task_get_id(task_self())) {
		return thread_self();
	}
	return NULL;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
	struct thread *t = sched_affinity_target(pid);
	int err;

	if (!t) {
		return SET_ERRNO(ESRCH);
	}
	if (!mask || cpusetsize < sizeof(*mask)) {
		return SET_ERRNO(EINVAL);
	}
	err = thread_set_affinity(t, (int)*mask);
	return err ? SET_ERRNO(-err) : 0;
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask) {
	struct thread *t = sched_affinity_target(pid);

	if (!t) {
		return SET_ERRNO(ESRCH);
	}
	if (!mask || cpusetsize < sizeof(*mask)) {
		return SET_ERRNO(EINVAL);
	}
	*mask = (cpu_set_t)thread_get_affinity(t);
	return 0;
}
