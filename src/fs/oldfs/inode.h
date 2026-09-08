/**
 * @file
 * @brief pool of nodes
 *
 * @date 06.10.10
 * @author Nikolay Korotky
 */

#ifndef FS_NODE_H_
#define FS_NODE_H_

#include <fcntl.h>
//#include <sys/stat.h>
#include <limits.h>

#include <lib/libds/slist.h>
#include <lib/libds/tree.h>

#include <kernel/thread/sync/mutex.h>

#include <fs/super_block.h>

struct inode_operations;
struct super_block;
struct dentry;

struct flock_shared {
	struct thread *holder;
	struct dlist_head flock_link;
};

struct node_flock {
	struct mutex      exlock;
	long              shlock_count;
	struct dlist_head shlock_holders;
	spinlock_t        flock_guard;
};

struct inode {
	int      i_no;
	size_t        i_size;
	unsigned int  i_ctime; /* time of last status change */
	unsigned int  i_mtime;

	mode_t                i_mode;/* discrete access mode Read-Write-Execution */
	uid_t                 i_owner_id;/* owner user ID */
	gid_t                 i_group_id;/* owner group ID */

	struct dentry        *i_dentry;
	struct super_block      *i_sb;
	struct inode_operations *i_ops;

	void                 *i_privdata;

	unsigned int i_nlink;

	/* i_nlink counts names in the tree; these two count
	 * users of the pointer. Finding a path and using what it led to are two
	 * operations, and between them another core can unlink the inode --
	 * i_nlink going to zero frees it, and whoever was holding it is left
	 * pointing at a pool slot that the next open will hand to someone else.
	 *
	 * i_ref  users holding this inode across a window in which it could be
	 *        unlinked -- open file descriptors, today.
	 * i_dying set when the inode leaves the name tree with users still on it;
	 *        the last inode_unref() is what frees it then. Also what tells a
	 *        would-be user that this inode is on its way out, so a reference
	 *        taken after the unlink is refused rather than granted on a
	 *        corpse.
	 *
	 * Both are read and written under vfs.c's tree lock, which is also what
	 * makes the unlink and the reference an order rather than a race. */
	int i_ref;
	int i_dying;

	/* Which allocation of this pool slot this is.
	 *
	 * A descriptor holds a pointer, and a pointer that has been freed and
	 * handed out again is still a perfectly valid pointer -- to somebody
	 * else's file. Observed shape: a writer's descriptor survived an
	 * unmount, the remount refilled the pools, and every later write went
	 * into whichever file inherited the slot. fsck saw nothing, because each
	 * write was a legal write to a legal cluster of the wrong file.
	 *
	 * The driver's guard catches a NULL private pointer, which is the window
	 * of one write; it cannot catch the valid-and-wrong pointer that follows.
	 * A generation can: whoever holds this inode records the number it saw,
	 * and a mismatch means the slot changed hands. */
	unsigned int i_gen;

	struct slist_link dirent_link;

	/* node name (use vfs_get_path_by_node() for get full path*/
	char                  name[NAME_MAX + 1];

	int                   mounted; /* is mount point*/

	struct node_flock     flock;

	/* service data structure for enabling tree operation */
	struct tree_link      tree_link;
};

extern struct inode *inode_new(struct super_block *sb);
extern void inode_del(struct inode *node);

/**
 * @param name Non-empty string.
 * @param name_len (optional) how many bytes to take from name.
 *    If zero, the name must be a null-terminated string.
 */
extern struct inode *inode_alloc(struct super_block *sb);

/* inode_ref() answers 0 if the inode is already on its way
 * out, in which case the caller must not use it; every successful reference is
 * matched by exactly one inode_unref(). Both live in vfs.c, next to the lock
 * that orders them against vfs_del_leaf(). */
extern int inode_ref(struct inode *node);
extern void inode_unref(struct inode *node);

/* "The tree is done with this inode." Frees it if nobody holds it, marks it
 * dying if somebody does -- and then the last inode_unref() frees it. This is
 * the ONLY way an inode that was ever reachable may be given up: a plain
 * inode_free() on one somebody holds is the use-after-free this counting
 * exists to prevent. */
extern void inode_release(struct inode *node);

/* Hands the filesystem back whatever it hung off this inode, now. Needed
 * before the superblock goes, because a deferred inode outlives the unlink. */
extern void inode_detach_fs(struct inode *node);

/* Frees postponed because somebody still held the inode, and references
 * refused because it was already dying. Both are evidence that the window
 * this counting exists to close is a real one. */
extern unsigned long inode_free_deferred;
extern unsigned long inode_ref_refused;

/* Uses of a descriptor whose inode is not the one it opened, and uses of one
 * whose inode has left the tree. Both must be zero; a non-zero value names a
 * silent corruption that would otherwise be found by its consequences. */
extern unsigned long fdesc_stale_gen;
extern unsigned long fdesc_dead_inode;

/* Walks that arrived at an inode after it left the name tree. */
extern unsigned long vfs_walk_dying;

extern void inode_free(struct inode *node);
extern void *inode_priv(const struct inode *node);
extern void inode_priv_set(struct inode *node, void *priv);
extern size_t inode_size(const struct inode *node);
extern void inode_size_set(struct inode *node, size_t sz);
extern unsigned inode_ctime(const struct inode *node);
extern void inode_ctime_set(struct inode *node, unsigned ctime);
extern unsigned inode_mtime(const struct inode *node);
extern void inode_mtime_set(struct inode *node, unsigned mtime);
extern char *inode_name(struct inode *node);
extern char *inode_name_set(struct inode *node, const char *name);
#if 0
static inline int node_is_directory(struct inode *node) {
	return S_ISDIR(node->i_mode);
}
#endif
#endif /* FS_NODE_H_ */
