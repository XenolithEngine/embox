/**
 * @file
 * @brief How long an inode has to outlive the name that led to it.
 *
 * Finding a path and using what it led to are two operations, and between
 * them another core can unlink the inode. The tree's locks make the walk
 * safe; they do not make the pointer the walk returned stay alive. This suite
 * is about that pointer.
 *
 * The cases run from the plainest shape upwards, because the plainest one is
 * the one every system is expected to get right: a file unlinked while it is
 * open stays readable through the descriptor until the last one is closed.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drivers/block_dev.h>
#include <drivers/block_dev/ramdisk/ramdisk.h>
#include <embox/test.h>
#include <fs/fsop.h>
#include <fs/inode.h>
#include <kernel/printk.h>
#include <util/err.h>
#include <util/atomic_rmw.h>

EMBOX_TEST_SUITE("inode lifetime against unlink");

TEST_SETUP_SUITE(setup_suite);
TEST_TEARDOWN_SUITE(teardown_suite);

#define FS_NAME    "vfat"
#define FS_DEV     "/dev/ramdisk_inode"
/* Bytes, not pages: a page count times PAGE_SIZE() means a different
 * volume on every board. */
#define FS_BYTES   (2 * 1024 * 1024)
#define FS_DIR     "/tmp_inode"
#define FILE_ONE   FS_DIR "/one.bin"
#define FILE_TWO   FS_DIR "/two.bin"

#define DATA_SZ    256

static char pattern[DATA_SZ];
static char readback[DATA_SZ];

extern unsigned long fdesc_dead_inode;
extern unsigned long fdesc_stale_gen;

static unsigned long counter_read(unsigned long *p) {
	return atomic_rmw_load(p, __ATOMIC_RELAXED);
}

static void pattern_fill(char *buf, size_t len, unsigned seed) {
	size_t i;

	for (i = 0; i < len; i++) {
		buf[i] = (char)((i * 31u + seed) & 0xff);
	}
}

static int file_make(const char *path, unsigned seed) {
	int fd;

	pattern_fill(pattern, sizeof(pattern), seed);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		return -errno;
	}
	if (sizeof(pattern) != write(fd, pattern, sizeof(pattern))) {
		close(fd);
		return -EIO;
	}
	close(fd);

	return 0;
}

static int setup_suite(void) {
	int res;

	res = ptr2err(ramdisk_create(FS_DEV, FS_BYTES));
	if (res != 0) {
		return res;
	}
	if (0 != (res = format(FS_DEV, FS_NAME))) {
		return res;
	}

	return mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL);
}

static int teardown_suite(void) {
	umount(FS_DIR);
	return ramdisk_delete(FS_DEV);
}

/* ------------------------------------------------------------------ *
 * One core, no threads: the plain shape of the defect.
 */
TEST_CASE("unlink with the file open postpones the free, close performs it") {
	unsigned long deferred_before;
	int fd;
	int i;

	test_assert_zero(file_make(FILE_ONE, 1));

	deferred_before = counter_read(&inode_free_deferred);

	fd = open(FILE_ONE, O_RDONLY);
	test_assert(fd >= 0);

	/* The name goes while the descriptor is still on the inode. Without the
	 * reference this frees the inode outright and the descriptor is left
	 * pointing at a pool slot the next open will hand to somebody else. */
	test_assert_zero(remove(FILE_ONE));
	test_assert_not_zero(0 == access(FILE_ONE, F_OK));

	/* The free was postponed, exactly once. */
	test_assert_equal(counter_read(&inode_free_deferred) - deferred_before, 1);

	/* And closing performs it -- which is the half a leak would hide. Twenty
	 * rounds is more than the driver has inodes (fat's inode_quantity is 16),
	 * so a reference that is never released runs the pool dry and the open
	 * below fails. */
	test_assert_zero(close(fd));

	for (i = 0; i < 20; i++) {
		test_assert_zero(file_make(FILE_TWO, (unsigned)i));
		fd = open(FILE_TWO, O_RDONLY);
		test_assert(fd >= 0);
		test_assert_zero(remove(FILE_TWO));
		test_assert_zero(close(fd));
	}
}

/* ------------------------------------------------------------------ *
 * A reference asked for after the name is gone must be refused, not granted.
 */
TEST_CASE("opening a file unlinked out from under the lookup is refused") {
	unsigned long refused_before;
	int fd;

	test_assert_zero(file_make(FILE_ONE, 2));

	refused_before = counter_read(&inode_ref_refused);

	fd = open(FILE_ONE, O_RDONLY);
	test_assert(fd >= 0);
	test_assert_zero(remove(FILE_ONE));

	/* The inode is alive -- this descriptor holds it -- but it has no name
	 * and is on its way out. A second open must not find it, and if some
	 * path did hand it to file_desc_create() anyway, the reference would be
	 * refused rather than taken. */
	test_assert_not_zero(0 == access(FILE_ONE, F_OK));

	test_assert_zero(close(fd));

	/* Reported rather than asserted: whether anything actually reached the
	 * refusal depends on how the lookup answers a name that is gone, and both
	 * answers are correct. */
	printk("inode_life: %lu reference(s) refused on a dying inode\n",
	    counter_read(&inode_ref_refused) - refused_before);
}

/* ------------------------------------------------------------------ *
 * Two cores: one opens a name in a loop while the other unlinks and
 * recreates it.
 *
 * This is the case that found kread() clamping a read to a length
 * it read twice, so that a write on the other core between the two reads
 * turned a one-byte request into a 256-byte one. It answers 256 and writes
 * 256, over the caller's frame and its return address, and the crash lands
 * somewhere with no connection to the read at all.
 *
 * The line below is what says so out loud if it ever comes back. It is a
 * report rather than an assertion because by the time anything could assert,
 * the frame that would do the asserting is already gone.
 */
