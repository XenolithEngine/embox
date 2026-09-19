/**
 * @file
 *
 * @date 14.04.2017
 * @author Anton Bondarev
 */

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>


int mknod(const char *pathname, mode_t mode, dev_t dev) {
	/* Nothing makes device nodes under DVFS -- devfs lists the devices that
	 * exist. Saying "done" for a node that is not there was worse than
	 * saying no. */
	return SET_ERRNO(ENOSYS);
}
