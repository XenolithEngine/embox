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
 * without that module answers NCPU. */
extern struct thread *cpu_get_idle(unsigned int cpu_id) __attribute__((weak));
/* mem/phymem: the allocator of the RAM the kernel image does not take. Weak
 * for the same reason. */
extern struct page_allocator *phy_allocator(void) __attribute__((weak));

/* The pages phymem was given: all RAM past the image and its reserve, less
 * the allocator's own bitmap, and counting any carve-out it was told to keep
 * out (the Pi 4's gpu_mem). What mimalloc reads as physical memory too. */
static long phys_pages(void) {
	struct page_allocator *pa;

	pa = phy_allocator ? phy_allocator() : NULL;
	if (!pa || pa->page_size == 0) {
		return -1;
	}
	return (long)((size_t)pa->pages_n * pa->page_size / PAGE_SIZE());
}

/* The cores that are running, not the ones the build allows for, so that a
 * thread pool sized from it gets one busy thread per core. */
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
		return phys_pages();
	case _SC_OPEN_MAX:
		return _SC_OPEN_MAX;
	default:
		errno = EINVAL;
		return -1;
	}
}
