/* XENOLITH_EL0_ENTRY */
/**
 * @file
 * @brief Enter EL0 (Xenolith K2 overlay).
 *
 * The architecture half of kernel/usermode.c. Upstream ships this for x86
 * only; on aarch64 embox.arch.usermode resolved to the generic no-op stub.
 *
 * Entry state follows docs/EMBOX-SYSCALL-ABI.md section 3.1: SPSR_EL1 selects
 * EL0t with every DAIF mask clear (EL0 runs preemptible -- the timer IRQ is
 * what keeps a runaway user loop from owning the CPU), ELR_EL1 is the entry
 * point, SP_EL0 the user stack, TPIDR_EL0 zero, and x0..x30 are handed over
 * zeroed so nothing of the kernel's register state leaks across the boundary.
 *
 * SPSR = 0 *is* "EL0t, DAIF clear": M[4:0] = 0b00000 and D/A/I/F sit at bits
 * 9..6. Written as xzr rather than a named constant so it is obvious that no
 * bit is being left to chance.
 *
 * There is no return path here. EL0 comes back only through an exception, and
 * aarch64_sync_handler decides what that means.
 *
 * usermode_entry_clone is the same entry for a thread created by clone(220):
 * the difference is that its stack, its TLS pointer and its return address come
 * from the caller instead of being a fresh program's. x0 is zero either way,
 * which for the clone child is not tidiness but the ABI -- it is the value
 * clone() returns in the child.
 */

.section .text
.balign 4

.globl usermode_entry
.type usermode_entry, %function

/* void usermode_entry(struct ue_data *data)
 *   struct ue_data { void *ip; void *sp; }  -- kernel/usermode.h
 */
usermode_entry:
	ldr	x1, [x0, #0]		/* data->ip */
	ldr	x2, [x0, #8]		/* data->sp */

	msr	elr_el1, x1
	msr	sp_el0, x2
	msr	tpidr_el0, xzr
	msr	spsr_el1, xzr		/* EL0t, DAIF clear */

	mov	x0, xzr
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	mov	x4, xzr
	mov	x5, xzr
	mov	x6, xzr
	mov	x7, xzr
	mov	x8, xzr
	mov	x9, xzr
	mov	x10, xzr
	mov	x11, xzr
	mov	x12, xzr
	mov	x13, xzr
	mov	x14, xzr
	mov	x15, xzr
	mov	x16, xzr
	mov	x17, xzr
	mov	x18, xzr
	mov	x19, xzr
	mov	x20, xzr
	mov	x21, xzr
	mov	x22, xzr
	mov	x23, xzr
	mov	x24, xzr
	mov	x25, xzr
	mov	x26, xzr
	mov	x27, xzr
	mov	x28, xzr
	mov	x29, xzr
	mov	x30, xzr

	isb
	eret

.size usermode_entry, . - usermode_entry

.globl usermode_entry_clone
.type usermode_entry_clone, %function

/* void usermode_entry_clone(struct xl_clone_entry *data)
 *   struct xl_clone_entry { void *ip; void *sp; void *tls; }
 *
 * ip is the parent's return address from its own `svc`: the child resumes
 * exactly where the parent did, on its own stack, and tells itself apart by
 * x0 == 0.
 */
usermode_entry_clone:
	ldr	x1, [x0, #0]		/* data->ip  */
	ldr	x2, [x0, #8]		/* data->sp  */
	ldr	x3, [x0, #16]		/* data->tls */

	msr	elr_el1, x1
	msr	sp_el0, x2
	msr	tpidr_el0, x3
	msr	spsr_el1, xzr		/* EL0t, DAIF clear */

	mov	x0, xzr
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	mov	x4, xzr
	mov	x5, xzr
	mov	x6, xzr
	mov	x7, xzr
	mov	x8, xzr
	mov	x9, xzr
	mov	x10, xzr
	mov	x11, xzr
	mov	x12, xzr
	mov	x13, xzr
	mov	x14, xzr
	mov	x15, xzr
	mov	x16, xzr
	mov	x17, xzr
	mov	x18, xzr
	mov	x19, xzr
	mov	x20, xzr
	mov	x21, xzr
	mov	x22, xzr
	mov	x23, xzr
	mov	x24, xzr
	mov	x25, xzr
	mov	x26, xzr
	mov	x27, xzr
	mov	x28, xzr
	mov	x29, xzr
	mov	x30, xzr

	isb
	eret

.size usermode_entry_clone, . - usermode_entry_clone

/**
 * Only switches to a new stack. Declared by kernel/usermode.h; nothing in the
 * tree calls it, and on aarch64 "switch the EL0 stack" from EL1 means writing
 * SP_EL0, not SP.
 */
.globl usermode_set_stack
.type usermode_set_stack, %function
usermode_set_stack:
	msr	sp_el0, x0
	ret
.size usermode_set_stack, . - usermode_set_stack
