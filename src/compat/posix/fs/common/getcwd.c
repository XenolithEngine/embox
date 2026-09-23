/**
 * @file
 * @brief
 *
 * @author Ilia Vaprol
 * @date 31.03.13
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fs/dvfs.h>
#include <kernel/task/resource/vfs.h>

/* The working directory's name, from the working directory. It came from
 * $PWD, which chdir() keeps for the shell -- and the environment stores a
 * string in 64 bytes, so a directory deeper than 59 characters could not be
 * entered at all (chdir failed on the setenv), and one entered any other way
 * than chdir() was never reported. The task holds the directory itself;
 * asking it is the answer whatever the environment says. */
char *getcwd(char *buff, size_t size) {
	struct task_vfs *t;
	struct dentry *pwd = NULL;
	int res;

	if ((buff == NULL) || (size == 0)) {
		SET_ERRNO(EINVAL);
		return NULL;
	}

	/* Taken under the tree lock, so a chdir() on another thread that lets go
	 * of the old directory cannot let reclaim have it mid-walk. */
	t = task_self_resource_vfs();
	dvfs_lock();
	if (t && t->pwd) {
		pwd = t->pwd;
		dentry_ref_inc(pwd);
	}
	dvfs_unlock();

	if (pwd == NULL) {
		/* Before the first chdir: the root, as it always was. */
		if (size < 2) {
			SET_ERRNO(ERANGE);
			return NULL;
		}
		strcpy(buff, "/");
		return buff;
	}

	res = dentry_full_path_n(pwd, buff, size);
	dentry_ref_dec(pwd);
	if (res) {
		SET_ERRNO(res == -ENAMETOOLONG ? ERANGE : -res);
		return NULL;
	}

	return buff;
}
