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
 */
#include <stdint.h>

#include <kernel/thread.h>

void _NORETURN aarch64_usermode_dead(uint64_t reason) {
	thread_exit((void *)(uintptr_t)reason);
}
