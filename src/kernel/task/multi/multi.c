/**
 * @file
 * @brief
 *
 * @date 31.08.11
 * @author Anton Kozlov
 */

#include <assert.h>
#include <compiler.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <framework/mod/options.h>
#include <kernel/nsproxy.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/sched/sched_lock.h>
#include <kernel/sched/schedee_priority.h>
#include <kernel/task.h>
#include <kernel/task/kernel_task.h>
#include <kernel/task/resource.h>
#include <kernel/task/resource/waitpid.h>
#include <kernel/sched/waitq.h>
#include <kernel/task/resource/errno.h>
#include <kernel/task/task_table.h>
#include <kernel/thread.h>
#include <util/binalign.h>
#include <util/err.h>
#include <util/atomic_rmw.h>
#include <util/log.h>
#include <hal/cpu.h>
#include <kernel/critical.h>
#include <kernel/time/ktime.h>

#if OPTION_GET(NUMBER, task_quantity)
extern struct thread *main_thread_create(unsigned int flags, size_t stack_sz,
    void *(*run)(void *), void *arg);
extern void main_thread_delete(struct thread *t);
#else
#define main_thread_create thread_create_with_stack
#define main_thread_delete thread_delete
#endif /* OPTION_GET(NUMBER, task_quantity) */

struct task_trampoline_arg {
	void *(*run)(void *);
	void *run_arg;
};

struct task *task_self(void) {
	struct thread *th = thread_self();

	if (!th) {
		/* Scheduler was not started yet */
		return task_kernel_task();
	}

	assert(th->task);

	return th->task;
}

struct task *task_find(pid_t pid) {
	struct task *task;

	if (pid == 0) {
		return task_self();
	}

	task_foreach(task) {
		if (pid == task->tsk_id) {
			return task;
		}
	}

	return NULL;
}

static void *task_trampoline(void *arg_) {
	struct task_trampoline_arg *arg = arg_;
	void *res;

	res = arg->run(arg->run_arg);
	task_exit(res);

	/* NOTREACHED */

	panic("Returning from task_trampoline()");

	return res;
}

static size_t task_get_stack_size(struct task *parent) {
	size_t stack_sz;

	if (parent) {
		stack_sz = task_getrlim_stack_size(parent);
	}
	else {
		stack_sz = THREAD_DEFAULT_STACK_SIZE;
	}

	return stack_sz;
}

int new_task(const char *name, void *(*run)(void *), void *arg) {
	struct task_trampoline_arg *trampoline_arg;
	struct thread *thd = NULL;
	struct task *self_task = NULL;
	int res, tid;
	size_t stack_sz;

	/**
	 * stack_sz has effect only when task_quantity has value
	 * AND (USE_USER_STACK == 0). Otherwise stack_sz will be
	 * replaced by STACK_SZ
	 */
	stack_sz = task_get_stack_size(task_self());
	stack_sz += sizeof(struct task) + TASK_RESOURCE_SIZE;

#if OPTION_GET(NUMBER, task_quantity)
	assertf(OPTION_GET(NUMBER, resource_size) == TASK_RESOURCE_SIZE,
	    "resource_size (%d) must be set up %d",
	    OPTION_GET(NUMBER, resource_size), TASK_RESOURCE_SIZE);
#endif /* OPTION_GET(NUMBER, task_quantity) */

	sched_lock();
	{
		if (!task_table_has_space()) {
			res = -ENOMEM;
			goto out_unlock;
		}

		/*
		 * Thread does not run until we go through sched_unlock()
		 */
		thd = main_thread_create(THREAD_FLAG_NOTASK | THREAD_FLAG_SUSPENDED,
		    stack_sz, task_trampoline, NULL);
		if (0 != ptr2err(thd)) {
			res = ptr2err(thd);
			goto out_unlock;
		}

		trampoline_arg = thread_stack_alloc(thd, sizeof *trampoline_arg);
		if (trampoline_arg == NULL) {
			res = -ENOMEM;
			goto out_threadfree;
		}

		trampoline_arg->run = run;
		trampoline_arg->run_arg = arg;
		thread_set_run_arg(thd, trampoline_arg);

		self_task = thread_stack_alloc(thd, sizeof *self_task + TASK_RESOURCE_SIZE);
		if (self_task == NULL) {
			res = -ENOMEM;
			goto out_threadfree;
		}

		tid = task_table_add(self_task);
		if (tid < 0) {
			res = tid;
			goto out_threadfree;
		}

		task_init(self_task, tid, task_self(), name, thd,
		    task_self()->tsk_priority);

		res = task_resource_inherit(self_task, task_self());
		if (res != 0) {
			goto out_tablefree;
		}

		schedee_priority_set(&thd->schedee,
		    schedee_priority_get(&thread_self()->schedee));

		thread_detach(thd);
		thread_launch(thd);

		res = self_task->tsk_id;

		goto out_unlock;

out_tablefree:
		task_table_del(tid);

out_threadfree:
		thread_terminate(thd);
	}
out_unlock:
	sched_unlock();

	return res;
}

