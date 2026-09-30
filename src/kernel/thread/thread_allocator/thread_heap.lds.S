#include <asm-generic/embox.lds.h>
#include <framework/mod/options.h>

SECTIONS {
	/* In the reserve, which the boot does not zero: the heap is tens of
	 * megabytes, and every stack handed out is filled anyway. */
	.bss..reserve.thread_heap (NOLOAD): ALIGN(0x1000) {
		thread_heap_start = .;
		. += OPTION_GET(NUMBER,thread_heap_size);
	}
}
