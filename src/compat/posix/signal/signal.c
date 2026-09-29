/**
 * @file
 * @brief
 *
 * @date 08.10.2012
 * @author Anton Kozlov
 * @author Eldar Abusalimov
 */

#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stddef.h>
#include <pthread.h>

#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/thread/signal.h>
#include <kernel/task/resource/sig_table.h>

#include <util/math.h>

static void sighandler_ignore(int sig) {
	/* do nothing */
}

/* SIG_DFL is the handler the task started with for this signal
 * (task_resource_sig_default()), not one "terminate" for every signal: the
 * default of SIGCHLD, SIGURG and SIGWINCH is to ignore. And it has to be
 * recognised on the way out, or signal() on a fresh task answers with a
 * kernel function pointer where POSIX says SIG_DFL. */
int sigaction(int sig, const struct sigaction *restrict act,
		struct sigaction *restrict oact) {
	struct sigaction *sig_table = task_self_resource_sig_table();

	if (!check_range(sig, 1, _SIG_TOTAL) || IS_UNMODIFIABLE_SIGNAL(sig))
		return SET_ERRNO(EINVAL);

	if (oact) {
		sighandler_t ofunc = sig_table[sig].sa_handler;
		memcpy(oact, &sig_table[sig], sizeof(struct sigaction));

		if (ofunc == task_resource_sig_default(sig)) {
			ofunc = SIG_DFL;
		} else if (ofunc == sighandler_ignore) {
			ofunc = SIG_IGN;
		}

		oact->sa_handler = ofunc;
	}

	if (act) {
		sighandler_t func = act->sa_handler;
		memcpy(&sig_table[sig], act, sizeof(struct sigaction));

		if (func == SIG_DFL) {
			func = task_resource_sig_default(sig);
		} else if (func == SIG_IGN || func == SIG_ERR) {
			func = sighandler_ignore;
		}

		sig_table[sig].sa_handler = func;
	}

	return 0;
}

sighandler_t signal(int sig, sighandler_t func) {
	struct sigaction act  = { 0 };
	struct sigaction oact = { 0 };
	int err;

	act.sa_handler = func;

	err = sigaction(sig, &act, &oact);
	if (err) {
		return SIG_ERR;
	}

	return oact.sa_handler;
}

/* 0 or a negative error number. The callers below had passed that negative
 * number to SET_ERRNO(), so a bad signal left errno at -EINVAL. */
static int thread_signal(struct thread *thread, int sig,
		const siginfo_t *info) {
	int err;

	err = sigstate_send(&thread->sigstate, sig, info);
	if (err) {
		return err;
	}

	if (sig != 0) {
		sched_signal(&thread->schedee);
	}

	return 0;
}

int sigqueue(int tid, int sig, const union sigval value) {
	struct task *task;
	siginfo_t info;
	int err;

	task = task_table_get(tid);
	if (!task)
		return SET_ERRNO(ESRCH);

	// TODO prepare it
	info.si_value = value;

	err = thread_signal(task_get_main(task), sig, &info);
	if (err)
		return SET_ERRNO(-err);

	return 0;
}

/* POSIX: the error number is the return value; errno is left alone. */
int pthread_kill(pthread_t thread, int sig) {
	assert(thread);

	return -thread_signal(thread, sig, NULL);
}

int kill(int tid, int sig) {
	struct task *task;
	int err;

	if (!check_range(sig, 0, _SIG_TOTAL)) {
		return SET_ERRNO(EINVAL);
	}

	if (tid == 0) {
		/* The caller's process group. A task is its own group here. */
		task = task_self();
	}
	else if (tid == -1) {
		/* Every process the caller may signal, NYI */
		return SET_ERRNO(ENOSYS);
	}
	else {
		/* -pgid names the group led by that task, which is the task. */
		task = task_table_get(tid < 0 ? -tid : tid);
	}

	if (!task) {
		return SET_ERRNO(ESRCH);
	}

	/* With the null signal the checks above are the whole call. */
	err = thread_signal(task_get_main(task), sig, NULL);
	if (err) {
		return SET_ERRNO(-err);
	}

	return 0;
}

int raise(int signo) {
	int err;

	err = thread_signal(thread_self(), signo, NULL);
	if (err) {
		return SET_ERRNO(-err);
	}

	return 0;
}