int task_start(struct task *task, void *(*run)(void *), void *arg) {
	struct task_trampoline_arg *trampoline_arg;
	struct thread *thd = NULL;
	int res;

	sched_lock();
	{
		thd = task->tsk_main;
		trampoline_arg = thread_stack_alloc(thd, sizeof *trampoline_arg);
		if (trampoline_arg == NULL) {
			res = -ENOMEM;
			goto out_threadfree;
		}

		trampoline_arg->run = run;
		trampoline_arg->run_arg = arg;
		thread_set_run_arg(thd, trampoline_arg);

		thread_detach(thd);
		thread_launch(thd);

		res = 0;

		goto out_unlock;
out_threadfree:
		thread_terminate(thd);
	}
out_unlock:
	sched_unlock();

	return res;
}

int task_prepare(const char *name) {
	struct thread *thd = NULL;
	struct task *self_task = NULL;
	int res, tid;
	size_t stack_sz;

	/**
	 * stack_sz has effect only when task_quantity has value
	 * AND (USE_USER_STACK == 0). Otherwise stack_sz will be
	 * replaced by STACK_SZ
	 */
	stack_sz = task_get_stack_size(task_self());
	stack_sz += sizeof(struct task) + TASK_RESOURCE_SIZE;

	sched_lock();
	{
		if (!task_table_has_space()) {
			res = -ENOMEM;
			goto out_unlock;
		}

		/*
		 * Thread does not run until we go through sched_unlock()
		 */
		thd = main_thread_create(THREAD_FLAG_NOTASK | THREAD_FLAG_SUSPENDED,
		    stack_sz, task_trampoline, NULL);
		if (0 != ptr2err(thd)) {
			res = ptr2err(thd);
			goto out_unlock;
		}

		schedee_priority_set(&thd->schedee,
		    schedee_priority_get(&thread_self()->schedee));

		self_task = thread_stack_alloc(thd, sizeof *self_task + TASK_RESOURCE_SIZE);
		if (self_task == NULL) {
			res = -ENOMEM;
			goto out_threadfree;
		}

		tid = task_table_add(self_task);
		if (tid < 0) {
			res = tid;
			goto out_threadfree;
		}

		task_init(self_task, tid, task_self(), name, thd,
		    task_self()->tsk_priority);

		res = task_resource_inherit(self_task, task_self());
		if (res != 0) {
			goto out_tablefree;
		}

		res = self_task->tsk_id;

		goto out_unlock;

out_tablefree:
		task_table_del(tid);

out_threadfree:
		thread_terminate(thd);
	}
out_unlock:
	sched_unlock();

	return res;
}

void task_init(struct task *tsk, int id, struct task *parent, const char *name,
    struct thread *main_thread, task_priority_t priority) {
	assert(tsk != NULL);
	assert(binalign_check_bound((uintptr_t)tsk, sizeof(void *)));

	tsk->tsk_id = id;
	tsk->status = 0;
	tsk->tsk_exiting = 0;

	dlist_init(&tsk->child_list);
	dlist_head_init(&tsk->child_lnk);

	task_set_name(tsk, name);

	if (main_thread) {
		tsk->tsk_main = main_thread;
		main_thread->task = tsk;
	}

	tsk->parent = parent;
	if (parent) {
		dlist_add_prev(&tsk->child_lnk, &parent->child_list);
	}

#if defined(NET_NAMESPACE_ENABLED) && (NET_NAMESPACE_ENABLED == 1)
	set_task_proxy(tsk, parent);
#endif

	task_setrlim_stack_size(tsk, task_get_stack_size(parent));

	tsk->tsk_priority = priority;

	tsk->tsk_clock = 0;

	task_resource_init(tsk);
}

static void task_make_children_daemons(struct task *task) {
	struct task *child = NULL;
	struct task *krn_task = task_kernel_task();

	dlist_foreach_entry(child, &task->child_list, child_lnk) {
		dlist_del_init(&child->child_lnk);

		child->parent = krn_task;
		dlist_add_prev(&child->child_lnk, &krn_task->child_list);
	}
}

int task_thread_killed(struct thread *t) {
	struct task *task;

	if (!t || !(task = t->task)) {
		return 0;
	}
	return atomic_rmw_load(&task->tsk_exiting, __ATOMIC_ACQUIRE)
	       && (task->tsk_exiter != t);
}

