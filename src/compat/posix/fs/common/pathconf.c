/**
 * @file
 * @brief pathconf() and fpathconf(): the limits of a file's filesystem.
 *
 * One tree of names serves every filesystem here, so the answers are the
 * build's own limits whatever the file; the path or descriptor is checked so
 * that a missing file or a bad descriptor is reported as POSIX asks.
 */

#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static long conf_value(int name) {
	switch (name) {
	case _PC_NAME_MAX:
		return NAME_MAX;
	case _PC_PATH_MAX:
		return PATH_MAX;
	default:
		errno = EINVAL;
		return -1;
	}
}

long int pathconf(const char *path, int name) {
	struct stat st;

	if (stat(path, &st) != 0) {
		return -1; /* errno from stat() */
	}
	return conf_value(name);
}

long int fpathconf(int fd, int name) {
	struct stat st;

	if (fstat(fd, &st) != 0) {
		return -1; /* EBADF from fstat() */
	}
	return conf_value(name);
}
