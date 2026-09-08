#include <hal/cpu.h>

/* One cache line is 64 bytes on Cortex-A53 and A72.
 * Without the two ALIGNs below, every CPU's __critical_count shares a line
 * with its neighbours', and that line moves between the cores on every
 * critical section. */
#define CPUDATA_LINE 64

SECTIONS {
	.data.embox.cpudata : ALIGN(CPUDATA_LINE) {
		__cpudata_start = .;
		*(.embox.cpudata.data)
		. = ALIGN(CPUDATA_LINE);
		__cpudata_block_end = .;
		. += (__cpudata_block_end - __cpudata_start) * (NCPU - 1);
		__cpudata_end = .;
	}
}