/* Off every CPU and not runnable: blocked, or through thread_exit(). */
static inline int task_thread_quiet(struct thread *t) {
	return t->schedee.released && !t->schedee.ready;
}

/* Stop every other thread of the exiting task before anything is taken from
 * it -- close the window thread_terminate() could only narrow.
 *
 * thread_terminate() cannot stop a thread that is running on another core,
 * and it abandons a blocked one mid-wait, with whatever it had linked on its
 * stack still linked. Freeing the task's resources after that let the other
 * thread walk freed page tables in a system call and read a freed errno (A1:
 * libc++'s thread.thread.member/detach on four cores took the kernel down).
 *
 * So the threads are made to leave on their own. They are killed
 * (task_thread_killed(): tsk_exiter is set, tsk_exiting already is), which
 * ends any wait they are in with -EINTR, and a killed thread is diverted to
 * thread_exit() wherever it would go back to user mode (the aarch64 exception
 * return paths). The blocked ones are woken to get there; the running ones
 * get a reschedule IPI, whose interrupt return is such a point. This waits,
 * with the scheduler lock released so that they can run, until every one of
 * them is quiet -- off every CPU and not runnable -- and returns holding the
 * lock again, as it was called.
 *
 * A thread that does not come -- blocked in a wait that is not a
 * sched_wait() and so cannot be interrupted -- is quiet too, and is torn down
 * as before. One that stays runnable is reported after five seconds and the
 * teardown goes ahead: the old behaviour, now the exception. Five, not one:
 * on one core the scheduler slices every 100 ms, and a task with a handful of
 * threads spinning in user mode needs several slices before each of them has
 * been on the CPU once to see it is killed. */
static void task_quiesce(struct task *task, struct thread *main_thr) {
	struct thread *self = thread_self();
	struct thread *thr;
	unsigned int depth;
	time64_t deadline;
	int busy, running;

	task->tsk_exiter = self;

	/* The lock can only be given up if this is its one level: the callers
	 * take it in task_start_exit() and nothing else. */
	depth = (critical_count() & CRITICAL_SCHED_LOCK)
	        / __CRITICAL_COUNT(CRITICAL_SCHED_LOCK);
	if (depth != 1) {
		log_error("task %d: exiting with the scheduler lock held %u times; "
		          "not waiting for its threads", task->tsk_id, depth);
		return;
	}

	if ((main_thr != self) && main_thr->schedee.waiting
	    && !main_thr->schedee.finished) {
		sched_wakeup(&main_thr->schedee);
	}
	dlist_foreach_entry(thr, &main_thr->thread_link, thread_link) {
		if ((thr != self) && thr->schedee.waiting && !thr->schedee.finished) {
			sched_wakeup(&thr->schedee);
		}
	}

	deadline = ktime_get_ns() + 5000000000LL;
	for (;;) {
		busy = running = 0;
		if ((main_thr != self) && !task_thread_quiet(main_thr)) {
			busy++;
			running |= main_thr->schedee.active;
		}
		dlist_foreach_entry(thr, &main_thr->thread_link, thread_link) {
			if ((thr != self) && !task_thread_quiet(thr)) {
				busy++;
				running |= thr->schedee.active;
			}
		}
		if (!busy) {
			return;
		}
		if (ktime_get_ns() > deadline) {
			log_error("task %d: %d thread(s) still runnable five seconds into "
			          "its exit; tearing it down anyway", task->tsk_id, busy);
			return;
		}
#ifdef SMP
		if (running) {
			extern void smp_send_resched(int cpu_id);
			int cpu;

			for (cpu = 0; cpu < NCPU; cpu++) {
				if (cpu != (int)cpu_get_id()) {
					smp_send_resched(cpu);
				}
			}
		}
#endif
		sched_unlock();
		ksleep(1);
		sched_lock();
	}
}

