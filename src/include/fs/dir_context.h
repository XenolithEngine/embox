/**
 * @file
 *
 * @date Dec 26, 2019
 * @author Anton Bondarev
 */

#ifndef SRC_INCLUDE_FS_DIR_CONTEXT_H_
#define SRC_INCLUDE_FS_DIR_CONTEXT_H_

struct dir_ctx {
	int   flags;
	void *fs_ctx;
	/* A position of more than one number. It belongs to the reader, not to
	 * the directory, so readers of one directory do not share it. */
	unsigned long fs_pos[3];
};

#endif /* SRC_INCLUDE_FS_DIR_CONTEXT_H_ */
