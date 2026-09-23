/**
 * @file
 *
 * @brief
 *
 * @date 12.07.2018
 * @author Filipp Chubukov
 */

#include <embox/test.h>
#include <kernel/printk.h>
#include <mem/page.h>

EMBOX_TEST_SUITE("page_allocator_init test");

TEST_CASE("Init page_allocator with one page") {
	struct page_allocator *allocator;
	char buff[0x10];

	allocator =  page_allocator_init(buff, 0x10, 0x10);
	test_assert_null(allocator);
}

TEST_CASE("Init page_allocator with len < page_size") {
	struct page_allocator *allocator;
	char buff[0x100];

	allocator =  page_allocator_init(buff, 0x10, 0x100);
	test_assert_null(allocator);
}

/* A request larger than the pool has to be refused.
 *
 * It was not: search_multi_page() kept the index in a size_t and compared it
 * against the `-1' that search_first_free() returned as an unsigned int. On a
 * 64-bit target 0x00000000ffffffff is not (size_t)-1, so the sentinel was
 * carried on as an index, and page_i2ptr() -- which takes an int -- turned it
 * into pages_start + (-1 * page_size): one page BELOW the pool. On a pool
 * large enough for the scan to go round again it does not come back at all.
 *
 * On a 32-bit target size_t is as wide as the sentinel and nothing was ever
 * wrong, which is why this went unseen.
 *
 * Note for a bisect: at a commit that has this case without the fix in
 * src/mem/pagealloc/bitmask.c it can hang rather than fail. */
TEST_CASE("a request larger than the pool is refused") {
	static char buff[0x2000];
	struct page_allocator *allocator;
	void *p;

	allocator = page_allocator_init(buff, sizeof(buff), 0x100);
	test_assert_not_null(allocator);

	/* One more page than exists, and then a size that is absurd rather than
	 * merely too large -- the second is what a byte count mistaken for a page
	 * count looks like. The pointer and the pool are printed because "not
	 * NULL" does not say whether the allocator handed out memory it does not
	 * have or simply lost track of the index. */
	p = page_alloc(allocator, (size_t)allocator->pages_n + 1);
	if (p != NULL) {
		printk("page_alloc: %u+1 pages of %u answered %p; pool is %u pages at "
		       "%p..%p\n",
		    allocator->pages_n, (unsigned)allocator->page_size, p,
		    allocator->pages_n, allocator->pages_start,
		    (char *)allocator->pages_start
		        + allocator->pages_n * allocator->page_size);
	}
	test_assert_null(p);

	p = page_alloc(allocator, (size_t)-1 / 0x100);
	if (p != NULL) {
		printk("page_alloc: an absurd request answered %p\n", p);
	}
	test_assert_null(p);

	/* Still usable afterwards: a refused request must not have marked
	 * anything busy on its way out. */
	p = page_alloc(allocator, 1);
	test_assert_not_null(p);
	page_free(allocator, p, 1);
}

/* page_reserve() takes a range out for good: nothing inside it is handed out
 * afterwards, everything outside it still is, and the free count says so.
 * What it is for is a firmware carve-out inside the RAM region -- on a
 * Raspberry Pi 4 the VideoCore's memory, framebuffer included, which the
 * allocator used to hand out as free pages. */
TEST_CASE("a reserved range is never handed out, and the rest still is") {
	static char buff[0x4000];
	struct page_allocator *allocator;
	char *first, *lo, *hi, *p;
	size_t n, got = 0, before;

	allocator = page_allocator_init(buff, sizeof(buff), 0x100);
	test_assert_not_null(allocator);
	n = allocator->pages_n;
	test_assert(n > 8);
	/* The pool is inside the memory it was given */
	test_assert((char *)allocator->pages_start + n * 0x100
	    <= buff + sizeof(buff));

	first = allocator->pages_start;
	/* Pages 2..4, given by a range that starts and ends mid-page */
	lo = first + 2 * 0x100 + 0x10;
	hi = first + 4 * 0x100 + 0x80;
	before = allocator->free;
	test_assert_equal(3, page_reserve(allocator, lo, hi - lo));
	test_assert_equal(before - 3 * 0x100, allocator->free);

	/* Again: nothing more to take */
	test_assert_zero(page_reserve(allocator, lo, hi - lo));
	/* Outside the allocator: nothing either */
	test_assert_zero(page_reserve(allocator, buff + sizeof(buff), 0x1000));

	while (NULL != (p = page_alloc(allocator, 1))) {
		test_assert(p < first + 2 * 0x100 || p >= first + 5 * 0x100);
		got++;
	}
	test_assert_equal(n - 3, got);
}
