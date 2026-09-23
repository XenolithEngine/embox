/**
* @file
* @brief
*
* @author Denis Deryugin
* @date 3 Apr 2015
*/

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <fs/inode.h>
#include <fs/dentry.h>
#include <kernel/task/resource/vfs.h>
#include <util/atomic_rmw.h>
#include <util/log.h>

/**
* @brief POSIX-compatible function changing process working directory
*
* @param path Path to new working directory
*
* @return On success, 0 returned, otherwise -1 returned and ERRNO is set
*	 appropriately.
*/
int chdir(const char *path) {
	struct lookup l = { NULL, NULL };
	int err;
	char new_pwd[PATH_MAX - 1];
	struct task_vfs *t;
	struct dentry *old;

	if (path == NULL) {
		SET_ERRNO(ENOENT);
		return -1;
	}

	if ((err = dvfs_lookup(path, &l))) {
		return SET_ERRNO(-err);
	}

	if (l.item == NULL) {
		return SET_ERRNO(ENOENT);
	}

	if (!(l.item->flags & S_IFDIR)) {
		dentry_ref_dec(l.item);
		return SET_ERRNO(ENOTDIR);
	}

	if (dentry_full_path_n(l.item, new_pwd, sizeof(new_pwd))) {
		dentry_ref_dec(l.item);
		return SET_ERRNO(ENAMETOOLONG);
	}

	if ((t = task_self_resource_vfs()) == NULL) {
		dentry_ref_dec(l.item);
		log_error("task VFS structure is NULL");
		return SET_ERRNO(EIO);
	}

	/* $PWD is for the shell; the working directory is t->pwd, and getcwd()
	 * reads that. A name the environment cannot hold (it keeps strings in 64
	 * bytes) used to make chdir() fail outright -- now $PWD is dropped
	 * instead, rather than left naming the directory before this one. */
	if (-1 == setenv("PWD", new_pwd, 1)) {
		unsetenv("PWD");
	}

	/* The lookup's reference becomes the working directory's. It used to be
	 * dropped first and taken again after, and in between the directory
	 * belonged to nobody -- free for reclaim to take. */
	old = atomic_rmw_exchange(&t->pwd, l.item, __ATOMIC_ACQ_REL);
	if (old) {
		dentry_ref_dec(old);
	}

	return 0;
}
