/**
 * @file devfs_notify_dvfs.c
 * @brief A device that goes away takes its name in /dev with it.
 *
 * devfs has no stored directory: ino_lookup() walks the device tables on
 * every miss, so a new device is found the first time anybody asks. A name
 * once found stays in the DVFS tree, though (local_lookup() answers from the
 * parent's children before ino_lookup()), and its inode's private pointer is
 * the dev_module being freed. Left there, a path lookup of a new device of
 * the same name would answer with the freed one.
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

	/* Rescanned rather than iterated: freeing a dentry may free its parent
	 * too, so a list position does not survive. Each turn frees a dentry or
	 * clears the pointer that made it match, so the loop ends. */
	while ((d = dentry_of_module(devmod)) != NULL) {
		if (d->usage_count > 0) {
			/* Somebody holds it (an open descriptor, a mount): the name goes
			 * now and the dentry lives until they let go, as with unlink().
			 * The private pointer is cleared; the module is about to be freed. */
			inode_priv_set(d->d_inode, NULL);
			dentry_unlink_dying(d);
		}
		else if (0 != dvfs_destroy_dentry(d)) {
			/* Refused with usage_count 0 (the root, or the pool disagrees):
			 * clearing the pointer is all that is left. */
			inode_priv_set(d->d_inode, NULL);
		}
	}

	dvfs_unlock();
}
