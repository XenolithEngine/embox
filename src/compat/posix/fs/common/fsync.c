/**
 * @file
 *
 * @date Nov 15, 2013
 * @author: Anton Bondarev
 */

#include <errno.h>
#include <unistd.h>

#include <kernel/task/resource/idesc_table.h>
#include <kernel/task/resource/index_descriptor.h>

/* Nothing is held back per descriptor to flush: FAT writes its cached FAT
 * sector out at the end of every operation (fat_unlock). A descriptor that is
 * not open is EBADF. */
int fsync(int fd) {
	if (!idesc_index_valid(fd) || !index_descriptor_get(fd)) {
		return SET_ERRNO(EBADF);
	}
	return 0;
}
