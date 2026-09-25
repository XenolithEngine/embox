/**
 * @brief
 *
 * @date 15.12.22
 * @author Aleksey Zhmulin
 */
#ifndef ARCH_AARCH64_KERNEL_EXCEPTIONS_EXCEPTION_H_
#define ARCH_AARCH64_KERNEL_EXCEPTIONS_EXCEPTION_H_

#include <stdint.h>

struct excpt_context {
	uint64_t x[30];
	uint64_t lr;
	uint64_t sp;
	uint64_t pc;
	uint64_t psr;
};

extern void aarch64_print_excpt_context(struct excpt_context *ctx);

/* Divert a thread of a task being torn down to its exit instead of EL0
 * (sync_handler.c). */
extern void aarch64_el0_return_check(struct excpt_context *ctx);

#endif /* ARCH_AARCH64_KERNEL_EXCEPTIONS_EXCEPTION_H_ */
