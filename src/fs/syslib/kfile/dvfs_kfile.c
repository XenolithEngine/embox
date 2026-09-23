/**
 * @file
 * @brief  DVFS interface implementation
 * @author Denis Deryugin
 * @date   11 Mar 2014
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>

#include <drivers/block_dev.h> /* block_dev_block_size */
#include <fs/dentry.h>
#include <fs/file_desc.h>
#include <fs/kfile.h>
#include <fs/dvfs.h>

#include <util/atomic_rmw.h>
#include <util/math.h>

/**
 * @brief Uninitialize file descriptor
 * @param desc File descriptor to be uninitialized
 *
 * @return Negative error code
 * @retval  0 Ok
 * @retval -1 Descriptor fields are inconsistent
 */
int kclose(struct file_desc *desc) {
	if (!desc || !desc->f_dentry)
		return -1;

	if (!(desc->f_dentry->flags & VFS_DIR_VIRTUAL)) {
		assert(desc->f_ops);
	}

	/* flock(2) locks belong to the open file, and go with it */
	dvfs_flock_release(desc);

	/* The driver is told only about a file it still has. */
	if (desc->f_ops && desc->f_ops->close && desc->f_inode
	    && 0 == dvfs_file_valid(desc)) {
		desc->f_ops->close(desc);
	}

	/* The descriptor's reference. A cached dentry stays for reclaim; one
	 * whose name was removed while this was open is freed here. */
	dentry_ref_dec(desc->f_dentry);

	dvfs_destroy_file(desc);
	return 0;
}

/**
 * @brief Application level interface to write the file
 * @param desc  File to be written
 * @param buf   Source of the data
 * @param count Length of the data
 *
 * @return Bytes written or negative error code
 * @retval -ENOSYS Function is not implemented in file system driver
 * @retval  -EBADF The file was removed while this was open
 */
int kwrite(struct file_desc *desc, char *buf, int count) {
	struct inode *inode;
	int res;

	if (!desc) {
		return -EINVAL;
	}

	inode = desc->f_inode;
	assert(inode);

	if ((res = dvfs_file_valid(desc))) {
		return res;
	}

	if (!desc->f_ops || !desc->f_ops->write) {
		return -ENOSYS;
	}

	res = desc->f_ops->write(desc, buf, count);
	if (res > 0) {
		desc->f_pos += res;
		/* The size goes up after the data is there, not before. It used
		 * to be raised first (through ino_truncate) and the data written
		 * after, and a reader on another core that looked in between saw
		 * the new size and read whatever the new cluster held -- another,
		 * deleted file's bytes. The drivers that keep the size themselves
		 * (FAT, ext2, ramfs) have already done this. */
		if (!(inode->i_mode & DVFS_NO_LSEEK)
		    && desc->f_pos > (off_t) inode->i_size) {
			inode->i_size = desc->f_pos;
		}
	}

	/* What the driver wrote, not what was asked: a short or failed write
	 * used to be reported as the whole count. */
	return res;
}

/**
 * @brief Application level interface to read the file
 * @param desc  File to be read
 * @param buf   Destination
 * @param count Length of the data
 *
 * @return Bytes read or negative error code
 * @retval -ENOSYS Function is not implemented in file system driver
 * @retval  -EBADF The file was removed while this was open
 */
int kread(struct file_desc *desc, char *buf, int count) {
	off_t size, pos;
	int res;

	if (!desc) {
		return -1;
	}

	if ((res = dvfs_file_valid(desc))) {
		return res;
	}

	/* One reading of the size, clamped against in signed arithmetic. With
	 * f_pos past the end, the unsigned difference this used to take was a
	 * huge number and the read went on past the end of the file; read twice,
	 * a write on another core in between could make the clamp and the read
	 * disagree. */
	size = (off_t) atomic_rmw_load(&desc->f_inode->i_size, __ATOMIC_RELAXED);
	pos = desc->f_pos;
	if (!(desc->f_inode->i_mode & DVFS_NO_LSEEK)) {
		if (pos >= size) {
			return 0;
		}
		if ((off_t) count > size - pos) {
			count = (int) (size - pos);
		}
	}

	if (count <= 0) {
		return 0;
	}

	if (desc->f_ops && desc->f_ops->read) {
		res = desc->f_ops->read(desc, buf, count);
	}
	else {
		return -ENOSYS;
	}

	if (res > 0) {
		desc->f_pos += res;
	}

	return res;
}

int kfstat(struct file_desc *desc, struct stat *sb) {
	const struct inode *node;
	size_t block_size = 512;
	int res;

	if ((res = dvfs_file_valid(desc))) {
		return res;
	}

	if (desc->f_inode == NULL) {
		/* A virtual directory made under another virtual one has no inode */
		*sb = (struct stat){
		    .st_mode = S_IFDIR | S_IRWXA,
		    .st_nlink = 1,
		    .st_blksize = block_size,
		};
		return 0;
	}
	node = desc->f_inode;

	if (node->i_sb && node->i_sb->bdev) {
		block_size = block_dev_block_size(node->i_sb->bdev);
	}

	/* st_dev, st_nlink, st_atime and st_blksize were never filled, and
	 * st_blocks counted device blocks, with a remainder test that compared
	 * the quotient with the block size. What POSIX says instead:
	 *   st_nlink  1 -- no driver here keeps a link count, and 0 reads as
	 *             "deleted" to anyone who checks;
	 *   st_atime  the modification time: nothing records access, and a
	 *             file cannot have been read before it was written;
	 *   st_blocks 512-byte units, rounded up;
	 *   st_ino    the driver's number, 0 when it has none (-1 in i_no). */
	*sb = (struct stat){
	    .st_dev = node->i_sb ? node->i_sb->sb_dev : 0,
	    .st_ino = node->i_no > 0 ? (ino_t)node->i_no : 0,
	    .st_mode = node->i_mode,
	    .st_nlink = 1,
	    .st_uid = node->i_owner_id,
	    .st_gid = node->i_group_id,
	    .st_size = node->i_size,
	    .st_blksize = block_size,
	    .st_blocks = (node->i_size + 511) / 512,
	    .st_atime = node->i_mtime,
	    .st_mtime = node->i_mtime,
	    .st_ctime = node->i_ctime,
	};

	return 0;
}

int kioctl(struct file_desc *fp, int request, void *data) {
	int res;

	if ((res = dvfs_file_valid(fp))) {
		return res;
	}
	if (!fp->f_ops || !fp->f_ops->ioctl) {
		return -ENOTTY;
	}
	return fp->f_ops->ioctl(fp, request, data);
}
