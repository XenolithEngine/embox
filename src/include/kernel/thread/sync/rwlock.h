/**
 * @file
 * @brief Defines read-write lock structure and methods associated with it.
 *
 * @date 04.09.12
 * @author Anton Bulychev
 */

#ifndef KERNEL_THREAD_SYNC_RWLOCK_H_
#define KERNEL_THREAD_SYNC_RWLOCK_H_

#include <kernel/sched/waitq.h>

struct rwlock {
	struct waitq wq;
	int status;
	int count;
};

typedef struct rwlock rwlock_t;

/* What rwlock_init() leaves, for a lock defined statically (the POSIX
 * PTHREAD_RWLOCK_INITIALIZER). All zeroes is not it: the wait queue's
 * spinlock has no owner as -1. Status 0 is RWLOCK_STATUS_NONE. */
#define RWLOCK_INIT_STATIC \
	{                                                  \
		{ /* wait_queue init */                        \
			DLIST_INIT_NULL(),                         \
			/* spinlock_t lock*/                       \
			{ /*l*/__SPIN_UNLOCKED,                    \
				/* owner */ (unsigned int)-1,          \
				/*contention_count */SPIN_CONTENTION_LIMIT \
			}                                          \
		},                                             \
		/* status */ 0,                                \
		/* count */ 0                                  \
	}

extern void rwlock_init(rwlock_t *r);
extern void rwlock_read_up(rwlock_t *r);
extern void rwlock_read_down(rwlock_t *r);
extern void rwlock_write_up(rwlock_t *r);
extern void rwlock_write_down(rwlock_t *r);
extern void rwlock_any_down(rwlock_t *r);


#endif /* KERNEL_THREAD_SYNC_RWLOCK_H_ */
