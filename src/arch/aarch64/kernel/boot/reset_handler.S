/**
 * @file
 * @brief
 *
 * @date 17.10.23
 * @author Aleksey Zhmulin
 */
#include <hal/cpu.h>
#include <hal/reg.h>

.weak gicv3_init_el3

/* Where the device tree pointer is kept. .data, because .bss is
 * zeroed long after the store below. See the patch that put this here. */
.section .data
.global aarch64_boot_x0
.align 3
aarch64_boot_x0:
	.quad 0

.section .bootcode, "ax", %progbits
.global aarch64_reset_handler

aarch64_reset_handler:
	/* The firmware hands the flattened device tree address in x0,
	 * and the next instruction overwrites it. Save it before anything else
	 * happens; first writer wins, so a secondary core cannot displace the
	 * primary's value. Plain store: the MMU is off here. */
	adrp    x1, aarch64_boot_x0
	add     x1, x1, #:lo12:aarch64_boot_x0
	ldr     x2, [x1]
	cbnz    x2, 1f
	str     x0, [x1]
1:

	/* X20 says which kind of core this is, x21 says which one. Both survive
	 * the descent: AAPCS makes x19-x28 callee-saved and the only calls below
	 * are into C.
	 *
	 * The secondary halves are #ifdef SMP because aarch64_ap_boot and
	 * aarch64_ap_start belong to embox.arch.aarch64.smp, and a board that does
	 * not include that module must still link. x20 stays -- it costs one
	 * instruction and keeps one shape for both builds. */
	mov     x20, xzr
	mov     x21, xzr
	b       aarch64_common_entry

#ifdef SMP
.global aarch64_ap_entry
aarch64_ap_entry:
	/* PSCI CPU_ON started this core here, with x0 = the context id we asked
	 * it to deliver, which is the logical CPU id. MMU off, caches off, same
	 * exception level the primary arrived at. */
	mov     x21, x0
	mov     x20, #1
	b       aarch64_common_entry

.global aarch64_ap_entry_mpidr
aarch64_ap_entry_mpidr:
	/* The same descent, for a core released off a spin table instead. The
	 * Pi's armstub -- and QEMU's model of it -- jumps here with x0..x3
	 * zeroed, so nothing carries the id and the core reads its own: Aff0 is
	 * what this port hands out (ap_mpidr() in arch/smp/smp.c). */
	mrs     x21, mpidr_el1
	and     x21, x21, #0xff
	mov     x20, #1
#endif /* SMP */

aarch64_common_entry:
	/* Mask Interrupts, System Errors and Debug */
	msr     daifset, #(DAIF_D_imm | DAIF_A_imm | DAIF_I_imm | DAIF_F_imm)

	/* Get current exception level */
	mrs     x0, CurrentEL
	cmp     x0, #0b1000 /* EL, bits [3:2] */
	bgt     el3_entry
	beq     el2_entry
	blo     el1_entry
	b       die

el3_entry:
	/* Setup stack: _stack_top is the image's, and there is one of it. A
	 * secondary uses the one the primary put at offset 0 of aarch64_ap_boot
	 * before it asked for this core. */
#ifdef SMP
	cbz     x20, 8f
	/* This core's own entry, not the one entry. x21 is
	 * the logical id; the entries are one cache line each, hence lsl #6, and
	 * a _Static_assert in smp.c holds the two together. */
	ldr     x0, =aarch64_ap_boot
	add     x0, x0, x21, lsl #6
	ldr     x0, [x0]
	b       9f
8:
#endif
	ldr     x0, =_stack_top
#ifdef SMP
9:
#endif
	mov     sp, x0

	/* Disable the MMU for EL3 */
	dsb     sy
	mrs     x0, sctlr_el3
	bl      flush_cache_if_mmu_on
	msr     sctlr_el3, xzr
	isb

	/* Reset SCTLR_EL2 and HCR_EL2 */
	msr     sctlr_el2, xzr
	msr     hcr_el2, xzr

	/* EL2 Execution state is AArch64, EL0 and EL1 are in Non-Secure state */
	mov     x0, #(SCR_EL3_RW | SCR_EL3_NS)
	msr     scr_el3, x0

	/* Exception taken from AArch64 EL2h */
	mov     x0, #(SPSR_ELn_D | SPSR_ELn_A | SPSR_ELn_I | SPSR_ELn_F | SPSR_ELn_M_EL2h)
	msr     spsr_el3, x0

	/* Configure GICv3 */
	ldr     x0, =gicv3_init_el3
	cmp     x0, #0
	beq     0f
	blr     x0
