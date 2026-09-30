/**
 * @file
 * @brief Test for setjmp facility.
 *
 * @date 06.03.10
 * @author Eldar Abusalimov
 */

#include <setjmp.h>
#include <stdio.h>
#include <embox/test.h>

#define TEST_JUMP_VALUE 0xFADE

EMBOX_TEST_SUITE("stdlib/setjmp test");

TEST_CASE("Save context with 'setjmp' function than restore with 'longjmp' call") {
	jmp_buf jb;
	switch (setjmp(jb)) {
	case 0:
		longjmp(jb, TEST_JUMP_VALUE);
		test_fail("We passed longjmp call");
		break;

	case TEST_JUMP_VALUE:
		test_assert(true);
		break;

	default:
		test_fail("Setjmp return strange value");
		break;
	}
}

static volatile int zero_jumps;

TEST_CASE("longjmp with 0 makes setjmp return 1") {
	jmp_buf jb;

	zero_jumps = 0;
	switch (setjmp(jb)) {
	case 0:
		/* Without this check a second 0 would loop for good. */
		if (zero_jumps++) {
			test_fail("longjmp(env, 0) made setjmp return 0");
			break;
		}
		longjmp(jb, 0);
		test_fail("We passed longjmp call");
		break;

	case 1:
		break;

	default:
		test_fail("longjmp(env, 0) made setjmp return neither 0 nor 1");
		break;
	}
}
