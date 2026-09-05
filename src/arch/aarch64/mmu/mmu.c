/**
 * @file mmu.c
 * @brief
 * @author Denis Deryugin <deryugin.denis@gmail.com>
 * @version
 * @date 07.08.2019
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <framework/mod/options.h>
#include <hal/cache.h>
#include <hal/mem_barriers.h>
#include <hal/mmu.h>
#include <hal/reg.h>
#include <mem/vmem.h>
#include <util/field.h>
#include <util/log.h>

#include "mmu.h"

/* One decision in two options, see the Mybuild: a Non-cacheable table walk
 * does not snoop the cache a descriptor store is sitting in. */
#define WALK_CACHEABLE   OPTION_GET(NUMBER, walk_cacheable)
#define PTE_DCACHE_FLUSH OPTION_GET(NUMBER, pte_dcache_flush)

/* Make a descriptor visible to a walk that does not look in this cache */
static inline void mmu_desc_written(const void *entry, size_t len) {
	if (PTE_DCACHE_FLUSH) {
		dcache_flush(entry, len);
	}
}

#define DEFAULT_ASID        0

#define MEM_DEVICE_ATTRINDX 0
#define MEM_NORMAL_ATTRINDX 1

static int mmu_init(void) {
	uint64_t mair;
	uint64_t tcr;
	uint64_t tmp;

	/* Load Translation Control Register */
	tcr = ARCH_REG_LOAD(TCR_EL1);

	switch (AARCH64_MMU_GRANULE) {
	case 4:
		tmp = TCR_ELn_TG0_4KB;
		break;
	case 16:
		tmp = TCR_ELn_TG0_16KB;
		break;
	case 64:
		tmp = TCR_ELn_TG0_64KB;
		break;
	default:
		tmp = TCR_ELn_TG0_4KB;
		log_crit("Wrong granule configuration '%i'", AARCH64_MMU_GRANULE);
	}

	/* Set Granule Size */
	tcr = FIELD_SET(tcr, TCR_ELn_TG0, tmp);

	/* Set maximum PA range */
	tmp = ARCH_REG_LOAD(ID_AA64MMFR0_EL1);
	tmp = FIELD_GET(tmp, ID_AA64MMFR0_EL1_PAR);
	tcr = FIELD_SET(tcr, TCR_EL1_IPS, tmp);

	/* Set 48-bit VA range 0 (0x0000_0000_0000_0000 - 0x0000_FFFF_FFFF_FFFF) */
	tcr = FIELD_SET(tcr, TCR_ELn_T0SZ, 64 - 48);

	/* Set 48-bit VA range 1 (0xFFFF_0000_0000_0000 - 0xFFFF_FFFF_FFFF_FFFF) */
	tcr = FIELD_SET(tcr, TCR_ELn_T1SZ, 64 - 48);

	/* TTBR0_EL1.ASID defines the ASID */
	tcr &= ~TCR_EL1_A1;

	/* XENOLITH_EL0_MMU: the kernel is identity-mapped in the low half, so
	   everything -- kernel and user alike -- is translated through TTBR0.
	   Nothing generates a high-half address; disable the TTBR1 walk so a
	   stray one faults instead of walking a stale root. */
	tcr |= TCR_EL1_EPD1;

	/* Table walks are cacheable and Inner Shareable, matching the
	 * attributes the page tables themselves are mapped with */
	if (WALK_CACHEABLE) {
		tcr = FIELD_SET(tcr, TCR_ELn_IRGN0, TCR_ELn_IRGN0_WBWA);
		tcr = FIELD_SET(tcr, TCR_ELn_ORGN0, TCR_ELn_ORGN0_WBWA);
		tcr = FIELD_SET(tcr, TCR_ELn_SH0, TCR_ELn_SH0_IS);
		tcr = FIELD_SET(tcr, TCR_ELn_IRGN1, TCR_ELn_IRGN1_WBWA);
		tcr = FIELD_SET(tcr, TCR_ELn_ORGN1, TCR_ELn_ORGN1_WBWA);
		tcr = FIELD_SET(tcr, TCR_ELn_SH1, TCR_ELn_SH1_IS);
	}

	/* Store Translation Control Register */
	ARCH_REG_STORE(TCR_EL1, tcr);

	/* Set memory attributes (0 - device; 1 - normal) */
	mair = ARCH_REG_LOAD(MAIR_EL1);
	mair = FIELD_SET(mair, MAIR_ELn_ATTR0, MAIR_ELn_ATTRn_DEVICE_nGnRnE);
	mair = FIELD_SET(mair, MAIR_ELn_ATTR1, MAIR_ELn_ATTRn_NORMAL);
	ARCH_REG_STORE(MAIR_EL1, mair);

	/* XENOLITH_EL0_MMU: the reset handler wrote SCTLR_EL1 = 0, which clears
	   every RES1 bit -- SPAN among them. On a core with ARMv8.1-PAN that means
	   PSTATE.PAN = 1 on each exception entry to EL1, and the kernel loses
	   access to every page carrying AP[1] (the user pages this MMU now emits):
	   copy_to_user would take a permission fault. Writing the RES1 set keeps
	   the ARMv8.0 behaviour on every core. A53/A72 have no PAN, so this is a
	   no-op on the boards in tree and correct on their successors. */
	ARCH_REG_ORIN(SCTLR_EL1, SCTLR_EL1_RES1);

	return 0;
}

