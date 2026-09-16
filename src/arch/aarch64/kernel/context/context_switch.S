/**
 * @file context_switch.S
 * @brief Save/restore GPRs plus the EL0 pair in the TCB, and push q0–q31
 *        onto the thread stack *before* recording SP.
 *
 * Storing Q below the saved SP is invalid on AAPCS64 (no red zone; the
 * next IRQ's sub sp overwrites it). Lower SP, save Q, then save that SP.
 * context_init reserves 512 bytes at stack top so the first restore is safe.
 *
 * SP_EL0 and TPIDR_EL0 are at offsets 120 and 128 -- the two words
 * patches/aarch64-context-el0.py adds to struct context. They are EL0 state,
 * so the kernel never reads them, but a thread returning to EL0 does: without
 * this, a second EL0 thread would eret onto the first one's stack with the
 * first one's thread-local pointer.
 */
.text
.global context_switch
context_switch:
	/* x0 = save (current), x1 = load (next) */
	sub     sp, sp, #512
	stp     q0, q1, [sp, #0]
	stp     q2, q3, [sp, #32]
	stp     q4, q5, [sp, #64]
	stp     q6, q7, [sp, #96]
	stp     q8, q9, [sp, #128]
	stp     q10, q11, [sp, #160]
	stp     q12, q13, [sp, #192]
	stp     q14, q15, [sp, #224]
	stp     q16, q17, [sp, #256]
	stp     q18, q19, [sp, #288]
	stp     q20, q21, [sp, #320]
	stp     q22, q23, [sp, #352]
	stp     q24, q25, [sp, #384]
	stp     q26, q27, [sp, #416]
	stp     q28, q29, [sp, #448]
	stp     q30, q31, [sp, #480]

	mov     x2, sp
	mrs     x3, spsr_el1
	mrs     x4, daif
	mrs     x5, sp_el0
	mrs     x6, tpidr_el0
	stp     x19, x20, [x0, #0]
	stp     x21, x22, [x0, #16]
	stp     x23, x24, [x0, #32]
	stp     x25, x26, [x0, #48]
	stp     x27, x28, [x0, #64]
	stp     x29, x30, [x0, #80]
	stp     x2, x3, [x0, #96]
	str     x4, [x0, #112]
	stp     x5, x6, [x0, #120]

	ldp     x19, x20, [x1, #0]
	ldp     x21, x22, [x1, #16]
	ldp     x23, x24, [x1, #32]
	ldp     x25, x26, [x1, #48]
	ldp     x27, x28, [x1, #64]
	ldp     x29, x30, [x1, #80]
	ldp     x2, x3, [x1, #96]
	ldr     x4, [x1, #112]
	ldp     x5, x6, [x1, #120]
	mov     sp, x2
	ldp     q0, q1, [sp, #0]
	ldp     q2, q3, [sp, #32]
	ldp     q4, q5, [sp, #64]
	ldp     q6, q7, [sp, #96]
	ldp     q8, q9, [sp, #128]
	ldp     q10, q11, [sp, #160]
	ldp     q12, q13, [sp, #192]
	ldp     q14, q15, [sp, #224]
	ldp     q16, q17, [sp, #256]
	ldp     q18, q19, [sp, #288]
	ldp     q20, q21, [sp, #320]
	ldp     q22, q23, [sp, #352]
	ldp     q24, q25, [sp, #384]
	ldp     q26, q27, [sp, #416]
	ldp     q28, q29, [sp, #448]
	ldp     q30, q31, [sp, #480]
	add     sp, sp, #512
	msr     daif, x4
	msr     sp_el0, x5
	msr     tpidr_el0, x6
	ret
