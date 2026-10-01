/**
 * @file
 * @brief Std signals.
 *
 * @date 07.10.2013
 * @author Eldar Abusalimov
 */

#include <kernel/thread/signal.h>

#include <assert.h>
#include <errno.h>

#include <kernel/irq_lock.h>

#include <lib/libds/bit.h>
#include <lib/libds/array.h>
#include <util/math.h>

struct sigstate *sigstate_init(struct sigstate *sigstate) {
	assert(sigstate);

	sigemptyset(&sigstate->pending);
	siginfoq_init(&sigstate->infoq);

	return sigstate;
}

int sigstate_send(struct sigstate *sigstate, int sig, const siginfo_t *info) {
	int err = 0;

	assert(sigstate);

	if (sig == 0) {
		/*
		 if sig is zero, error checking shall be performed
		 but no signal shall actually be sent.
		 */
		return 0;
	}

	if (!check_range(sig, 1, _SIG_TOTAL))
		return -EINVAL;

	irq_lock();

	if (info)
		err = siginfoq_enqueue(&sigstate->infoq, sig, info);
	if (!err)
		sigaddset(&sigstate->pending, sig);

	irq_unlock();

	return err;
}

static int sigset_first(sigset_t *set) {
	int sig;

	sig = bitmap_find_first_bit(set->bitmap, _SIG_TOTAL);
	if (sig == _SIG_TOTAL)
		sig = 0;  /* no pending signal */
	assert(check_range(sig, 0, _SIG_TOTAL));

	return sig;
}

/* Nothing pending, read without the lock. It is the answer almost every time
 * -- every sleep and every threadsig_unlock() asks -- and irq_lock() and
 * irq_unlock() for it cost about a microsecond on the A72.
 *
 * A bit this read misses was set after it, as far as this thread can tell:
 * the same answer the locked read would have given had it run a moment
 * earlier. The sender then calls sched_signal(), and the thread handles the
 * signal the next time it is switched to (thread_process()). A signal that
 * woke a sleeper is not missed: it was set before sched_wakeup(), and the
 * sleeper got back to its CPU through the run queue's lock after that. */
static int sigstate_nothing_pending(struct sigstate *sigstate) {
	size_t i;

	for (i = 0; i < ARRAY_SIZE(sigstate->pending.bitmap); i++) {
		if (__atomic_load_n(&sigstate->pending.bitmap[i], __ATOMIC_RELAXED)) {
			return 0;
		}
	}
	return 1;
}

int sigstate_receive(struct sigstate *sigstate, siginfo_t *info) {
	int sig;
	int nr_left;

	assert(sigstate && info);

	if (sigstate_nothing_pending(sigstate)) {
		return 0;
	}

	irq_lock();

	sig = sigset_first(&sigstate->pending);
	if (sig) {
		nr_left = siginfoq_dequeue(&sigstate->infoq, sig, info);
		if (nr_left <= 0)
			/* no more (or no at all) pending signals for this signo */
			sigdelset(&sigstate->pending, sig);

		if (nr_left < 0) {
			/* signal was not queued, fill in siginfo by hand */
			info->si_signo = sig;
			// TODO the rest
		}
	}

	irq_unlock();

	return sig;
}

