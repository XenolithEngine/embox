/**
 * @file
 * @brief
 *
 * @date 6.08.2014
 * @author Vita Loginova
 */

#include <util/log.h>

#include <hal/cpu.h>

#include <kernel/thread.h>
#include <kernel/thread/thread_stack.h>
#include <kernel/addr_space.h>
#include <kernel/panic.h>

static struct thread *saved_prev __cpudata__; // XXX

/**
 * Any fresh thread must call this function from a trampoline.
 * Basically it emulates returning from the scheduler as it would be done
 * in case if 'context_switch' would return as usual (into 'sched_switch',
 * from where it was called) instead of jumping into a thread trampoline.
 */

void thread_ack_switched(void) {
	ADDR_SPACE_FINISH_SWITCH();
	sched_finish_switch(&cpudata_var(saved_prev)->schedee);
	log_debug("first switch on cpu :%d",cpu_get_id());
	ipl_enable();
	/* we add first timer the same behavious as sched_ticker_update*/
	sched_ticker_add();
	sched_timing_start(&thread_self()->schedee);	
	sched_unlock();
}

static void thread_prepare_switch(struct thread *prev, struct thread *next) {
	prev->critical_count = critical_count();
	critical_count() = next->critical_count;
	sched_start_switch(&next->schedee);
	log_debug("thread prepare switch on cpu :%d",cpu_get_id());
}

extern void thread_set_current(struct thread *t);
/** Locks: IPL. */
void thread_context_switch(struct thread *prev, struct thread *next) {
	/* The thread leaving the core has done all it will do on its stack for
	 * now: if it went past the bottom, say so here, while the damage is one
	 * switch old and the thread is known. */
	if (!thread_stack_canary_ok(prev)) {
		panic("stack overflow: thread %d (%p), stack %p, %zu bytes\n",
		    prev->id, (void *)prev, thread_stack_get(prev),
		    thread_stack_get_size(prev));
	}

	thread_prepare_switch(prev, next);

	/* Preserve initial semantics of prev/next. */
	cpudata_var(saved_prev) = prev;
	ADDR_SPACE_PREPARE_SWITCH();

	thread_set_current(next);
	context_switch(&prev->context, &next->context);  /* implies cc barrier */

	ADDR_SPACE_FINISH_SWITCH();
	prev = cpudata_var(saved_prev);

	sched_finish_switch(&prev->schedee);
	log_debug("thread finish switch on cpu :%d",cpu_get_id());
}

