/**
 * @file
 *
 * @details http://pubs.opengroup.org/onlinepubs/007908775/xsh/sysconf.html
 *
 * @date Oct 17, 2013
 * @author: Anton Bondarev
 */
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <time.h>

#include <mem/page.h>
#include <hal/cpu.h>

struct thread;
/* kernel/cpu: a started core has an idle thread. Weak, so that a build
 * without that module answers NCPU as before. */
extern struct thread *cpu_get_idle(unsigned int cpu_id) __attribute__((weak));

/* The cores that are running, not the ones the build allows for: a thread
 * pool sized from NCPU on a board (or a QEMU -smp) with fewer cores puts
 * several busy threads on each. */
static long cpus_online(void) {
	long n = 0;
	unsigned int i;

	if (!cpu_get_idle) {
		return NCPU;
	}
	for (i = 0; i < NCPU; i++) {
		if (cpu_get_idle(i) != NULL) {
			n++;
		}
	}
	return n ? n : 1;
}

long int sysconf(int name) {
	switch(name) {
	case _SC_PAGESIZE:
		return PAGE_SIZE();
	case _SC_CLK_TCK:
		return CLK_TCK;
	case _SC_NPROCESSORS_ONLN:
	//http://www.gnu.org/software/libc/manual/html_node/Processor-Resources.html
		return cpus_online();
	case _SC_GETPW_R_SIZE_MAX:
		return 0x200;
	case _SC_ATEXIT_MAX:
		return ATEXIT_MAX;
	case _SC_PHYS_PAGES:
		//FIXME
		return 0x1000;
	case _SC_OPEN_MAX:
		return _SC_OPEN_MAX;
	default:
		return -EINVAL;
	}
}
