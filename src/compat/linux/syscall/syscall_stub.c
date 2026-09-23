/**
 * @file
 * @brief syscall(2) for the one number this header defines
 *
 * libc++abi asks syscall(SYS_gettid) who is running a static-initialisation
 * guard, so each thread has to get its own id.
 */

#include <errno.h>
#include <sys/syscall.h>

#include <kernel/thread.h>

long syscall(long number, ...) {
	switch (number) {
	case SYS_gettid:
		/* Thread ids start at 1 (kernel/thread/core.c), and 0 is what
		 * libc++abi reads as "no owner". */
		return thread_self()->id;
	default:
		errno = ENOSYS;
		return -1;
	}
}
