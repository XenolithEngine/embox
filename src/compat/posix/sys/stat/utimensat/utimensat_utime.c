/**
 * @file
 * @brief utimensat() over utime().
 *
 * Only the modification time is kept -- nothing records access -- and in
 * whole seconds. There are no directory descriptors, so a relative path is
 * taken from the working directory and anything but AT_FDCWD is refused.
 *
 * @date 29.09.2026
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>

static int utimensat_valid(const struct timespec *ts) {
	return ts->tv_nsec == UTIME_NOW || ts->tv_nsec == UTIME_OMIT
	       || (ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000L);
}

int utimensat(int dirfd, const char *path, const struct timespec times[2],
    int flags) {
	struct utimbuf ub;
	struct stat st;

	(void)flags; /* AT_SYMLINK_NOFOLLOW: there are no symbolic links */

	if (path == NULL) {
		return SET_ERRNO(EFAULT);
	}
	if (path[0] != '/' && dirfd != AT_FDCWD) {
		return SET_ERRNO(dirfd >= 0 ? ENOTDIR : EBADF);
	}
	if (times == NULL) {
		return utime(path, NULL);
	}
	if (!utimensat_valid(&times[0]) || !utimensat_valid(&times[1])) {
		return SET_ERRNO(EINVAL);
	}

	if (times[1].tv_nsec == UTIME_OMIT) {
		/* The modification time stays; the access time is not kept. Still
		 * an error if the file is not there. */
		return stat(path, &st);
	}

	ub.modtime = (times[1].tv_nsec == UTIME_NOW) ? time(NULL) : times[1].tv_sec;
	ub.actime = (times[0].tv_nsec == UTIME_NOW || times[0].tv_nsec == UTIME_OMIT)
	                ? ub.modtime : times[0].tv_sec;

	return utime(path, &ub);
}
