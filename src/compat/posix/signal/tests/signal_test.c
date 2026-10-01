/**
 * @file
 *
 * @brief
 *
 * @date 07.12.2011
 * @author Anton Bondarev
 */

#include <embox/test.h>

#include <errno.h>

#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include <kernel/task.h>
#include <kernel/time/ktime.h>

EMBOX_TEST_SUITE("test for task API");

static volatile int flag = 0;
static volatile char flag2 = 0;
static volatile unsigned long spins = 0;

/* SIGKILL cannot be caught: the handled signal is SIGUSR1, and SIGKILL is
 * what ends the task. */
static void sig_hnd(int sig) {
	flag = sig;
}

static void *task_hnd(void *arg) {

	signal(SIGUSR1, sig_hnd);

	while(1) {
		spins++;
		sleep(0);
	}

	return NULL;
}

TEST_CASE("create task, send it a signal it handles, then SIGKILL") {
	unsigned long before;
	int tid = new_task("", task_hnd, NULL);

	ksleep(100);
	kill(tid, SIGUSR1);
	ksleep(100);
	test_assert_equal(SIGUSR1, flag);

	kill(tid, SIGKILL);
	ksleep(100);
	/* The task is gone: its loop does not run. */
	before = spins;
	ksleep(100);
	test_assert_equal(before, spins);
}

TEST_CASE("SIGKILL cannot be caught") {
	errno = 0;
	test_assert(signal(SIGKILL, sig_hnd) == SIG_ERR);
	test_assert_equal(EINVAL, errno);
}

static void sig_hnd2(int sig) {
	flag2 ++;
}

static void *task_hnd2(void *arg) {

	signal(1, sig_hnd2);

	while(1)
		sleep(0);

	return NULL;
}

TEST_CASE("create task and send him signal 3 times") {
	int tid = new_task("", task_hnd2, NULL);
	flag2 = 0;

	ksleep(100);
	kill(tid, 1);

	ksleep(100);
	kill(tid, 1);

	ksleep(100);
	kill(tid, SIGKILL);

	test_assert(flag2 == 2);

}

static void *thread_hnd(void *arg) {

	ksleep(1000);

	task_exit(NULL);

	return NULL;
}

static void *task_hnd_thread(void *arg) {
	struct thread *thd;

	pthread_create(&thd, 0, thread_hnd, NULL);

	return NULL;
}

TEST_CASE("create a task with 2 threads, kill it") {
	 new_task("", task_hnd_thread, NULL);
}