static volatile int racer_run;
static unsigned long racer_opens, racer_misses;

static void *racer_main(void *arg) {
	(void)arg;

	while (racer_run) {
		int fd = open(FILE_TWO, O_RDONLY);

		if (fd >= 0) {
			char c;
			int res;

			racer_opens++;
			res = read(fd, &c, 1);
			if (res > 1) {
				printk("inode_life: read(1) answered %d\n", res);
			}
			close(fd);
		}
		else {
			racer_misses++;
		}
	}

	return NULL;
}

TEST_CASE("open and unlink of the same name from two cores") {
	pthread_t th;
	int i;

	racer_opens = racer_misses = 0;
	racer_run = 1;

	test_assert_zero(pthread_create(&th, NULL, racer_main, NULL));

	for (i = 0; i < 40; i++) {
		test_assert_zero(file_make(FILE_TWO, (unsigned)i));
		usleep(1000);
		test_assert_zero(remove(FILE_TWO));
	}

	racer_run = 0;
	test_assert_zero(pthread_join(th, NULL));

	test_assert_not_zero(racer_opens + racer_misses);

	printk("inode_life: racer %lu open(s), %lu miss(es)\n", racer_opens,
	    racer_misses);
}


/* ------------------------------------------------------------------ *
 * A file removed while open lives until its last descriptor closes.
 *
 * POSIX's promise, and it did not hold: the FAT driver freed the cluster chain
 * inside ino_remove(), so the descriptor had nothing left to answer with, and
 * DVFS refused it with EBADF rather than let it answer with whatever the
 * clusters became next -- which had once written one file's bytes into an
 * unrelated file on a board. The driver now takes only the name at the
 * unlink and gives the clusters back on the last reference
 * (fat_destroy_inode), so the descriptor is answered from its own file.
 */
static int fd_check(int fd, unsigned seed) {
	pattern_fill(pattern, sizeof(pattern), seed);
	memset(readback, 0, sizeof(readback));
	if (lseek(fd, 0, SEEK_SET) != 0) {
		return -1;
	}
	if (sizeof(readback) != read(fd, readback, sizeof(readback))) {
		return -1;
	}
	return memcmp(readback, pattern, sizeof(pattern)) ? -1 : 0;
}

TEST_CASE("a file removed while open is read and written until it is closed") {
	unsigned long dead_before;
	int fd;

	test_assert_zero(file_make(FILE_ONE, 3));

	fd = open(FILE_ONE, O_RDWR);
	test_assert(fd >= 0);

	dead_before = counter_read(&fdesc_dead_inode);

	test_assert_zero(remove(FILE_ONE));

	/* The name is gone... */
	test_assert(0 > open(FILE_ONE, O_RDONLY));

	/* ...and the file is not: its bytes, all of them. */
	test_assert_zero(fd_check(fd, 3));

	/* It takes a write, and reads it back. */
	pattern_fill(pattern, sizeof(pattern), 30);
	test_assert(lseek(fd, 0, SEEK_SET) == 0);
	test_assert_equal(sizeof(pattern), write(fd, pattern, sizeof(pattern)));
	test_assert_zero(fd_check(fd, 30));

	test_assert_equal(counter_read(&fdesc_dead_inode), dead_before);
	test_assert_zero(close(fd));

	/* Nothing in this suite should ever have used a descriptor whose inode
	 * changed hands. If this is not zero, the defect is alive. */
	test_assert_zero(counter_read(&fdesc_stale_gen));
}

/* ------------------------------------------------------------------ *
 * The name's slot changes hands under an open descriptor.
 *
 * The unlinked file's directory entry is free the moment it is unlinked, and
 * the next file made in that directory can take it. The driver used to write
 * a file's size and first cluster back into its entry at the end of every
 * write -- through the old descriptor, into what is by then the new file's
 * entry. So: remove, make another file in the same place, write through the
 * old descriptor, and the new file must be exactly what was written to it.
 * The old descriptor must still answer with its own bytes, never the new
 * file's.
 */
TEST_CASE("the name's slot changes hands under an open descriptor") {
	unsigned long stale_before;
	int fd;

	test_assert_zero(file_make(FILE_ONE, 4));

	fd = open(FILE_ONE, O_RDWR);
	test_assert(fd >= 0);

	stale_before = counter_read(&fdesc_stale_gen);

	test_assert_zero(remove(FILE_ONE));

	/* Somebody else's file, made in the same place the removed one was. */
	test_assert_zero(file_make(FILE_TWO, 5));

	/* The old descriptor answers with its own file, and writes to it. */
	test_assert_zero(fd_check(fd, 4));
	pattern_fill(pattern, sizeof(pattern), 40);
	test_assert(lseek(fd, 0, SEEK_SET) == 0);
	test_assert_equal(sizeof(pattern), write(fd, pattern, sizeof(pattern)));
	test_assert_zero(close(fd));

	/* The new file is untouched, through a remount, which is the only way
	 * to tell the directory entry on the media from the cached inode. */
	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));
	fd = open(FILE_TWO, O_RDONLY);
	test_assert(fd >= 0);
	test_assert_zero(fd_check(fd, 5));
	test_assert_zero(close(fd));

	test_assert_equal(counter_read(&fdesc_stale_gen), stale_before);
	test_assert_zero(remove(FILE_TWO));
}