0:
	/* Determine EL2 entry */
	adr     x0, el2_entry
	msr     elr_el3, x0
	isb

	/* Drop to EL2 */
	eret

el2_entry:
	/* Setup stack: _stack_top is the image's, and there is one of it. A
	 * secondary uses the one the primary put at offset 0 of aarch64_ap_boot
	 * before it asked for this core. */
#ifdef SMP
	cbz     x20, 8f
	/* This core's own entry, not the one entry. x21 is
	 * the logical id; the entries are one cache line each, hence lsl #6, and
	 * a _Static_assert in smp.c holds the two together. */
	ldr     x0, =aarch64_ap_boot
	add     x0, x0, x21, lsl #6
	ldr     x0, [x0]
	b       9f
8:
#endif
	ldr     x0, =_stack_top
#ifdef SMP
9:
#endif
	mov     sp, x0

	/* Disable the MMU for EL2 */
	dsb     sy
	mrs     x0, sctlr_el2
	bl      flush_cache_if_mmu_on
	msr     sctlr_el2, xzr
	isb

	/* EL1 stage 2 address translation disabled */
	/* EL1 Execution state is AArch64 */
	mov     x0, #(HCR_EL2_RW)
	msr     hcr_el2, x0

	/* Reset SCTLR_EL1 */
	msr     sctlr_el1, xzr

	/* Don't trap to EL2 from EL1 */
	msr     hstr_el2, xzr
	msr     cptr_el2, xzr

	/* Reset VTTBR_EL2 because TLB invalidation depends on the VMID */
	msr     vttbr_el2, xzr

	/* Exception taken from AArch64 EL1h */
	mov     x0, #(SPSR_ELn_D | SPSR_ELn_A | SPSR_ELn_I | SPSR_ELn_F | SPSR_ELn_M_EL1h)
	msr     spsr_el2, x0

	/* Enable access to the physical timers at EL1 */
	mrs     x0, cnthctl_el2
	orr     x0, x0, #(CNTHCTL_EL2_EL1PCTEN | CNTHCTL_EL2_EL1PCEN)
	msr     cnthctl_el2, x0

	/* Set the address to return to our return address */
	adr     x0, el1_entry
	msr     elr_el2, x0
	isb

	/* Drop to EL1 */
	eret

el1_entry:
	/* Setup stack: _stack_top is the image's, and there is one of it. A
	 * secondary uses the one the primary put at offset 0 of aarch64_ap_boot
	 * before it asked for this core. */
#ifdef SMP
	cbz     x20, 8f
	/* This core's own entry, not the one entry. x21 is
	 * the logical id; the entries are one cache line each, hence lsl #6, and
	 * a _Static_assert in smp.c holds the two together. */
	ldr     x0, =aarch64_ap_boot
	add     x0, x0, x21, lsl #6
	ldr     x0, [x0]
	b       9f
8:
#endif
	ldr     x0, =_stack_top
#ifdef SMP
9:
#endif
	mov     sp, x0

	/* Disable the MMU for EL0 and EL1 */
	dsb     sy
	mrs     x0, sctlr_el1
	bl      flush_cache_if_mmu_on
	msr     sctlr_el1, xzr
	isb

	/* Set up exception vectors */
	adr     x0, aarch64_kernel_vectors
	msr     vbar_el1, x0

	/* The logical CPU id lives in TPIDR_EL1 and its reset value is UNKNOWN.
	 * cpu_get_id() reads it, and cpudata_var() reads cpu_get_id() -- including
	 * __critical_count, which the SError handler unmasked on the next line
	 * would touch first thing. The primary core is 0; secondaries write their
	 * own id on their own path into this handler. */
	msr     tpidr_el1, x21

	/* Unmask System Errors */
	msr     daifclr, #(DAIF_A_imm)

	/* Enable FP/SIMD */
	mov     x0, #(0x3 << 20)
	msr     cpacr_el1, x0

	/* Everything from here to kernel_start happens once per image, not once
	 * per core. A secondary that zeroed .bss again would erase the kernel that
	 * started it. */
#ifdef SMP
	cbz     x20, 7f
	mov     x0, x21
	bl      aarch64_ap_start
	b       die
7:
#endif

	/* Zero .bss */
	ldr     x0, =_bss_vma
	mov     x1, #0
	ldr     x2, =_bss_len
bss_loop:
	str     x1, [x0, 0]
	add     x0, x0, #8
	subs    x2, x2, #8
	b.gt    bss_loop

	/* Copy .data */
	ldr     x0, =_data_vma
	ldr     x1, =_data_lma
	ldr     x2, =_data_len

	/* Don't copy if same address */
	cmp     x0, x1
	beq     data_loop_done

