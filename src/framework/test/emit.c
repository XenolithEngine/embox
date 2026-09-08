/**
 * @file
 * @brief TODO documentation for emit.c -- Eldar Abusalimov
 *
 * @date Apr 2, 2012
 * @author Eldar Abusalimov
 */

#include <framework/test/emit.h>
#include <hal/ipl.h>
#include <kernel/spinlock.h>

/* Masking interrupts is not a lock once there is more than
 * one core, and this buffer is written by every thread a suite starts.
 *
 * Two cores in here at once both read b->ptr, both store through it and both
 * advance it, so one character is lost and -- worse -- the overflow branch
 * below increments b->ptr with nothing to stop it. Past the end of the buffer
 * the `*b->ptr = '\0'` above writes into whatever follows. What that looked
 * like was a synchronous exception with pc pointing INTO THE STACK, out of a
 * printf that had nothing to do with it, in a suite that only had to emit
 * eleven characters.
 *
 * One lock for every buffer: emitting is a few instructions and suites do it
 * rarely, so there is nothing to gain from a lock per buffer and one less
 * thing to get wrong. */
static spinlock_t emit_lock = SPIN_STATIC_UNLOCKED;

void test_emit_into(struct test_emit_buffer *b, char ch) {
	ipl_t ipl = spin_lock_ipl(&emit_lock);
	{
		if (test_emit_buffer_overflown(b)) {
			goto out;
		}

		if (!test_emit_buffer_full(b)) {
			*b->ptr++ = ch;
			*b->ptr = '\0';
		}
		else {
			/* do overflow */
			b->ptr++;
		}
	}
out:
	spin_unlock_ipl(&emit_lock, ipl);
}
