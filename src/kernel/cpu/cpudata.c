/**
 * @file
 * @brief
 *
 * @date 08.02.12
 * @author Anton Bulychev
 * @author Ilia Vaprol
 */

#include <stdint.h>

#include <kernel/cpu/cpudata.h>

/* The reset handler calls this with the MMU off, where every access is to
 * Device memory: no unaligned access, no DC ZVA, and no library routine such
 * as memcpy(). So: aligned 64-bit words, through a volatile pointer, which
 * also keeps the compiler from turning the loop into memcpy(). The blocks are
 * linker-placed and word-aligned; a tail shorter than a word goes by bytes. */
void cpudata_init(void) {
	unsigned int cpu_id;
	const char *src = __CPUDATA_START;
	char *cpudata_block = __CPUDATA_START;
	const size_t words = __CPUDATA_BLOCK_SZ / sizeof(uint64_t);
	size_t i;

	for (cpu_id = 1; cpu_id < NCPU; cpu_id++) {
		volatile uint64_t *dst;

		cpudata_block += __CPUDATA_BLOCK_SZ;
		dst = (volatile uint64_t *)cpudata_block;
		for (i = 0; i < words; i++) {
			dst[i] = ((const uint64_t *)src)[i];
		}
		for (i = words * sizeof(uint64_t); i < __CPUDATA_BLOCK_SZ; i++) {
			((volatile char *)cpudata_block)[i] = src[i];
		}
	}
}
