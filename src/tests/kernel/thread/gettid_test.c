/**
 * @file
 * @brief syscall(SYS_gettid) names the calling thread
 *
 * libc++abi asks it who is running a static-initialisation guard. It used to
 * answer the same for every thread, so two threads racing one guard would
 * have looked like one thread initialising recursively.
 */

#include <stdint.h>
#include <sys/syscall.h>

#include <embox/test.h>
#include <kernel/thread.h>
#include <util/err.h>

EMBOX_TEST_SUITE("syscall(SYS_gettid)");

static void *other_tid(void *arg) {
	*(long *)arg = syscall(SYS_gettid);
	return NULL;
}

TEST_CASE("it is this thread's id, not zero") {
	long tid = syscall(SYS_gettid);

	test_assert(tid > 0);
	test_assert_equal(thread_self()->id, tid);
}

TEST_CASE("another thread gets another id") {
	struct thread *t;
	long mine = syscall(SYS_gettid);
	long theirs = 0;
	long id;

	t = thread_create(0, other_tid, &theirs);
	test_assert_zero(ptr2err(t));
	id = t->id;   /* read before the join, which may free the thread */
	test_assert_zero(thread_join(t, NULL));

	test_assert(theirs > 0);
	test_assert(theirs != mine);
	test_assert_equal(id, theirs);
}