data_loop:
	ldr     x3, [x1, 0]
	str     x3, [x0, 0]
	add     x0, x0, #8
	add     x1, x1, #8
	subs    x2, x2, #8
	bgt     data_loop
data_loop_done:

#if NCPU > 1
	/* The linker script repeats the per-CPU block NCPU
	 * times but initialises only the first. Copy it over the rest, after
	 * .data is in place and before anything takes a critical section. */
	bl      cpudata_init
#endif /* NCPU > 1 */

	/* Jump to arch-independent part */
	bl      kernel_start

	/* Returning from kernel_start */
die:
	b       die

flush_cache_if_mmu_on:
	/* The bootloader may have been running with translation on, with the
	 * data cache on, or with both. Either one means the image we are about
	 * to run out of, and everything else it left behind, can be sitting in
	 * a cache that the caller is one instruction away from turning off.
	 *
	 * x22, not x20: x20 says which kind of core this is and x21 says which
	 * one, and both have to survive the whole descent -- see the entry
	 * points at the top of this file. AAPCS makes x19-x28 callee-saved, so
	 * the C calls below leave them alone; this function must too. */
	mov     x19, lr
	mov     x22, x0
	/* Two tests, not one: an AArch64 logical immediate is a rotated run of
	 * ones, and M|C is two separate bits. */
	tst     x22, SCTLR_ELn_M
	bne     1f
	tst     x22, SCTLR_ELn_C
	beq     0f
1:

	/* First clean what we need to keep, by virtual address. */
	ldr     x0, =_data_lma
	ldr     x1, =_data_len
	bl      dcache_flush

	ldr     x0, =_rodata_lma
	ldr     x1, =_rodata_len
	bl      dcache_flush

	ldr     x0, =_text_lma
	ldr     x1, =_text_len
	bl      dcache_flush

	/* Then drop every remaining line, because what the bootloader left in
	 * them is not ours and the page tables we are about to build are not
	 * allowed to find it. This has to happen here: DC ISW does nothing once
	 * SCTLR.C is clear, and clearing it is the caller's next instruction.
	 * The U-Boot handoff on RK3566/RK3588 needs it; a board reached by
	 * `-kernel` or by an armstub arrives with both bits clear and never
	 * takes this path at all. */
	bl      dcache_inval_all_sw
	ic      iallu
	dsb     sy
	isb
0:
	ret     x19

/* Invalidate the whole data cache by set/way, registers only -- no stack, no
 * memory, because there may be nothing trustworthy in either. Set/way is the
 * only way to reach lines whose addresses we do not know. Transcribed from
 * U-Boot v2026.07 __asm_invalidate_dcache_all, including its FEAT_CCIDX
 * handling: CCSIDR_EL1 lays the fields out differently when that is
 * implemented, and reading the old layout on a CPU that has it walks the
 * wrong number of sets. */
dcache_inval_all_sw:
	dsb     sy
	mrs     x10, clidr_el1
	ubfx    x11, x10, #24, #3       /* LoC */
	cbz     x11, 9f
	mov     x15, lr
	mrs     x16, id_aa64mmfr2_el1
	ubfx    x16, x16, #20, #4       /* FEAT_CCIDX */
	mov     x0, #0
2:
	add     x12, x0, x0, lsl #1
	lsr     x12, x10, x12
	and     x12, x12, #7            /* cache type at this level */
	cmp     x12, #2
	b.lt    8f                      /* no data or unified cache here */
	lsl     x12, x0, #1
	msr     csselr_el1, x12
	isb
	mrs     x6, ccsidr_el1
	ubfx    x2, x6, #0, #3          /* log2(line length) - 4 */
	cbz     x16, 3f
	ubfx    x3, x6, #3, #21         /* associativity, CCIDX */
	ubfx    x4, x6, #32, #24        /* sets, CCIDX */
	b       4f
3:
	ubfx    x3, x6, #3, #10         /* associativity */
	ubfx    x4, x6, #13, #15        /* sets */
4:
	add     x2, x2, #4
	clz     w5, w3
5:
	mov     x6, x3
6:
	lsl     x7, x6, x5
	orr     x9, x12, x7
	lsl     x7, x4, x2
	orr     x9, x9, x7
	dc      isw, x9
	subs    x6, x6, #1
	b.ge    6b
	subs    x4, x4, #1
	b.ge    5b
8:
	add     x0, x0, #1
	cmp     x11, x0
	b.gt    2b
	mov     x0, #0
	msr     csselr_el1, x0
	dsb     sy
	isb
	mov     lr, x15
9:
	ret