void mmu_on(void) {
	static bool initialized = false;

	if (!initialized) {
		mmu_init();
		initialized = true;
	}

	dmb(sy);

	/* Enable MMU (stage 1 address translation) together with the caches.
	 * With SCTLR.M set but SCTLR.C clear, all data accesses are treated as
	 * Non-cacheable regardless of what the page tables say, and exclusive
	 * accesses to Non-cacheable memory are not architecturally guaranteed
	 * to work: on a Cortex-A72 an ldaxr/stxr pair to such memory raises an
	 * SError. */
	/* The bootloader's TLB entries are consulted the moment SCTLR.M is set,
	 * so they go before it, not after */
	ARCH_REG_STORE(TLBI_VMALLE1, 0);
	__asm__ __volatile__("ic iallu" : : : "memory");
	dsb(sy);
	isb();
	ARCH_REG_ORIN(SCTLR_EL1, SCTLR_ELn_M | SCTLR_ELn_C | SCTLR_ELn_I);
	isb();
}

void mmu_off(void) {
	/* Flush TLB */
	ARCH_REG_STORE(TLBI_VMALLE1, 0);

	/* Disable MMU (stage 1 address translation). The caches have to go
	 * down with it: leaving SCTLR.C set with translation off would make
	 * every access Cacheable, which is not what the caller asked for. */
	ARCH_REG_CLEAR(SCTLR_EL1, SCTLR_ELn_C | SCTLR_ELn_I | SCTLR_ELn_M);
	isb();
}

mmu_ctx_t mmu_create_context(uintptr_t *pgd) {
	mmu_ctx_t ctx;

	ctx = (mmu_ctx_t)pgd;

	/* Assume mmu_ctx_t and ttbr0 are the same values for aarch64, so just set
	   ASID=0 for now (as we have a single address space system-wide) */
	if (DEFAULT_ASID != FIELD_GET(ctx, TTBRn_EL1_ASID)) {
		log_crit("16 most-sign bits of pgd should be zero (pgd=%p)", pgd);
		ctx = FIELD_SET(ctx, TTBRn_EL1_ASID, DEFAULT_ASID);
		return 0;
	}

	return ctx;
}

void mmu_set_context(mmu_ctx_t ctx) {
	/* XENOLITH_EL0_MMU: TTBR0 alone. The kernel lives in the low half of the
	   same table, so a task root always carries the kernel's top-level entries
	   (vmem_clone_kernel_tables) and the instruction after this switch is
	   still mapped. TTBR1 is left as mmu_init() found it -- EPD1 is set, so it
	   is never walked.

	   ASID stays 0 for every context, so the TLB cannot tell two address
	   spaces apart: the whole thing has to go on each switch. That is the
	   price of ASID=0 and the reason real ASIDs are worth doing later. */
	ARCH_REG_STORE(TTBR0_EL1, ctx);
	isb();

	ARCH_REG_STORE(TLBI_VMALLE1, 0);
	dsb(sy);
	isb();
}

uintptr_t *mmu_get_root(mmu_ctx_t ctx) {
	return (uintptr_t *)FIELD_GET(ctx, TTBRn_ELn_BADDR);
}

void mmu_flush_tlb(void) {
	ARCH_REG_STORE(TLBI_VMALLE1, 0);
}

mmu_vaddr_t mmu_get_fault_address(void) {
	return ARCH_REG_LOAD(FAR_EL1);
}

uintptr_t *mmu_get(int lvl, uintptr_t *entry) {
	if (!entry) {
		log_error("Entry is NULL!");
		return 0;
	}

	return (uintptr_t *)(*entry & ~MMU_PAGE_MASK);
}

void mmu_set(int lvl, uintptr_t *entry, uintptr_t value) {
	if (!entry) {
		log_error("entry is NULL!");
		return;
	}

	if ((lvl < 0) || (lvl > MMU_LAST_LEVEL)) {
		log_error("wrong MMU level: %i (max %i)", lvl, MMU_LAST_LEVEL);
		return;
	}

	*entry = value | MMU_DESC_VD | MMU_DESC_TP;
	mmu_desc_written(entry, sizeof(*entry));
}

void mmu_unset(int lvl, uintptr_t *entry) {
	if (!entry) {
		log_error("entry is NULL!");
		return;
	}

	if ((lvl < 0) || (lvl > MMU_LAST_LEVEL)) {
		log_error("wrong MMU level: %i (max %i)", lvl, MMU_LAST_LEVEL);
		return;
	}

	*entry = 0;
	mmu_desc_written(entry, sizeof(*entry));
}

