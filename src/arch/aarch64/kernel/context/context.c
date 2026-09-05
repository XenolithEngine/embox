/**
 * @file context.c
 * @brief aarch64 context_init with a 512-byte FPSIMD frame under SP
 *        (Xenolith overlay). struct context stays 120 bytes.
 *
 * The frame is not zeroed: the bootstrap context is initialised with the
 * stack pointer it is running on, and clearing below it would overwrite
 * the caller's live frame. A thread that has never run has no Q state.
 */
#include <stdint.h>
#include <string.h>

#include <hal/context.h>
#include <hal/reg.h>

#define FPSIMD_FRAME 512

void context_init(struct context *ctx, unsigned int flags,
    void (*routine_fn)(void), void *sp) {
	uintptr_t top;

	memset(ctx, 0, sizeof(*ctx));

	ctx->lr = (uint64_t)routine_fn;
	/* stp of Q registers needs 16-byte alignment; thread stacks are only
	 * guaranteed 8. */
	top = (uintptr_t)sp & ~(uintptr_t)15;
	ctx->sp = (uint64_t)(top - FPSIMD_FRAME);

	if (!(flags & CONTEXT_IRQDISABLE)) {
		ctx->daif |= DAIF_I | DAIF_F;
	}
}
