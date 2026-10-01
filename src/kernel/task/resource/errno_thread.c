/**
 * @file
 * @brief errno per thread, as POSIX has it
 *
 * With errno.c every thread of a task shares one word, and a thread that fails
 * a call and then reads why can read another thread's reason. This keeps the
 * word in the thread.
 */

#include <kernel/critical.h>
#include <kernel/sched/current.h>
#include <kernel/cpu/cpudata.h>
#include <kernel/task.h>
#include <kernel/thread.h>
#include <kernel/task/resource/errno.h>

/* Before the scheduler starts there is no thread, one CPU and nothing else
 * running: one word is enough. */
static int errno_boot;

/* An interrupt handler runs on the thread it interrupted, and so does an
 * lthread (the timers, the deferred halves of drivers): thread_self()
 * answers that thread. What they leave in errno must not land in the
 * thread's, which may be between a failed call and reading why. Neither
 * migrates while it runs, so a word per CPU each is enough -- two, because
 * an interrupt can land in an lthread. */
static int errno_irq __cpudata__;
static int errno_lthread __cpudata__;

int * task_resource_errno(const struct task *task) {
	struct thread *main = task_get_main(task);

	return main ? &main->errno_val : &errno_boot;
}

int * task_self_resource_errno(void) {
	struct thread *t;

	if (critical_inside(CRITICAL_IRQ_HANDLER)) {
		return cpudata_ptr(&errno_irq);
	}

	t = thread_self();
	if (!t) {
		return &errno_boot;
	}

	if (schedee_get_current() != &t->schedee) {
		return cpudata_ptr(&errno_lthread);
	}

	return &t->errno_val;
}
