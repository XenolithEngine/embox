/**
 * @file devfs_notify_dvfs.c
 * @brief A device that goes away takes its name in /dev with it.
 *
 * devfs has no stored directory: ino_lookup() walks the device tables on
 * every miss, so a device that appears is found the first time anybody asks.
 * A device that DISAPPEARS is a different matter, because the name it was
 * found under stays in the DVFS tree -- local_lookup() answers from the
 * parent's list of children before ino_lookup() is ever reached -- and that
 * cached dentry holds an inode whose private pointer is the dev_module that
 * is being freed right now.
 *
 * What that costs, measured: create a ramdisk, delete it, create another
 * under the same name, and there are two devices. block_dev_find() walks the
 * device table and answers with the new one; a path lookup answers from the
 * stale dentry and gives the old one, whose memory has already been handed
 * back. mkfs() resolves its device by path (dvfs_lookup, see fs/dvfs/compat.c)
 * and so formats the destroyed object; the volume then mounts and reads and
 * writes correctly, because the pages underneath are the same pages -- while
 * a direct read of sector 0 through the device the NAME now means returns
 * the bytes the media held before the format. Nothing reports an error at
 * any point. Under oldfs this file's counterpart does the same job by
 * deleting the node; under DVFS it was a stub that did nothing.
 *
 * Found by scripts/embox-fat-hunt.sh (MODES=alone), which reproduces it in
 * one round: embox.test.fs.fat_ops run a second time in the same boot.
 */
#include <stddef.h>

#include <drivers/dev_module.h>
#include <fs/dentry.h>
#include <fs/dvfs.h>
#include <fs/inode.h>
#include <lib/libds/dlist.h>

/* Every dentry that exists, linked by d_lnk. dvfs_remove.c reaches for it
 * the same way. */
extern struct dlist_head dentry_dlist;

void devfs_notify_new_module(struct dev_module *devmod) {
	/* Nothing to do: there is no directory to add to. The next lookup of
	 * the name misses the tree and asks devfs_lookup(), which walks the
	 * device tables and finds it. */
}

/* The one dentry naming this device module, or NULL. Under the tree lock. */
static struct dentry *dentry_of_module(struct dev_module *devmod) {
	struct dentry *d;

	dlist_foreach_entry(d, &dentry_dlist, d_lnk) {
		if (d->d_inode != NULL && inode_priv(d->d_inode) == devmod) {
			return d;
		}
	}
	return NULL;
}

void devfs_notify_del_module(struct dev_module *devmod) {
	struct dentry *d;

	if (devmod == NULL) {
		return;
	}

	dvfs_lock();

	/* Rescanned rather than iterated: freeing a dentry drops a reference on
	 * its parent and may free that too, so a position in the list does not
	 * survive an eviction. There is one name per device module in practice;
	 * the loop is for the case where that stops being true, and it ends
	 * because every turn either frees a dentry or clears the pointer that
	 * made it match. */
	while ((d = dentry_of_module(devmod)) != NULL) {
		if (d->usage_count > 0) {
			/* Somebody holds it -- an open descriptor, a mount. The name
			 * goes now and the dentry lives until they let go, which is what
			 * unlink() does with a file that is still open. Its private
			 * pointer is cleared first: the module is about to be freed, and
			 * a dangling pointer that nothing looks at today is a defect
			 * waiting for the day something does. */
			inode_priv_set(d->d_inode, NULL);
			dentry_unlink_dying(d);
		}
		else if (0 != dvfs_destroy_dentry(d)) {
			/* Refused with usage_count 0: it is the root, or the pool
			 * disagrees. Clearing the pointer is all that is left, and it
			 * still beats leaving a freed module reachable by name. */
			inode_priv_set(d->d_inode, NULL);
		}
	}

	dvfs_unlock();
}
