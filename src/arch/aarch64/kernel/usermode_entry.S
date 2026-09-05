/* XENOLITH_EL0_ENTRY */
/**
 * @file
 * @brief Enter EL0: the architecture half of kernel/usermode.c.
 *
 * Entry state (docs/EMBOX-SYSCALL-ABI.md section 3.1): SPSR_EL1 = 0 is EL0t
 * with every DAIF mask clear, so EL0 is preemptible and the timer IRQ keeps a
 * runaway user loop from owning the CPU. ELR_EL1 is the entry point, SP_EL0
 * the user stack, TPIDR_EL0 zero, and x0..x30 are zeroed so no kernel register
 * state reaches EL0.
 *
 * There is no return path: EL0 comes back only through an exception, and
 * aarch64_sync_handler decides what that means.
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

/** Sets the EL0 stack: from EL1 that is SP_EL0, not SP. */
.globl usermode_set_stack
.type usermode_set_stack, %function
usermode_set_stack:
	msr	sp_el0, x0
	ret
.size usermode_set_stack, . - usermode_set_stack
