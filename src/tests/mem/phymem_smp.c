/**
 * @file
 * @brief phymem from several cores at once
 *
 * The page allocator had no lock ("TODO page: have no synchronization" in
 * bitmask.c), and under SMP more than one core asks it for memory: EL0 mmap
 * stripes, page tables, an application's own allocator. Two cores searching
 * the same bitmap can both find the same free run and both take it.
 *
 * Each thread takes runs of random length, keeps up to KEEP of them, and marks
 * every page with its own id and the run's serial. Before a run goes back its
 * marks are checked: a page two threads were handed has the other's mark. At
 * the end the free count is what it was, or a run was lost or freed twice.
 */

#include <stdint.h>
#include <string.h>

#include <embox/test.h>
#include <kernel/printk.h>
#include <kernel/thread.h>
#include <mem/page.h>
#include <mem/phymem.h>
#include <util/err.h>

EMBOX_TEST_SUITE("phymem from several cores at once");

#define THREADS 4
#define ROUNDS  4000
#define KEEP    8
#define MAX_RUN 16

struct run {
	uint64_t *pages;
	size_t n;
	uint64_t mark;
};

static volatile unsigned long collisions;
static volatile unsigned long refused;

static uint64_t make_mark(unsigned id, unsigned long serial) {
	return ((uint64_t)(id + 1) << 48) | serial;
}

static void run_mark(struct run *r) {
	size_t i;

	for (i = 0; i < r->n; i++) {
		r->pages[i * (PAGE_SIZE() / sizeof(uint64_t))] = r->mark;
	}
}

static int run_check(const struct run *r) {
	size_t i;

	for (i = 0; i < r->n; i++) {
		if (r->pages[i * (PAGE_SIZE() / sizeof(uint64_t))] != r->mark) {
			return -1;
		}
	}
	return 0;
}

static void *worker(void *arg) {
	unsigned id = (unsigned)(uintptr_t)arg;
	struct run held[KEEP] = {};
	unsigned long x = 0x9e3779b9ul * (id + 1);
	unsigned long round;
	unsigned slot;

	for (round = 0; round < ROUNDS; round++) {
		x = x * 6364136223846793005ul + 1442695040888963407ul;
		slot = (unsigned)(x >> 33) % KEEP;

		if (held[slot].pages) {
			if (run_check(&held[slot])) {
				__atomic_add_fetch(&collisions, 1, __ATOMIC_RELAXED);
			}
			phymem_free(held[slot].pages, held[slot].n);
			held[slot].pages = NULL;
			continue;
		}

		held[slot].n = 1 + (size_t)((x >> 17) % MAX_RUN);
		held[slot].pages = phymem_alloc(held[slot].n);
		if (!held[slot].pages) {
			__atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
			continue;
		}
		held[slot].mark = make_mark(id, round);
		run_mark(&held[slot]);
	}

	for (slot = 0; slot < KEEP; slot++) {
		if (held[slot].pages) {
			if (run_check(&held[slot])) {
				__atomic_add_fetch(&collisions, 1, __ATOMIC_RELAXED);
			}
			phymem_free(held[slot].pages, held[slot].n);
		}
	}
	return NULL;
}

TEST_CASE("no run of pages is handed to two threads, and every page comes back") {
	struct thread *t[THREADS];
	size_t free_before = phy_allocator()->free;
	unsigned i;

	collisions = 0;
	refused = 0;

	for (i = 0; i < THREADS; i++) {
		t[i] = thread_create(0, worker, (void *)(uintptr_t)i);
		test_assert_zero(ptr2err(t[i]));
	}
	for (i = 0; i < THREADS; i++) {
		test_assert_zero(thread_join(t[i], NULL));
	}

	printk("phymem_smp: %u thread(s) x %u rounds, %lu refused, %lu collision(s)\n",
	    THREADS, ROUNDS, refused, collisions);

	test_assert_zero(collisions);
	test_assert_equal(free_before, phy_allocator()->free);
}
