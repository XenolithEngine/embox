/**
 * @file
 * @brief A thread pinned with pthread_setaffinity_np() runs where it is told.
 *
 * The POSIX calls were stubs, and the scheduler kept a thread on a core its
 * mask had just excluded whenever an equal-priority thread was queued there
 * (BF-44). Needs three started cores.
 *
 * @date 29.09.2026
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>

#include <embox/test.h>
#include <hal/cpu.h>
#include <kernel/cpu/cpu.h>

EMBOX_TEST_SUITE("pthread_setaffinity_np pins a thread to a core");

#define ITERS 100000

static int cores_started(void) {
	int n = 0;
	unsigned int i;

	for (i = 0; i < NCPU; i++) {
		n += cpu_get_idle(i) != NULL;
	}
	return n;
}

static void pin(unsigned int cpu) {
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	test_assert_zero(pthread_setaffinity_np(pthread_self(), sizeof(set), &set));
}

static void *stays_on_2(void *arg) {
	unsigned long off = 0;
	cpu_set_t back;
	int i;

	pin(2);
	for (i = 0; i < ITERS; i++) {
		off += sched_getcpu() != 2;
		if ((i & 1023) == 0) {
			sched_yield();
		}
	}
	test_assert_zero(pthread_getaffinity_np(pthread_self(), sizeof(back), &back));
	test_assert_equal(1 << 2, (int)back);
	return (void *)off;
}

TEST_CASE("a thread pinned to core 2 runs on core 2 and reads its mask back") {
	pthread_t t;
	void *off;

	if (cores_started() < 3) {
		return;
	}
	test_assert_zero(pthread_create(&t, NULL, stays_on_2, NULL));
	test_assert_zero(pthread_join(t, &off));
	test_assert_equal(0, (int)(uintptr_t)off);
}

static volatile int spin_stop;

static void *spins_on_1(void *arg) {
	pin(1);
	while (!spin_stop) {
	}
	return NULL;
}

static void *moves_to_2(void *arg) {
	int where;

	pin(1);
	/* An equal-priority thread is queued for core 1 while this runs there:
	 * the case where the scheduler used to keep this thread where it was. */
	pin(2);
	where = sched_getcpu();
	return (void *)(uintptr_t)where;
}

TEST_CASE("a thread moves at once when an equal-priority one waits for its old core") {
	pthread_t spinner, mover;
	void *where;

	if (cores_started() < 3) {
		return;
	}
	spin_stop = 0;
	test_assert_zero(pthread_create(&spinner, NULL, spins_on_1, NULL));
	test_assert_zero(pthread_create(&mover, NULL, moves_to_2, NULL));
	test_assert_zero(pthread_join(mover, &where));
	spin_stop = 1;
	test_assert_zero(pthread_join(spinner, NULL));
	test_assert_equal(2, (int)(uintptr_t)where);
}

TEST_CASE("a mask with no started core is refused") {
	cpu_set_t none;

	if (cores_started() >= (int)CPU_SETSIZE) {
		return;
	}
	CPU_ZERO(&none);
	CPU_SET(CPU_SETSIZE - 1, &none);
	test_assert_equal(EINVAL,
	    pthread_setaffinity_np(pthread_self(), sizeof(none), &none));
}
