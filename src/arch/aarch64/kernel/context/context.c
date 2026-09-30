/**
 * @file context.c
 * @brief aarch64 context_init with a 64-byte FPSIMD frame under SP:
 *        d8-d15, what context_switch saves.
 *
 * The frame is not zeroed: the bootstrap context is initialised with the
 * stack pointer it is running on, and clearing below it would overwrite
 * the caller's live frame. A new thread starts with undefined d8-d15.
 */
#include <stdint.h>
#include <string.h>

#include <hal/context.h>
#include <hal/reg.h>

#define FPSIMD_FRAME 64

void context_init(struct context *ctx, unsigned int flags,
    void (*routine_fn)(void), void *sp) {
	uintptr_t top;

	memset(ctx, 0, sizeof(*ctx));

	/* C11: a thread starts in its creator's floating-point environment. This
	 * runs on the creating thread (thread_init), so the registers are its. */
	__asm__ __volatile__("mrs %0, fpcr" : "=r"(ctx->fpcr));
	__asm__ __volatile__("mrs %0, fpsr" : "=r"(ctx->fpsr));

	ctx->lr = (uint64_t)routine_fn;
	/* stp of Q registers needs 16-byte alignment; thread stacks are only
	 * guaranteed 8. */
	top = (uintptr_t)sp & ~(uintptr_t)15;
	ctx->sp = (uint64_t)(top - FPSIMD_FRAME);

	if (!(flags & CONTEXT_IRQDISABLE)) {
		ctx->daif |= DAIF_I | DAIF_F;
	}
}
