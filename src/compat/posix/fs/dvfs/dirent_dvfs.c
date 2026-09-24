/**
 * @file
 * @brief
 *
 * @date 5 Apr 2015
 * @author Denis Deryugin
 */

#include <errno.h>
#include <posix_errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>

#include <dirent.h>
#include <dirent_dvfs_impl.h>
#include <framework/mod/options.h>
#include <fs/dvfs.h>
#include <mem/misc/pool.h>

#define MAX_DIR_QUANTITY OPTION_GET(NUMBER, dir_quantity)

POOL_DEF(dir_pool, DIR, MAX_DIR_QUANTITY);

/* The type of the entry, which the tree already knows: the inode's mode, or,
 * for a virtual directory that has no inode, the dentry's own flags. It was
 * never filled, so every reader that wanted it stat()ed the entry by name. */
static unsigned char dirent_type(const struct dentry *dentry) {
	mode_t mode = dentry->d_inode ? dentry->d_inode->i_mode : dentry->flags;

	switch (mode & S_IFMT) {
	case S_IFREG:  return DT_REG;
	case S_IFDIR:  return DT_DIR;
	case S_IFCHR:  return DT_CHR;
	case S_IFBLK:  return DT_BLK;
	case S_IFIFO:  return DT_FIFO;
	case S_IFLNK:  return DT_LNK;
	case S_IFSOCK: return DT_SOCK;
	default:       return DT_UNKNOWN;
	}
}

static inline void fill_dirent(struct dirent *dirent, struct dentry *dentry) {
	/* i_no is -1 on a driver that has no numbers of its own; 0 then, which
	 * is what "no number" reads as everywhere else. */
	dirent->d_ino = (dentry->d_inode && dentry->d_inode->i_no > 0)
	                    ? (ino_t)dentry->d_inode->i_no : 0;
	dirent->d_type = dirent_type(dentry);
	dirent->d_reclen = sizeof(*dirent);
	memcpy(dirent->d_name, dentry->name, NAME_MAX);
}

DIR *opendir(const char *path) {
	DIR *d;
	struct lookup l = {0, 0};
	int err;

	if ((err = dvfs_lookup(path, &l))) {
		SET_ERRNO(-err);
		return NULL;
	}

	if (l.item == NULL) {
		SET_ERRNO(ENOENT);
		return NULL;
	}

	if (!S_ISDIR(l.item->flags)) {
		dentry_ref_dec(l.item);
		SET_ERRNO(ENOTDIR);
		return NULL;
	}

	if (!(d = pool_alloc(&dir_pool))) {
		dentry_ref_dec(l.item);
		SET_ERRNO(ENOMEM);
		return NULL;
	}

	/* The DIR keeps the lookup's reference until closedir(). */
	*d = (DIR) {
		.dir_dentry = l.item,
	};

	fill_dirent(&d->dirent, l.item);

	return d;
}

int closedir(DIR *dir) {
	if (!dir) {
		SET_ERRNO(EBADF);
		return -1;
	}

	dentry_ref_dec(dir->dir_dentry);

	pool_free(&dir_pool, dir);

	return 0;
}

struct dirent *readdir(DIR *dir) {
	struct lookup l;
	int err;

	if (!dir) {
		SET_ERRNO(EBADF);
		return NULL;
	}

	l = (struct lookup) {
		.parent = dir->dir_dentry,
	};

	if ((err = dvfs_iterate(&l, &dir->ctx))) {
		SET_ERRNO(-err);
		return NULL;
	}

	if (!l.item) {
		return NULL;
	}

	fill_dirent(&dir->dirent, l.item);
	dir->pos++;
	dir->dirent.d_off = dir->pos;

	/* The entry stays in the tree as cache; reclaim takes it back when a
	 * pool needs it. It used to be destroyed here, which freed a dentry
	 * another core had just found and not yet taken. */
	dentry_ref_dec(l.item);

	return &dir->dirent;
}

/* Back to the first entry. A driver keeps nothing in the iteration context
 * but its position -- fs_ctx is an index or a pointer into data it already
 * owns, fs_pos plain numbers -- so a zeroed context is a directory not yet
 * read. This was a printk stub, and a reader that rewound read nothing. */
void rewinddir(DIR *dir) {
	if (!dir) {
		return;
	}
	dir->ctx = (struct dir_ctx) {0};
	dir->pos = 0;
}

/* The number of entries read so far: a position seekdir() can return to,
 * which is all POSIX asks of it. */
long telldir(DIR *dir) {
	if (!dir) {
		SET_ERRNO(EBADF);
		return -1;
	}
	return dir->pos;
}

/* The iteration can only move forward, so going to a position is starting
 * over and reading up to it. Directories here are short, and this is only
 * paid by a caller that asked for it. */
void seekdir(DIR *dir, long loc) {
	if (!dir || loc < 0) {
		return;
	}
	if (loc < dir->pos) {
		rewinddir(dir);
	}
	while (dir->pos < loc && readdir(dir)) {
	}
}
