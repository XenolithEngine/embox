/**
 * @file
 * @brief utimes() over utime(): the file system keeps whole seconds.
 *
 * @date 29.09.2026
 */

#include <errno.h>
#include <stddef.h>
#include <sys/time.h>
#include <utime.h>

int utimes(const char *path, const struct timeval times[2]) {
	struct utimbuf ub;

	if (times == NULL) {
		return utime(path, NULL);
	}
	if (times[0].tv_usec < 0 || times[0].tv_usec >= 1000000
	    || times[1].tv_usec < 0 || times[1].tv_usec >= 1000000) {
		return SET_ERRNO(EINVAL);
	}

	ub.actime = times[0].tv_sec;
	ub.modtime = times[1].tv_sec;

	return utime(path, &ub);
}
