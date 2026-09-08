/**
 * @file
 * @brief The MMU, on before anything can take a spinlock.
 *
 * On a Cortex-A72 an LDAXR/STXR pair to memory that is
 * not Normal Inner-Shareable Cacheable raises an SError. With SCTLR.M clear
 * every data access at EL1 is Device, so the FIRST real spinlock in the boot
 * aborts -- and with SMP compiled in that is inside the kernel task's init,
 * long before anything has a chance to turn translation on.
 *
 * That is what a Cortex-A72 board did the first time it was handed an SMP
 * image: `SError exception!' with pc in __spin_trylock_smp and x1 pointing
 * at the kernel task's idesc table. QEMU never showed it, because a
 * Cortex-A53 tolerates the same instruction.
 *
 * The ordinary path cannot fix it: mmu_on() is called from vmem_init(), which
 * depends on the kernel task and walks its memory areas, so it can only run
 * after the thing that faults.
 *
 * So this builds a flat map of its own -- one level-0 entry, one level-1
 * table of 1 GiB blocks -- and turns translation on from kernel_start(),
 * before a single module has initialised. RAM is Normal Inner-Shareable
 * Write-Back; everything else is Device-nGnRnE, which is what a peripheral
 * needs and what an unpopulated address deserves.
 *
 * It is deliberately coarse. A boot-time map has one job, and every page it
 * does not need is a page it can get wrong; whatever mapping the system wants
 * afterwards is free to replace this one through mmu_set_context().
 */
#include <stdint.h>

#include <framework/mod/options.h>
#include <hal/cache.h>
#include <hal/mmu.h>
#include <util/log.h>

#include <hal/reg.h>

#include "mmu.h"

#define EARLY_ON OPTION_GET(NUMBER, early_on)

#if EARLY_ON

#define GIB (1024ULL * 1024 * 1024)

/* 4 KiB granule, 48-bit VA: level 0 entries span 512 GiB, level 1 blocks
 * 1 GiB. One of each covers everything a board in this tree addresses. */
#define L1_BLOCKS 512

/* Block and table descriptors, and the attribute bits that matter here. */
#define DESC_BLOCK   0x1ULL
#define DESC_TABLE   0x3ULL
#define DESC_ATTR(i) ((uint64_t)(i) << 2)
#define DESC_AP_RW   (0x0ULL << 6)  /* EL1 read/write, EL0 none */
#define DESC_SH_IS   (0x3ULL << 8)  /* Inner Shareable */
#define DESC_AF      (0x1ULL << 10) /* accessed, so no fault on first touch */
#define DESC_UXN     (0x1ULL << 54)

/* MAIR index 0 is Device-nGnRnE and 1 is Normal; mmu_init() writes both. */
#define ATTR_DEVICE 0
#define ATTR_NORMAL 1

static uint64_t early_l0[512] __attribute__((aligned(4096)));
static uint64_t early_l1[L1_BLOCKS] __attribute__((aligned(4096)));

extern char _ram_base[];
extern char _ram_size[];

void arch_mmu_early_on(void) {
	uint64_t ram_start, ram_end;
	unsigned i;

	/* The DRAM the kernel was linked into, widened to whole blocks: the Pi
	 * loads at 0x80000 with memory from zero, QEMU's virt has its devices
	 * below RAM and RAM from 0x40000000. Rounding out rather than in is what
	 * makes one rule right on both -- a block is Normal only if it lies
	 * wholly inside that range, so the block holding a board's peripherals
	 * never becomes cacheable by accident. */
	ram_start = (uint64_t)(uintptr_t)_ram_base & ~(GIB - 1);
	ram_end = ((uint64_t)(uintptr_t)_ram_base + (uint64_t)(uintptr_t)_ram_size
	              + GIB - 1)
	          & ~(GIB - 1);

	for (i = 0; i < L1_BLOCKS; i++) {
		uint64_t base = (uint64_t)i * GIB;
		uint64_t desc = base | DESC_BLOCK | DESC_AP_RW | DESC_AF;

		if (base >= ram_start && base + GIB <= ram_end) {
			desc |= DESC_ATTR(ATTR_NORMAL) | DESC_SH_IS;
		}
		else {
			/* Device memory is never executed from, and saying so keeps a
			 * speculative fetch off a peripheral. */
			desc |= DESC_ATTR(ATTR_DEVICE) | DESC_UXN;
		}
		early_l1[i] = desc;
	}

	early_l0[0] = (uint64_t)(uintptr_t)early_l1 | DESC_TABLE;
	for (i = 1; i < 512; i++) {
		early_l0[i] = 0;
	}

	/* The tables were written with translation off, so those stores went
	 * straight to memory -- and the table walker, which mmu_init() may set up
	 * to read through the caches, would not see them if a stale line for the
	 * same address is sitting in this core's D-cache. Push them out by hand
	 * before anything can walk them. */
	dcache_flush(early_l0, sizeof(early_l0));
	dcache_flush(early_l1, sizeof(early_l1));

	ARCH_REG_STORE(TTBR0_EL1, (uint64_t)(uintptr_t)early_l0);

	/* mmu_on() sets TCR and MAIR on its first call, drops the bootloader's
	 * TLB entries, and enables SCTLR.M together with .C and .I -- which is
	 * the half of this that makes exclusives legal. */
	mmu_on();

	log_debug("early mmu on: RAM %#llx..%#llx normal, the rest device",
	    (unsigned long long)ram_start, (unsigned long long)ram_end);
}

#else /* !EARLY_ON */

/* The board did not ask, so the ordinary path decides when translation goes
 * on. Defined rather than omitted: kernel_start() calls this unconditionally
 * and a weak stub there would hide a misconfigured board rather than a
 * deliberate one. */
void arch_mmu_early_on(void) {
}

#endif /* EARLY_ON */
