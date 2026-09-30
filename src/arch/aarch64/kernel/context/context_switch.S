/**
 * @file context_switch.S
 * @brief Save/restore GPRs plus the EL0 pair in the TCB, and push d8-d15
 *        onto the thread stack *before* recording SP.
 *
 * Only the low 64 bits of d8-d15: that is all AAPCS64 asks a callee to
 * preserve, and context_switch is only reached as a C call
 * (thread_context_switch, the HAL test, the bootloader's goto). A preempted
 * thread's live q0-q31, fpsr and fpcr are in its exception frame (entry.S).
 *
 * Storing below the saved SP is invalid on AAPCS64 (no red zone; the next
 * IRQ's sub sp overwrites it). Lower SP, save D, then save that SP.
 * context_init reserves FPSIMD_FRAME bytes at stack top so the first restore
 * is safe.
 *
 * SP_EL0 and TPIDR_EL0 sit at offsets 120 and 128 of struct context. The
 * kernel never reads them, but each EL0 thread returns to its own pair.
 *
 * FPCR and FPSR are at 136 and 144 (XENOLITH_CONTEXT_FP in context.h): the
 * callee keeps FPCR by the ABI, and each thread has its own floating-point
 * environment by C11. They live in the TCB because context_init gives a new
 * thread its creator's values, and the frame may be on the bootstrap thread's
 * live stack. Each is written only when the incoming value differs: an FPCR
 * write is not free.
 */
.text
.global context_switch
context_switch:
	/* x0 = save (current), x1 = load (next) */
	sub     sp, sp, #64
	stp     d8, d9, [sp, #0]
	stp     d10, d11, [sp, #16]
	stp     d12, d13, [sp, #32]
	stp     d14, d15, [sp, #48]

	mov     x2, sp
	mrs     x3, spsr_el1
	mrs     x4, daif
	mrs     x5, sp_el0
	mrs     x6, tpidr_el0
	mrs     x7, fpcr
	mrs     x8, fpsr
	stp     x19, x20, [x0, #0]
	stp     x21, x22, [x0, #16]
	stp     x23, x24, [x0, #32]
	stp     x25, x26, [x0, #48]
	stp     x27, x28, [x0, #64]
	stp     x29, x30, [x0, #80]
	stp     x2, x3, [x0, #96]
	str     x4, [x0, #112]
	stp     x5, x6, [x0, #120]
	stp     x7, x8, [x0, #136]

	ldp     x19, x20, [x1, #0]
	ldp     x21, x22, [x1, #16]
	ldp     x23, x24, [x1, #32]
	ldp     x25, x26, [x1, #48]
	ldp     x27, x28, [x1, #64]
	ldp     x29, x30, [x1, #80]
	ldp     x2, x3, [x1, #96]
	ldr     x4, [x1, #112]
	ldp     x5, x6, [x1, #120]
	ldp     x9, x10, [x1, #136]
	mov     sp, x2
	ldp     d8, d9, [sp, #0]
	ldp     d10, d11, [sp, #16]
	ldp     d12, d13, [sp, #32]
	ldp     d14, d15, [sp, #48]
	add     sp, sp, #64
	msr     daif, x4
	msr     sp_el0, x5
	msr     tpidr_el0, x6
	cmp     x7, x9
	b.eq    1f
	msr     fpcr, x9
1:
	cmp     x8, x10
	b.eq    2f
	msr     fpsr, x10
2:
	ret
