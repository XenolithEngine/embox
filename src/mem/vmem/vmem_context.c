/**
 * @file
 *
 * @date Jul 31, 2014
 * @author: Anton Bondarev
 */
#include <errno.h>

#include <util/log.h>

#include <hal/mmu.h>
#include <mem/vmem.h>
#include <mem/mmap.h>
#include <mem/vmem/vmem_alloc.h>

#include <kernel/sched/sched_lock.h>

#include <kernel/task/resource/mmap.h>

/* XENOLITH_EL0_VMEM: root of the kernel task's context. Set on the first
   vmem_create_context() call, which is kernel_task's resource init. */
static uintptr_t *vmem_kernel_pgd = NULL;

void vmem_clone_kernel_tables(mmu_ctx_t ctx) {
	uintptr_t *pgd;
	int idx;
	int shared;

	if (!vmem_kernel_pgd) {
		return;
	}

	pgd = mmu_get_root(ctx);
	if (pgd == vmem_kernel_pgd) {
		return;
	}

	shared = 0;
	for (idx = 0; idx < (int)MMU_ENTRIES(0); idx++) {
		if (mmu_present(0, vmem_kernel_pgd + idx)) {
			pgd[idx] = vmem_kernel_pgd[idx];
			shared++;
		}
	}

	if (!shared) {
		/* The kernel map is built by vmem_init() at runlevel 0. A context
		   created before that would silently come out without the kernel in
		   it, and the fault would land on the first TTBR0 switch instead of
		   here. Say so where it can still be read. */
		log_error("kernel context has no top-level entries to share yet");
	}
}

int vmem_is_kernel_vaddr(mmu_vaddr_t virt_addr) {
	int idx;

	if (!vmem_kernel_pgd) {
		return 0;
	}

	idx = (virt_addr & MMU_MASK(0)) >> MMU_SHIFT(0);

	return mmu_present(0, vmem_kernel_pgd + idx);
}

int vmem_create_context(mmu_ctx_t *ctx) {
	uintptr_t *pgd = vmem_alloc_table(0);

	if (!pgd) {
		return -ENOMEM;
	}

	*ctx = mmu_create_context(pgd);

	/* XENOLITH_EL0_VMEM: the first context belongs to the kernel task; every
	   later one is an address space that must still contain the kernel. */
	if (!vmem_kernel_pgd) {
		vmem_kernel_pgd = pgd;
	}
	else {
		vmem_clone_kernel_tables(*ctx);
	}

	return ENOERR;
}

mmu_ctx_t vmem_current_context(void) {
	struct emmap *emmap;

	emmap = task_self_resource_mmap();

	return emmap->ctx;
}

static void vmem_free_table_level(int lvl, uintptr_t *tbl) {
	if (lvl == MMU_LAST_LEVEL) {
		vmem_free_table(lvl, tbl);
		return;
	}
	for (int idx = 0; idx < MMU_ENTRIES(lvl); idx++) {
		if (mmu_present(lvl, tbl + idx)) {
			vmem_free_table_level(lvl + 1, mmu_get(lvl, (tbl + idx)));
		}
	}
	vmem_free_table(lvl, tbl);
}

void vmem_free_context(mmu_ctx_t ctx) {
	uintptr_t *pgd = mmu_get_root(ctx);

#if MMU_LAST_LEVEL > 0
	if (pgd != vmem_kernel_pgd) {
		int idx;

		/* XENOLITH_EL0_VMEM: a top-level entry equal to the kernel's is the
		   kernel's -- it points at tables the kernel is still running on.
		   Free the root itself and nothing below those slots. */
		for (idx = 0; idx < (int)MMU_ENTRIES(0); idx++) {
			if (!mmu_present(0, pgd + idx)) {
				continue;
			}
			if (mmu_present(0, vmem_kernel_pgd + idx)
			    && (pgd[idx] == vmem_kernel_pgd[idx])) {
				continue;
			}
			vmem_free_table_level(1, mmu_get(0, pgd + idx));
		}
		vmem_free_table(0, pgd);

		return;
	}
#endif /* MMU_LAST_LEVEL > 0 */

	vmem_free_table_level(0, pgd);
}
