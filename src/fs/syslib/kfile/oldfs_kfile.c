/**
 * @file
 *
 * @date 06.08.09
 * @author Anton Bondarev
 */

#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <fs/inode.h>
#include <fs/file_desc.h>
#include <fs/kfile.h>

void kclose(struct file_desc *desc) {
	assert(desc);
	assert(desc->f_ops);

	if (desc->f_ops->close) {
		desc->f_ops->close(desc);
	}

	file_desc_destroy(desc);
}

ssize_t kwrite(struct file_desc *file, const void *buf, size_t size) {
	ssize_t ret;

	if (!file) {
		ret = -EBADF;
		goto end;
	}

	if (idesc_check_mode(&file->f_idesc, O_RDONLY)) {
		ret = -EBADF;
		goto end;
	}

	if (NULL == file->f_ops->write) {
		ret = -EBADF;
		goto end;
	}

	/* The same question, and here the answer costs more to get wrong: a write
	 * through a recycled descriptor lands in another file's clusters, and
	 * fsck sees nothing because every such write is legal. */
	if (!file_desc_valid(file)) {
		ret = -EBADF;
		goto end;
	}

	ret = file->f_ops->write(file, (void *)buf, size);
	if (ret > 0) {
		file_set_pos(file, file_get_pos(file) + ret);
	}

end:
	return ret;
}

ssize_t kread(struct file_desc *desc, void *buf, size_t size) {
	ssize_t ret;

	if (NULL == desc) {
		ret = -EBADF;
		goto end;
	}

	if (idesc_check_mode(&desc->f_idesc, O_WRONLY)) {
		ret = -EBADF;
		goto end;
	}

	if (NULL == desc->f_ops->read) {
		ret = -EBADF;
		goto end;
	}

	/* Is this still the file that was opened? A descriptor that outlived its
	 * volume points at a pool slot somebody else owns now, and reading through
	 * it reads their file. */
	if (!file_desc_valid(desc)) {
		ret = -EBADF;
		goto end;
	}

	/* Don't try to read past EOF.
	 *
	 * Read the file's length and this descriptor's position ONCE. They used
	 * to be read twice -- once to decide whether to clamp and once to do it
	 * -- and both are shared: another core writing to the same file changes
	 * i_size between the two reads. Then the clamp
	 * RAISES the size instead of lowering it, and the driver faithfully reads
	 * that many bytes into a buffer the caller sized for fewer.
	 *
	 * Measured on four cores with the boot-core fence lifted, one run in
	 * ten or so: read(fd, &c, 1) answering 256 and writing 256 bytes over the
	 * caller's frame -- return address included, so the crash lands somewhere
	 * with no connection to the read. Instrumented at every layer, all in the
	 * same failing run:
	 *
	 *   kread: 256 for an original 1; size now 256, i_size 0, pos 0
	 *
	 * i_size was 0 when the test read it (the file had just been recreated by
	 * the other core) and 256 by the assignment a few instructions later.
	 *
	 * The subtraction is guarded too: pos can legitimately be past i_size
	 * after a truncation, and an unsigned difference of those is not a
	 * length. */
	{
		size_t i_size = desc->f_inode->i_size;
		size_t pos = (size_t)file_get_pos(desc);
		size_t left = (i_size > pos) ? (i_size - pos) : 0;

		if (size > left) {
			size = left;
		}
	}

	ret = desc->f_ops->read(desc, buf, size);
	if (ret > 0) {
		file_set_pos(desc, file_get_pos(desc) + ret);
	}

end:
	return ret;
}

#include <drivers/block_dev.h> /* block_dev_block_size */
#include <string.h>

static int inode_fill_stat(struct inode *node, struct stat *sb) {
	memset(sb, 0 , sizeof(struct stat));

	sb->st_size = inode_size(node);
	sb->st_mode = node->i_mode;
	sb->st_uid = node->i_owner_id;
	sb->st_gid = node->i_group_id;
	sb->st_ctime = inode_ctime(node);
	sb->st_mtime = inode_mtime(node);
	sb->st_blocks = sb->st_size;

    if (node->i_sb->bdev) {
        sb->st_blocks /= block_dev_block_size(node->i_sb->bdev);
	    sb->st_blocks += (sb->st_blocks % block_dev_block_size(node->i_sb->bdev) != 0);	
    }

	return 0;
}

int kfstat(struct file_desc *desc, struct stat *sb) {
	if ((NULL == desc) || (sb == NULL)) {
		return -EBADF;
	}

	inode_fill_stat(desc->f_inode, sb);

	return 0;
}

int kioctl(struct file_desc *desc, int request, void *data) {
	int ret;

	if (NULL == desc) {
		return -EBADF;
	}

	if (NULL == desc->f_ops->ioctl) {
		return -ENOSUPP;
	}

	ret = desc->f_ops->ioctl(desc, request, data);

	if (ret < 0) {
		return ret;
	}

	return 0;
}
