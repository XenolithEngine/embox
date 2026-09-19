/**
 * @file
 * @brief
 *
 * @date 3 Apr 2015
 * @author Denis Deryugin
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#include <fs/dvfs.h>

int mkdir(const char *pathname, mode_t mode) {
	struct lookup lu;
	char last[NAME_MAX];
	int res;

	dvfs_lock();

	/* Resolve and create in one hold of the lock. An earlier component that
	 * is missing is ENOENT; it used to be taken as the parent, and
	 * mkdir("/a/b/c") with no /a made /c. */
	res = dvfs_lookup_at(NULL, pathname, &lu, last);
	if (res == 0) {
		if (lu.item) {
			res = -EEXIST;
		} else {
			res = dvfs_create_new(last, &lu,
			    S_IFDIR | (mode & VFS_DIR_VIRTUAL));
		}
		dvfs_lookup_put(&lu);
	}

	dvfs_unlock();

	if (res) {
		return SET_ERRNO(-res);
	}

	return 0;
}
