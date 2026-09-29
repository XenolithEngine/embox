/**
 * @file
 * @brief Does clock_gettime() see time between two ticks?
 *
 * A clock source without a counter device has the tick's resolution: the
 * clock stands still for a whole period and then jumps by it. Every interval
 * shorter than a tick then measures as zero or as one tick, and a sum of
 * such intervals is noise. On aarch64 that was armv8_phy_timer until it read
 * CNTPCT_EL0.
 */

#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <embox/test.h>
#include <hal/clock.h>
#include <kernel/printk.h>
#include <kernel/time/time.h>

EMBOX_TEST_SUITE("clock_gettime resolves time between ticks");

static uint64_t read_ns(clockid_t id) {
	struct timespec ts;

	clock_gettime(id, &ts);
	return (uint64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

/* The smallest step the clock takes, over a few hundred changes. */
static uint64_t smallest_step(clockid_t id) {
	uint64_t best = UINT64_MAX, prev, now;
	int changes = 0, reads = 0;

	prev = read_ns(id);
	while (changes < 256 && reads < 10000000) {
		now = read_ns(id);
		reads++;
		test_assert(now >= prev);
		if (now != prev) {
			if (now - prev < best) {
				best = now - prev;
			}
			prev = now;
			changes++;
		}
	}
	return best;
}

TEST_CASE("CLOCK_MONOTONIC steps by less than a tick") {
	uint64_t step = smallest_step(CLOCK_MONOTONIC);

	printk("clock_resolution: MONOTONIC smallest step %llu ns\n",
	    (unsigned long long)step);
	test_assert(step < NSEC_PER_MSEC / 10);
}

TEST_CASE("CLOCK_REALTIME steps by less than a tick") {
	uint64_t step = smallest_step(CLOCK_REALTIME);

	printk("clock_resolution: REALTIME smallest step %llu ns\n",
	    (unsigned long long)step);
	test_assert(step < NSEC_PER_MSEC / 10);
}

/* The counter and the tick must agree on how long a second is: a counter
 * frequency taken wrongly would make the clock run fast or slow against
 * every timeout in the kernel, which counts ticks. */
TEST_CASE("the clock and the tick agree over 200 ms") {
	uint64_t t0, t1, clock_ms;
	clock_t j0, j1;

	j0 = clock_sys_ticks();
	t0 = read_ns(CLOCK_MONOTONIC);
	usleep(200 * 1000);
	j1 = clock_sys_ticks();
	t1 = read_ns(CLOCK_MONOTONIC);

	clock_ms = (t1 - t0) / NSEC_PER_MSEC;
	printk("clock_resolution: 200 ms sleep: clock %llu ms, ticks %lu\n",
	    (unsigned long long)clock_ms, (unsigned long)(j1 - j0));
	test_assert(clock_ms >= 200);
	test_assert(clock_ms + 3 >= (uint64_t)(j1 - j0)
	            && (uint64_t)(j1 - j0) + 3 >= clock_ms);
}
