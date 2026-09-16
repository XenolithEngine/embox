/* XENOLITH_EL0_ENTRY */
/**
 * @file
 * @brief End a thread that has left EL0.
 *
 * aarch64_sync_handler() rewrites the exception frame so that the eret in
 * excpt_exit lands here: at EL1, on the thread's own kernel stack, with no
 * exception frame left live for the scheduler to run on top of. The reason in
 * x0 goes to thread_exit() for the joiner: tag 1 is an exit syscall with its
 * status in the low byte, tag 2 a fault with its ESR in the low word.
 *
 * Two landings (ABI section 6.1): exit(93) ends the calling thread only, while
 * exit_group(94) ends the task -- its threads, its address space, its memory.
 */
#include <stdint.h>

#include <kernel/task.h>
#include <kernel/thread.h>

void _NORETURN aarch64_usermode_dead(uint64_t reason) {
	thread_exit((void *)(uintptr_t)reason);
}

void _NORETURN aarch64_usermode_group_dead(uint64_t status) {
	task_exit((void *)(uintptr_t)(status & 0xff));
}
