/**
 * @file
 * @brief syscall(2) for the one number this header defines
 *
 * This used to answer 0 to everything. libc++abi asks syscall(SYS_gettid)
 * who is running a static-initialisation guard, so every thread was thread 0
 * to it, and two threads racing one guard would have been taken for one
 * thread initialising recursively.
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
