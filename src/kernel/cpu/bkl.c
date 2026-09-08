/**
 * @file
 * @brief Implementation of Big Kernel Lock
 *
 * @date 08.02.12
 * @author Anton Bulychev
 * @author Ilia Vaprol
 */

#include <assert.h>

#include <hal/ipl.h>
#include <hal/cpu.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <util/atomic_rmw.h>

static spinlock_t bkl = SPIN_STATIC_UNLOCKED;

void bkl_lock(void) {
	__spin_lock(&bkl);
}

void bkl_unlock(void) {
	__spin_unlock(&bkl);
}

int bkl_trylock(void) {
	return __spin_trylock(&bkl);
}

void bkl_wait(void) {
	int spins = 64;

	/* A read-spin: the line stays shared, so a waiting core does not fight the
	 * holder for it the way a compare-exchange loop does. Bounded, and with no
	 * accounting of its own, so the caller's trylock keeps feeding the
	 * contention detector at about the rate the old loop did -- 64 relaxed
	 * loads cost roughly what one failed exclusive store costs, and a detector
	 * calibrated in iterations would otherwise call a deadlock an order of
	 * magnitude too early. */
	while (spins-- > 0
	       && atomic_load(&bkl.l, __ATOMIC_RELAXED) != __SPIN_UNLOCKED) {
	}
}

int bkl_owned(void) {
	return bkl.owner == cpu_get_id();
}

/* A non-zero critical count on this CPU is a promise that this CPU holds the
 * BKL. Reaching a nested critical_enter()/critical_leave() without owning it
 * is that promise being broken. */
void bkl_assert_owned(unsigned int tested, unsigned int unit, void *from) {
	/* critical_enter()/critical_leave() are inlined into their caller, so the
	 * return address names the call site that broke the invariant. `tested` is
	 * the count the branch actually looked at and `critical_count()` the count
	 * now: the two differ only if this CPU changed hands in between. */
	/* The message buffer is 128 bytes, so this is terse on purpose: the count
	 * the branch tested and the count now, the level's unit, this CPU and the
	 * lock's owner, the thread and its state, and the return address -- which
	 * names the call site. It is passed in rather than taken here, because at
	 * -O0 critical_enter() is a real function and its own return address would
	 * name only itself. */
	assertf(bkl.owner == cpu_get_id(),
	    "c %#x/%#x u %#x cpu %u bkl %u th %p st %#x @%p", tested,
	    (unsigned int)critical_count(), unit, cpu_get_id(), bkl.owner,
	    thread_self(), thread_self() ? thread_self()->state : 0, from);
}
