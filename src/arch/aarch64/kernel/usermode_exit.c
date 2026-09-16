/* XENOLITH_EL0_ENTRY */
/**
 * @file
 * @brief End a thread that has left EL0 (Xenolith K2 overlay).
 *
 * aarch64_sync_handler() does not end the thread from inside the exception
 * frame -- that would run the scheduler on the exception stack while the frame
 * is still live. It rewrites the frame instead, so the eret in excpt_exit
 * lands here: at EL1, on the thread's own kernel stack, with the reason in x0
 * and nothing of the exception left on the stack.
 *
 * The reason is carried through to thread_exit(), so whoever joins the thread
 * learns how it ended: tag 1 is an exit syscall with its status in the low
 * byte, tag 2 a fault with its ESR in the low word.
 *
 * Two landings, because EL0 has two ways of ending (ABI section 6.1). exit(93)
 * ends the calling thread and nothing else -- with threads, the other threads
 * of the task keep running. exit_group(94) ends the task: its threads, its
 * address space, its memory. Before K6 there was never more than one EL0
 * thread and the two were the same thing.
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