uintptr_t mmu_pte_pack(uintptr_t addr, int prot) {
	uintptr_t pte;

	if (addr & MMU_PAGE_MASK) {
		log_error("address %p doesn't match mask %p", (void *)addr,
		    (void *)MMU_PAGE_MASK);
		addr &= MMU_PAGE_MASK;
	}

	pte = addr | MMU_DESC_VD | MMU_DESC_TP | MMU_DESC_AF;

	/* XENOLITH_EL0_MMU: AP[1] is what makes a page reachable from EL0.
	   VMEM_PAGE_USERMODE in prot selects the AP*0 pair; without it the page
	   stays EL1-only, which is what every kernel mapping wants. */
	if (prot & VMEM_PAGE_USERMODE) {
		pte = FIELD_SET(pte, MMU_DESC_AP,
		    (prot & PROT_WRITE) ? MMU_DESC_AP_RW0 : MMU_DESC_AP_RO0);
	}
	else {
		pte = FIELD_SET(pte, MMU_DESC_AP,
		    (prot & PROT_WRITE) ? MMU_DESC_AP_RW1 : MMU_DESC_AP_RO1);
	}

	if (!(prot & PROT_READ)) {
		log_error("Setting page non-readable WTF");
	}

	if (prot & PROT_NOCACHE) {
		/* Use memory attribute 0 (device) */
		pte = FIELD_SET(pte, MMU_DESC_ATTRINDX, MEM_DEVICE_ATTRINDX);
		pte = FIELD_SET(pte, MMU_DESC_SH, MMU_DESC_SH_OS);
	}
	else {
		/* Use memory attribute 1 (normal) */
		pte = FIELD_SET(pte, MMU_DESC_ATTRINDX, MEM_NORMAL_ATTRINDX);
		pte = FIELD_SET(pte, MMU_DESC_SH, MMU_DESC_SH_IS);
	}

	/* XENOLITH_EL0_MMU: PXN governs execution at EL1, UXN at EL0. A user page
	   is never executed by the kernel and a kernel page is never executed by
	   the application, so the bit for the other level is set unconditionally;
	   PROT_EXEC only decides the bit for the level that owns the page. */
	if (prot & VMEM_PAGE_USERMODE) {
		pte |= MMU_DESC_PXN;
		if (!(prot & PROT_EXEC)) {
			pte |= MMU_DESC_UXN;
		}
	}
	else {
		pte |= MMU_DESC_UXN;
		if (!(prot & PROT_EXEC)) {
			pte |= MMU_DESC_PXN;
		}
	}

	return pte;
}

int mmu_pte_set(uintptr_t *entry, uintptr_t value) {
	assert(entry);

	*entry = value;
	mmu_desc_written(entry, sizeof(*entry));

	return 0;
}

uintptr_t mmu_pte_get(uintptr_t *entry) {
	if (entry == NULL) {
		log_error("Entry is NULL!");
		return 0;
	}

	return *entry;
}

int mmu_present(int lvl, uintptr_t *entry) {
	if (entry == NULL) {
		log_error("Entry is NULL!");
		return 0;
	}

	return !!(*entry & MMU_DESC_VD);
}

uintptr_t mmu_pte_unpack(uintptr_t pte, int *flags) {
	int tmp;

	assert(flags);

	if (!(pte & MMU_DESC_VD) || !(pte & MMU_DESC_TP)) {
		log_error("Trying to unpack corrupted PTE");
	}

	*flags = 0;

	/* XENOLITH_EL0_MMU: AP first -- whether the page is a user page decides
	   which execute-never bit describes it. */
	tmp = FIELD_GET(pte, MMU_DESC_AP);
	switch (tmp) {
	case MMU_DESC_AP_RW1:
		*flags |= PROT_READ | PROT_WRITE;
		break;
	case MMU_DESC_AP_RO1:
		*flags |= PROT_READ;
		break;
	case MMU_DESC_AP_RW0:
		*flags |= PROT_READ | PROT_WRITE | VMEM_PAGE_USERMODE;
		break;
	case MMU_DESC_AP_RO0:
		*flags |= PROT_READ | VMEM_PAGE_USERMODE;
		break;
	default:
		log_error("Corrupted PTE access properties");
	}

	if (*flags & VMEM_PAGE_USERMODE) {
		if (!(pte & MMU_DESC_UXN)) {
			*flags |= PROT_EXEC;
		}
	}
	else {
		if (!(pte & MMU_DESC_PXN)) {
			*flags |= PROT_EXEC;
		}
	}

	tmp = FIELD_GET(pte, MMU_DESC_ATTRINDX);
	switch (tmp) {
	case MEM_DEVICE_ATTRINDX:
		*flags |= PROT_NOCACHE;
		break;
	case MEM_NORMAL_ATTRINDX:
		break;
	default:
		log_error("Corrupted PTE cache properties");
	}

	return pte & ~MMU_PAGE_MASK;
}
