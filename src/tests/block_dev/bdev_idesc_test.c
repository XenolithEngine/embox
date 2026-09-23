/**
 * @file
 * @brief read() and write() on a block device opened from /dev
 *
 * What bdev_idesc_read() got wrong, each a case below: a read shorter than a
 * block leaked its bounce buffer (the free() was under an inverted test); a
 * read longer than a block but not a whole number of them copied the caller's
 * length out of a one-block buffer, past its end; a one-block read at an
 * unaligned position read the block the position was in, from its start;
 * and a partition was bounded by its disk's size, so a read that started in
 * it ran on into whatever came next. The write side handed the caller's
 * buffer to the driver whole, whatever its length and position.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <drivers/block_dev.h>
#include <drivers/block_dev/partition.h>
#include <drivers/block_dev/ramdisk/ramdisk.h>
#include <embox/test.h>
#include <fs/mbr.h>
#include <util/err.h>

EMBOX_TEST_SUITE("block device read and write through /dev");

TEST_SETUP_SUITE(setup_suite);
TEST_TEARDOWN_SUITE(teardown_suite);

/* The name must not end in a digit: the partition is then DEV "1", not "p1" */
#define DEV       "/dev/ramdisk_bdt"
#define PART      DEV "1"
#define PART_NAME "ramdisk_bdt1"   /* the block device's own name */
#define DEV_BYTES (64 * 1024)
#define BS        512

/* The partition: blocks 16..23, followed by blocks that are not its */
#define PART_START 16
#define PART_BLKS  8

static struct ramdisk *rd;
static struct block_dev *part;
static char want[DEV_BYTES];
static char got[4 * BS];

/* Byte i of the device, whatever was written through the driver */
static char pattern(size_t i) {
	return (char)((i * 13u + (i >> 9) * 5u + 1u) & 0xff);
}

static int dev_read(const char *path, off_t at, void *buf, size_t len) {
	int fd, n;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -errno;
	}
	if (lseek(fd, at, SEEK_SET) != at) {
		close(fd);
		return -EIO;
	}
	n = read(fd, buf, len);
	close(fd);

	return n;
}

static int dev_write(const char *path, off_t at, const void *buf, size_t len) {
	int fd, n;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		return -errno;
	}
	if (lseek(fd, at, SEEK_SET) != at) {
		close(fd);
		return -EIO;
	}
	n = write(fd, buf, len);
	close(fd);

	return n;
}

TEST_CASE("a read shorter than a block, at an offset, returns those bytes") {
	test_assert_equal(100, dev_read(DEV, 700, got, 100));
	test_assert_zero(memcmp(got, want + 700, 100));
}

TEST_CASE("a read across block boundaries returns all of it") {
	test_assert_equal(3 * BS + 17, dev_read(DEV, 300, got, 3 * BS + 17));
	test_assert_zero(memcmp(got, want + 300, 3 * BS + 17));
}

TEST_CASE("a one-block read at an unaligned position is that block's worth") {
	test_assert_equal(BS, dev_read(DEV, BS + 5, got, BS));
	test_assert_zero(memcmp(got, want + BS + 5, BS));
}

TEST_CASE("a write shorter than a block keeps the rest of the block") {
	static const char msg[] = "partial";

	test_assert_equal(sizeof(msg),
	    dev_write(DEV, 2 * BS + 200, msg, sizeof(msg)));
	memcpy(want + 2 * BS + 200, msg, sizeof(msg));

	test_assert_equal(BS, dev_read(DEV, 2 * BS, got, BS));
	test_assert_zero(memcmp(got, want + 2 * BS, BS));
}

TEST_CASE("a partition reads from its own start") {
	test_assert_equal(BS, dev_read(PART, 0, got, BS));
	test_assert_zero(memcmp(got, want + PART_START * BS, BS));
}

TEST_CASE("a partition ends where its size says, not where its disk does") {
	/* At the end: nothing */
	test_assert_zero(dev_read(PART, PART_BLKS * BS, got, BS));
	/* Across the end: only the partition's part of it */
	test_assert_equal(BS / 2,
	    dev_read(PART, PART_BLKS * BS - BS / 2, got, 2 * BS));
	test_assert_zero(memcmp(got, want + (PART_START + PART_BLKS) * BS - BS / 2,
	    BS / 2));
	/* And a write there leaves the next block, which is not the
	 * partition's, as it was */
	memset(got, 0x5a, 2 * BS);
	test_assert_equal(BS / 2,
	    dev_write(PART, PART_BLKS * BS - BS / 2, got, 2 * BS));
	memset(want + (PART_START + PART_BLKS) * BS - BS / 2, 0x5a, BS / 2);
	test_assert_equal(BS,
	    dev_read(DEV, (PART_START + PART_BLKS) * BS, got, BS));
	test_assert_zero(memcmp(got, want + (PART_START + PART_BLKS) * BS, BS));
}

static int setup_suite(void) {
	struct block_dev *bdev;
	struct mbr *mbr = (struct mbr *)want;
	size_t i;
	int res;

	rd = ramdisk_create(DEV, DEV_BYTES);
	if (ptr2err(rd)) {
		return ptr2err(rd);
	}
	bdev = rd->bdev;
	if (bdev->block_size != BS) {
		return -EINVAL;
	}

	for (i = 0; i < DEV_BYTES; i++) {
		want[i] = pattern(i);
	}

	/* One partition in the MBR; the rest of block 0 stays the pattern */
	memset(mbr->ptable, 0, sizeof(mbr->ptable));
	mbr->ptable[0].type = 0x83;
	mbr->ptable[0].start_0 = PART_START;
	mbr->ptable[0].size_0 = PART_BLKS;
	mbr->sig_55 = 0x55;
	mbr->sig_aa = 0xAA;

	for (i = 0; i < DEV_BYTES / BS; i++) {
		res = bdev->driver->bdo_write(bdev, want + i * BS, BS, i);
		if (res != BS) {
			return -EIO;
		}
	}

	res = create_partitions(bdev);
	if (res < 0) {
		return res;
	}
	part = block_dev_find(PART_NAME);

	return part ? 0 : -ENOENT;
}

static int teardown_suite(void) {
	if (part) {
		block_dev_destroy(part);
	}
	return ramdisk_delete(DEV);
}
