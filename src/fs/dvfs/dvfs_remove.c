/**
 * @file
 * @brief  DVFS interface implementation
 * @author Denis Deryugin
 * @date   11 Mar 2014
 */

#include <util/log.h>

#include <assert.h>
#include <errno.h>

#include <fs/dentry.h>
#include <fs/inode.h>
#include <fs/inode_operation.h>
#include <fs/dvfs.h>

#include <sys/stat.h>
#include <util/atomic_rmw.h>

#define REMOVE_ANY  0
#define REMOVE_FILE 1
#define REMOVE_DIR  2

extern struct dlist_head dentry_dlist;

/* Frees the cached children of DIR that nobody holds, so that what is left on
 * its list is what somebody is using. */
static void dvfs_prune_children(struct dentry *dir) {
	struct dentry *child;
	int freed;

	do {
		freed = 0;
		dlist_foreach_entry(child, &dir->children, children_lnk) {
			if (child->usage_count == 0 && dlist_empty(&child->children)
			    && !(child->flags & DVFS_MOUNT_POINT) && !child->d_covered) {
				if (0 == dvfs_destroy_dentry(child)) {
					freed = 1;
					break;
				}
			}
		}
	} while (freed);
}

/* Removes the name LU found. LU is from dvfs_lookup_at() in the same hold of
 * the lock, item and parent both held; the caller puts it afterwards.
 *
 * The name goes at once -- off the disk, off the tree -- whoever else holds
 * the dentry. What they hold becomes DYING: a descriptor on it is refused from
 * then on (the driver has let go of the file), and the last reference frees
 * it. That is the order oldfs settled on too: a name that stays on the disk
 * until the last close is a name the next lookup finds, and the "deleted"
 * file comes back. */
int dvfs_remove_locked(struct lookup *lu, int kind) {
	struct dentry *d = lu->item;
	struct inode *i_no;
	int res;

	dvfs_lock_assert("dvfs_remove_locked");

	if (d == NULL) {
		return -ENOENT;
	}

	if (d == dvfs_root() || (d->flags & DVFS_MOUNT_POINT)) {
		return -EBUSY;
	}

	if (d->flags & DVFS_DYING) {
		return -ENOENT;
	}

	if (S_ISDIR(d->flags)) {
		if (kind == REMOVE_FILE) {
			return -EISDIR;
		}
		dvfs_prune_children(d);
		if (!dlist_empty(&d->children)) {
			/* Somebody holds a child, or a mount or a virtual directory is
			 * under it: either way it is not empty. */
			return -ENOTEMPTY;
		}
	} else if (kind == REMOVE_DIR) {
		return -ENOTDIR;
	}

	i_no = d->d_inode;

	if (d->d_sb != NULL) {
		assert(i_no);
		assert(i_no->i_ops);

		if (!i_no->i_ops->ino_remove) {
			return -EROFS;
		}

		res = i_no->i_ops->ino_remove(lu->parent->d_inode, i_no);
		if (res != 0) {
			log_error("Failed to remove inode");
			return res < 0 ? res : -EIO;
		}
	} else {
		/* A virtual directory: the tree held it by itself (see
		 * dvfs_create_new()), and that pin goes with the name. */
		assert(d->usage_count > 1);
		d->usage_count--;
	}

	if (d->usage_count > 1) {
		/* Held by somebody besides this lookup: the free waits for them */
		atomic_rmw_add_fetch(&inode_free_deferred, 1, __ATOMIC_RELAXED);
	}

	dentry_unlink_dying(d);

	return 0;
}

static int dvfs_remove_kind(const char *path, int kind) {
	struct lookup lookup;
	int res;

	dvfs_lock();

	res = dvfs_lookup_at(NULL, path, &lookup, NULL);
	if (res == 0) {
		res = dvfs_remove_locked(&lookup, kind);
		dvfs_lookup_put(&lookup);
	}

	dvfs_unlock();

	return res;
}

/**
 * @brief Delete file or empty directory from storage
 * @param path Path to file
 *
 * @return Negative error code
 * @retval  0 Ok
 */
int dvfs_remove(const char *path) {
	return dvfs_remove_kind(path, REMOVE_ANY);
}

int dvfs_unlink(const char *path) {
	return dvfs_remove_kind(path, REMOVE_FILE);
}

int dvfs_rmdir(const char *path) {
	return dvfs_remove_kind(path, REMOVE_DIR);
}
