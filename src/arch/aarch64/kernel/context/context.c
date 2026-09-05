/**
 * @file context.c
 * @brief aarch64 context_init with a 512-byte FPSIMD frame under SP
 *        (Xenolith overlay). struct context stays 120 bytes.
 *
 * Do not memset the frame: during sched init Embox context_init's the
 * bootstrap thread with sp = kernel stack top while already running
 * near that top. A 512-byte memset would wipe the live frame (saved
 * ctx pointer) and data-abort. First restore loads whatever is there;
 * a new thread does not care about initial Q.
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
	/* stp Qt requires 16-byte alignment. Pool used to be 8-aligned. */
	top = (uintptr_t)sp & ~(uintptr_t)15;
	ctx->sp = (uint64_t)(top - FPSIMD_FRAME);

	if (!(flags & CONTEXT_IRQDISABLE)) {
		ctx->daif |= DAIF_I | DAIF_F;
	}
}
