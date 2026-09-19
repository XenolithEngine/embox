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
	/* Where a driver whose position is more than one number keeps it. It
	 * belongs to the reader: a driver that kept it in the directory's own
	 * data had every reader of that directory share one position. */
	unsigned long fs_pos[3];
};

#endif /* SRC_INCLUDE_FS_DIR_CONTEXT_H_ */