void task_do_exit(struct task *task, int status) {
	struct thread *thr = NULL;
	struct thread *main_thr;

	assert(critical_inside(CRITICAL_SCHED_LOCK));

	/* The first exit decides; the rest only find out.
	 *
	 * thread_terminate() below cannot stop a thread that is running on another
	 * core. It takes the schedee out of the run queue and marks it, and the
	 * thread carries on until it next schedules -- there is no interrupting a
	 * core from here. So while one thread tears the task down, another can
	 * still reach the end of its own function, and task_trampoline() ends by
	 * calling task_exit() again, with the return value of a thread whose task
	 * is already gone.
	 *
	 * POSIX says the same thing about exit(): whichever thread calls it decides
	 * the status, and a later return from another thread does not change it. */
	if (atomic_rmw_exchange(&task->tsk_exiting, 1, __ATOMIC_ACQ_REL)) {
		/* Nothing left to tear down and nothing to say about the status. Stop
		 * this thread anyway: the caller goes on to task_finish_exit(), whose
		 * schedule() must not come back. */
		thread_terminate(thread_self());
		return;
	}

	/* Only past the latch: by the time a second thread gets here the first has
	 * already unregistered the main thread, which sets tsk_main to NULL. */
	main_thr = task->tsk_main;
	assert(main_thr);

	/* Every other thread of the task off the CPUs and out of the kernel
	 * first; see task_quiesce(). */
	task_quiesce(task, main_thr);

	/* Stop the threads BEFORE taking the resources away from them.
	 *
	 * Upstream deinitialises the resources first -- files, the task heap, the
	 * lot -- and terminates the threads afterwards. On one core that order is
	 * invisible, because no other thread of the task can be running while this
	 * one is inside task_do_exit(). On four it is exactly backwards: the other
	 * threads are running, and what is being pulled out from under them is the
	 * heap their next free() looks in.
	 *
	 * thread_terminate() cannot stop a thread that is running on another core
	 * this instant -- it takes the schedee out of the run queue and marks it,
	 * and the thread carries on until it next schedules. That used to be the
	 * window this left open; task_quiesce() above has now closed it, and every
	 * thread terminated here is already off the CPUs and out of the kernel. */
	dlist_foreach_entry(thr, &main_thr->thread_link, thread_link) {
		thread_terminate(thr);
		thread_delete(thr);
	}

	/* Deinitialize all resources */
	task_resource_deinit(task);

	/* Make all children of the specified task daemons (or, more generally, orphans).
	 * It is made by simply setting up the parent task of each child to kernel_task. */
	task_make_children_daemons(task);

	/* At the end terminate main thread */
	thread_terminate(main_thr);

	/* Only now may the parent collect this task.
	 *
	 * `struct task' is not allocated on its own -- task_create() carves it out
	 * of the main thread's stack, so freeing the main thread frees the task
	 * with it. task_collect() reaps any child whose status carries
	 * TASKST_EXITED_MASK and calls task_delete(), which does exactly that free.
	 *
	 * Upstream sets the status first and wakes the parent from the middle of
	 * task_resource_deinit() (the waitpid resource's deinit op), both long
	 * before the threads are torn down. On one core that is harmless, because
	 * the parent cannot run until the exiting thread blocks. On four it is a
	 * use-after-free of the task and of the main thread.
	 *
	 * So the status is published here, after every thread of the task has been
	 * terminated, and the parent is woken here rather than from a resource
	 * deinit that runs in the middle of the teardown. Whoever is still standing
	 * on a stack is parked by thread_delete() as before. */
	atomic_rmw_store(&task->status, status, __ATOMIC_RELEASE);

	if (task->parent) {
		waitq_wakeup_all(task_resource_waitpid(task->parent));
	}
}

void task_start_exit(void) {
	assert(task_self() != task_kernel_task());

	sched_lock();

	assert(critical_inside(CRITICAL_SCHED_LOCK));
}

void _NORETURN task_finish_exit(void) {
	assert(critical_inside(CRITICAL_SCHED_LOCK));

	/* Re-schedule */
	schedule();

	sched_unlock();

	/* NOTREACHED */
	panic("Returning from task_exit()");
}

void _NORETURN task_exit(void *res) {
	task_start_exit();

	task_do_exit(task_self(),
	    TASKST_EXITED_MASK | ((intptr_t)res & TASKST_EXITST_MASK));

	task_finish_exit();
}

void task_delete(struct task *tsk) {
	netns_decrement_ref_cnt(tsk->nsproxy.net_ns);
	dlist_del(&tsk->child_lnk);
	task_table_del(task_get_id(tsk));
	main_thread_delete(task_get_main(tsk));
}

int task_set_priority(struct task *tsk, task_priority_t new_prior) {
	struct thread *t;

	assert(tsk);

	if ((new_prior < TASK_PRIORITY_MIN) || (new_prior > TASK_PRIORITY_MAX)) {
		return -EINVAL;
	}

	sched_lock();
	{
		if (tsk->tsk_priority == new_prior) {
			sched_unlock();
			return 0;
		}

		tsk->tsk_priority = new_prior;

		task_foreach_thread(t, tsk) {
			/* reschedule thread */
			schedee_priority_set(&t->schedee, SCHED_OTHER_PRIORITY_NORM + new_prior);
		}
	}
	sched_unlock();

	return 0;
}
