/**
 * @file
 * @brief What a FAT driver has to get right, on a ramdisk instead of a card.
 *
 * Every case here is a defect that was found on real hardware, in a board
 * suite that needs an SD controller, a provisioned card and a host to run
 * fsck afterwards -- so none of it could ever run in CI. The behaviour it was
 * testing is the driver's, not the card's, and that part travels: a ramdisk
 * formatted vfat exercises the same code.
 *
 * What each case is, and what it caught:
 *
 *   read sizes          reads longer than one sector returned short or wrong
 *   read after write    a read in the same session as the write that preceded
 *                       it saw stale bytes -- the driver's cursor and its
 *                       one-sector buffer disagreeing
 *   rewrite             O_TRUNC to the same size, smaller, and larger; the
 *                       smaller case leaves a chain to shorten and the larger
 *                       one a chain to grow
 *   many files          sixteen files in a row, then read back: this is what
 *                       showed the allocator handing out a cluster twice
 *   interleaved         two descriptors written in turn, which is the shape
 *                       an update has when it logs while it downloads
 *   remount             everything written survives umount and mount, which
 *                       is the only way to tell a correct file from a correct
 *                       cache
 *   unlink              deleting a multi-cluster file used to never return,
 *                       and the space it freed used to not come back
 *
 * The pattern written is regenerable and compared byte for byte, because a
 * size and a return code are not evidence: the defect that started all of this
 * reported success for a write that stored 5.4 MiB of the 8 requested.
 */

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ctype.h>
#include <dirent.h>

#include <errno.h>

#include <drivers/block_dev.h>
#include <fs/fat_pool_use.h>
#include <drivers/block_dev/ramdisk/ramdisk.h>
#include <embox/test.h>
#include <fs/fsop.h>
#include <kernel/printk.h>
#include <kernel/thread.h>
#include <util/err.h>

EMBOX_TEST_SUITE("FAT driver operations");

TEST_SETUP_SUITE(setup_suite);
TEST_TEARDOWN_SUITE(teardown_suite);

#define FS_NAME   "vfat"
#define FS_DEV    "/dev/ramdisk_fat"
/* Big enough that a file spans many clusters and that sixteen of them fit:
 * every defect below needed more than one cluster to show. */
/* Bytes, not pages: a page count times PAGE_SIZE() means a different
 * volume on every board. */
#define FS_BYTES   (8 * 1024 * 1024)
#define FS_DIR    "/tmp_fat"

#define FILE_A    FS_DIR "/a.bin"
#define FILE_B    FS_DIR "/b.bin"

/* One file's worth of bytes, and the buffer everything is read back into.
 * Static: a test suite runs in kernel context and a stack is not the place. */
#define DATA_SZ   (32 * 1024)
static char pattern[DATA_SZ];
static char readback[DATA_SZ];

/* A regenerable pattern: byte i of the file is a function of i alone, so a
 * mismatch names the offset it happened at rather than "the file differs". */
static void pattern_fill(char *buf, size_t len, unsigned seed) {
	size_t i;

	for (i = 0; i < len; i++) {
		buf[i] = (char)((i * 31u + (i >> 8) * 7u + seed) & 0xff);
	}
}

static int pattern_first_diff(const char *got, const char *want, size_t len) {
	size_t i;

	for (i = 0; i < len; i++) {
		if (got[i] != want[i]) {
			return (int)i;
		}
	}
	return -1;
}

/* Write `len` bytes of the pattern into `path`, one write() call. */
static int write_file(const char *path, size_t len, unsigned seed) {
	int fd;
	ssize_t n;

	pattern_fill(pattern, len, seed);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		/* Said out loud: which call failed and why is the whole difference
		 * between a defect and a volume that is simply full. */
		printk("fat_ops: open(%s) failed, errno %d\n", path, errno);
		return -1;
	}
	n = write(fd, pattern, len);
	if (n != (ssize_t)len) {
		printk("fat_ops: write(%s, %u) wrote %d, errno %d\n", path,
		    (unsigned)len, (int)n, errno);
	}
	close(fd);

	return (n == (ssize_t)len) ? 0 : -1;
}

/* Read `path` back in `chunk`-sized pieces and compare. Returns the offset of
 * the first wrong byte, -1 if it is all correct, or -2 if the file ended
 * early -- which is a different failure and used to be the common one. */
static int check_file(const char *path, size_t len, size_t chunk) {
	size_t got = 0;
	int fd;

	memset(readback, 0, len);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -2;
	}
	while (got < len) {
		size_t want = (len - got < chunk) ? (len - got) : chunk;
		ssize_t n = read(fd, readback + got, want);

		if (n <= 0) {
			break;
		}
		got += (size_t)n;
	}
	close(fd);

	if (got != len) {
		return -2;
	}
	return pattern_first_diff(readback, pattern, len);
}

/* ------------------------------------------------------------------ *
 * THE VOLUME, READ FROM THE DEVICE
 *
 * Everything below this line asks the media rather than the driver. A driver
 * that answers open() from its own tree is consistent with itself whatever is
 * on the disk; the only way to tell a correct volume from a correct cache is
 * to read the bytes. */

struct volume {
	unsigned bytepersec, secperclus, reserved, numfats, rootentries;
	unsigned secperfat, numsecs, rootsecs, dataarea, clusters, bits;
	unsigned rootstart;      /* first root SECTOR (FAT12/16) */
	unsigned rootclus;       /* root CLUSTER (FAT32) */
	const uint8_t *sysid;
	uint8_t sec[512];
};

static struct block_dev *fat_bdev(void) {
	return block_dev_find("ramdisk_fat");
}

/* Fill a whole block device with one byte. See setup_suite for why. */
static void poison_media(const char *dev, size_t bytes, uint8_t fill) {
	struct block_dev *bdev = block_dev_find(dev + sizeof("/dev/") - 1);
	uint8_t buf[512];
	size_t off;

	if (bdev == NULL) {
		printk("fat_ops: %s not found, media left as it was\n", dev);
		return;
	}
	memset(buf, fill, sizeof(buf));
	for (off = 0; off + sizeof(buf) <= bytes; off += sizeof(buf)) {
		if ((int)sizeof(buf) != block_dev_write(bdev, (char *)buf, sizeof(buf),
		        off / bdev->block_size)) {
			printk("fat_ops: could not poison %s at offset %u\n", dev,
			    (unsigned)off);
			return;
		}
	}
}

/* Parse the boot sector the way any reader does. Returns 0, or -1 if the
 * device cannot be read. */
static int volume_read(struct block_dev *bdev, struct volume *v) {
	uint32_t fatsize32;

	if (bdev == NULL) {
		printk("fat_ops: volume_read: no device\n");
		return -1;
	}
	memset(v, 0, sizeof(*v));
	if ((int)sizeof(v->sec)
	    != block_dev_read(bdev, (char *)v->sec, sizeof(v->sec), 0)) {
		printk("fat_ops: volume_read: sector 0 of %s could not be read\n",
		    block_dev_name(bdev));
		return -1;
	}

	v->bytepersec  = v->sec[11] | (v->sec[12] << 8);
	v->secperclus  = v->sec[13];
	v->reserved    = v->sec[14] | (v->sec[15] << 8);
	v->numfats     = v->sec[16];
	v->rootentries = v->sec[17] | (v->sec[18] << 8);
	v->numsecs     = v->sec[19] | (v->sec[20] << 8);
	v->secperfat   = v->sec[22] | (v->sec[23] << 8);
	if (v->numsecs == 0) {
		v->numsecs = v->sec[32] | (v->sec[33] << 8)
		             | (v->sec[34] << 16) | (v->sec[35] << 24);
	}
	fatsize32 = v->sec[36] | (v->sec[37] << 8)
	            | (v->sec[38] << 16) | (v->sec[39] << 24);
	v->rootclus = v->sec[44] | (v->sec[45] << 8)
	              | (v->sec[46] << 16) | (v->sec[47] << 24);

	if (v->bytepersec == 0 || v->secperclus == 0) {
		printk("fat_ops: volume_read: %s has no boot sector -- first bytes "
		       "%02x %02x %02x %02x, %u B/sec, %u sec/clus\n",
		    block_dev_name(bdev), v->sec[0], v->sec[1], v->sec[2], v->sec[3],
		    v->bytepersec, v->secperclus);
		return -1;
	}
	v->rootsecs = (v->rootentries * 32 + v->bytepersec - 1) / v->bytepersec;
	v->rootstart = v->reserved
	               + v->numfats * (v->secperfat ? v->secperfat : fatsize32);
	v->dataarea = v->rootstart + v->rootsecs;
	if (v->numsecs <= v->dataarea) {
		printk("fat_ops: volume_read: %s says %u sectors but its data starts "
		       "at %u (%u reserved, %u FAT(s) of %u, %u root sectors)\n",
		    block_dev_name(bdev), v->numsecs, v->dataarea, v->reserved,
		    v->numfats, v->secperfat, v->rootsecs);
		return -1;
	}
	v->clusters = (v->numsecs - v->dataarea) / v->secperclus;
	v->bits = (v->clusters < 4085) ? 12 : (v->clusters < 65525) ? 16 : 32;
	v->sysid = (v->bits == 32) ? &v->sec[82] : &v->sec[54];
	if (!v->secperfat) {
		v->secperfat = fatsize32;
	}
	return 0;
}

static void volume_print(const struct volume *v, const char *what) {
	printk("fat_ops: %s: %u sectors of %u, %u/clus, %u reserved, %u FAT(s) "
	       "of %u, %u root entries at sector %u, data at %u, %u clusters -> "
	       "FAT%u, system \"%.8s\"\n",
	    what, v->numsecs, v->bytepersec, v->secperclus, v->reserved,
	    v->numfats, v->secperfat, v->rootentries, v->rootstart, v->dataarea,
	    v->clusters, v->bits, (const char *)v->sysid);
}

/* What the root directory holds, read off the device, entry by entry.
 *
 * This is the one question that separates a write that never landed from a
 * walk that stopped early, and it cannot be asked through open(): the names
 * are either in these sectors or they are not. Prints a line per anomaly and
 * returns how many live 8.3 entries it counted. */
static int root_dump(struct block_dev *bdev, const struct volume *v,
    int expect) {
	uint8_t buf[512];
	unsigned s, e;
	int live = 0, deleted = 0, lfn = 0;
	int first_free = -1, after_free = 0;

	if (bdev == NULL || v->rootentries == 0) {
		return -1;
	}

	for (s = 0; s < v->rootsecs; s++) {
		if ((int)sizeof(buf) != block_dev_read(bdev, (char *)buf, sizeof(buf),
		        (v->rootstart + s) * (v->bytepersec / bdev->block_size))) {
			printk("fat_ops: root sector %u could not be read\n",
			    v->rootstart + s);
			return -1;
		}
		for (e = 0; e < sizeof(buf) / 32; e++) {
			const uint8_t *d = buf + e * 32;
			int idx = (int)(s * (sizeof(buf) / 32) + e);

			if (d[0] == 0x00) {
				if (first_free < 0) {
					first_free = idx;
				}
				continue;
			}
			if (first_free >= 0) {
				after_free++;   /* a live entry past the end-of-directory mark */
			}
			if (d[0] == 0xe5) {
				deleted++;
				continue;
			}
			if ((d[11] & 0x0f) == 0x0f) {
				lfn++;
				continue;
			}
			live++;
		}
	}

	printk("fat_ops: the root area holds %d live entries, %d deleted, %d "
	       "long-name; first free entry at %d, %d live entries AFTER it "
	       "(expected %d files)\n",
	    live, deleted, lfn, first_free, after_free, expect);
	if (after_free) {
		printk("fat_ops: entries past the first free slot are unreachable: a "
		       "walk stops at the first 0x00 entry, so those files are on the "
		       "media and not in any listing\n");
	}
	return live;
}

/* The boot sector mkfs wrote must describe the volume mkfs made.
 *
 * A FAT boot sector comes in exactly two shapes, and which one it is decides
 * where the root directory lives:
 *
 *   FAT12/16   rootentries != 0 -- the root is a fixed run of sectors right
 *              after the FATs -- and the FAT's size is in the BPB
 *   FAT32      rootentries == 0, BPB FAT size 0, the size in the FAT32 EBPB
 *              instead, and the root is an ordinary cluster chain
 *
 * Nothing on the volume says which type it is: every reader, this driver and
 * Linux alike, counts the clusters and applies the same two thresholds. So a
 * boot sector whose EBPB says one thing while its cluster count says another
 * is not a matter of taste -- it is a description of a volume that does not
 * exist, and the fields that tell a reader where to look come from the half
 * that is wrong.
 *
 * This is checked from the bytes on the device and nothing else: the driver's
 * own volinfo would only prove the driver agrees with itself. */
TEST_CASE("the boot sector describes the volume that was made") {
	struct block_dev *bdev = fat_bdev();
	struct volume v;
	uint8_t fat[512];

	test_assert_not_null(bdev);
	test_assert_zero(volume_read(bdev, &v));
	volume_print(&v, "boot sector");

	test_assert_equal(0x55, v.sec[510]);
	test_assert_equal(0xaa, v.sec[511]);
	test_assert_equal(512u, v.bytepersec);
	test_assert(v.reserved > 0);
	test_assert(v.numfats > 0);

	if (v.bits == 32) {
		test_assert_equal(0u, v.rootentries);
		/* The BPB's own FAT size must be zero on FAT32 -- that zero is how a
		 * reader is told to take the size from the FAT32 EBPB instead. */
		test_assert_equal(0u, (unsigned)(v.sec[22] | (v.sec[23] << 8)));
		test_assert(v.secperfat > 0);
		test_assert(v.rootclus >= 2);
		test_assert_zero(memcmp(v.sysid, "FAT32   ", 8));
	}
	else {
		/* Not FAT32: the root is a fixed area, so it must have a size, and
		 * the FAT's size must be where a FAT12/16 reader looks for it. */
		test_assert(v.rootentries > 0);
		test_assert(v.sec[22] != 0 || v.sec[23] != 0);
		test_assert_zero(
		    memcmp(v.sysid, v.bits == 12 ? "FAT12   " : "FAT16   ", 8));
	}

	/* Entry 0 is the media descriptor with the type's high bits set, entry 1
	 * the end-of-chain mark. mkfs writes both; a zero there means the
	 * allocator may hand out cluster 0 or 1 as if they were free. On FAT32
	 * the root's own cluster must be claimed too, or the first file written
	 * is handed the root directory. */
	test_assert_equal((int)sizeof(fat),
	    block_dev_read(bdev, (char *)fat, sizeof(fat),
	        v.reserved * (v.bytepersec / bdev->block_size)));
	test_assert_equal(fat[0], 0xf8);
	test_assert(fat[1] != 0 || fat[2] != 0);
	if (v.bits == 32) {
		test_assert(fat[8] != 0 || fat[9] != 0 || fat[10] != 0 || fat[11] != 0);
	}
}

/* A volume large enough to actually BE FAT32, formatted as one.
 *
 * Every volume this tree makes -- the 2 and 4 and 8 and 40 MiB ramdisks, the
 * board's 64 MiB card partitions -- has fewer than 65 525 clusters, so it is
 * FAT12 or FAT16 no matter what anybody asks for. That means the FAT32 half
 * of the formatter is code that nothing runs, and code nothing runs is where
 * this project has found every defect so far. 65 525 clusters of 4 sectors
 * of 512 bytes is 134 MiB; take 140 so the count cannot land on the boundary.
 *
 * Only the formatter is exercised here, not the driver's FAT32 mount path:
 * this case reads the media and never mounts. A failed assertion longjmps out
 * of the case, so the ramdisk is gone before the first one -- a case that
 * unmounted the suite's own volume and then asserted would take every case
 * after it down with it. */
TEST_CASE("a volume big enough for FAT32 is formatted as one") {
	enum { F32_BYTES = 140 * 1024 * 1024 };
	struct block_dev *bdev;
	struct volume v;
	uint8_t fat[512], root[512];
	int res, made = 0, formatted = -1;
	unsigned rootsec = 0;

	res = ptr2err(ramdisk_create("/dev/ramdisk_f32", F32_BYTES));
	if (res != 0) {
		/* Not a failure: a board with less memory than this simply cannot
		 * hold a FAT32 volume, and saying so is more use than an assertion
		 * that reads "out of memory". */
		printk("fat_ops: no %u MiB ramdisk for the FAT32 case (%d) -- skipped\n",
		    (unsigned)(F32_BYTES / (1024 * 1024)), res);
		return;
	}
	made = 1;
	memset(&v, 0, sizeof(v));
	memset(fat, 0, sizeof(fat));
	memset(root, 0, sizeof(root));

	/* The metadata end only: 8 MiB covers the reserved sector, both FATs and
	 * the first data clusters, and poisoning 140 MiB one sector at a time
	 * would be the slowest thing in the suite. */
	poison_media("/dev/ramdisk_f32", 8 * 1024 * 1024, 0xf6);

	formatted = format("/dev/ramdisk_f32", FS_NAME);
	bdev = block_dev_find("ramdisk_f32");
	if (formatted != 0 || bdev == NULL) {
		printk("fat_ops: FAT32 volume: format returned %d, device %sfound\n",
		    formatted, bdev ? "" : "NOT ");
	}
	if (formatted == 0 && bdev != NULL && volume_read(bdev, &v) == 0) {
		volume_print(&v, "the FAT32 volume");
		block_dev_read(bdev, (char *)fat, sizeof(fat),
		    v.reserved * (v.bytepersec / bdev->block_size));
		if (v.rootclus >= 2) {
			rootsec = v.dataarea + (v.rootclus - 2) * v.secperclus;
			block_dev_read(bdev, (char *)root, sizeof(root),
			    rootsec * (v.bytepersec / bdev->block_size));
		}
	}

	if (made) {
		ramdisk_delete("/dev/ramdisk_f32");
	}

	/* Nothing below here touches the device. */
	test_assert_zero(formatted);
	test_assert(v.clusters >= 65525);
	test_assert_equal(32u, v.bits);

	/* The FAT32 shape, in full: no root area, the BPB's FAT size zero so a
	 * reader takes the EBPB's, and a root cluster to start the chain at. */
	test_assert_equal(0u, v.rootentries);
	test_assert_equal(0u, (unsigned)(v.sec[22] | (v.sec[23] << 8)));
	test_assert(v.secperfat > 0);
	test_assert(v.rootclus >= 2);
	test_assert_zero(memcmp(v.sysid, "FAT32   ", 8));

	/* Entries 0 and 1 reserved, and -- the one that matters -- the root's own
	 * cluster claimed. A zero there is a free cluster, and the first file
	 * written to the volume is handed the root directory. */
	test_assert_equal(fat[0], 0xf8);
	test_assert(fat[1] != 0 || fat[2] != 0 || fat[3] != 0);
	test_assert(fat[8] != 0 || fat[9] != 0 || fat[10] != 0 || fat[11] != 0);

	/* And it has to be empty. The media was 0xf6 before mkfs, so an
	 * uncleared root cluster is a directory full of entries with plausible
	 * names that nobody wrote. */
	test_assert(rootsec != 0);
	test_assert_zero(root[0]);
	test_assert_zero(memcmp(root, root + 1, sizeof(root) - 1));
}

/* The FAT sector write-back (fat_common.c): the driver holds the last FAT
 * sector written and puts it on the card, both copies, when another FAT
 * sector is written or the operation ends. The suite's own volume is FAT12,
 * which the write-back leaves alone, so this makes a FAT32 one and mounts it
 * inside the first.
 *
 * Two files grown by turns, a megabyte each in 32 KiB writes: every write()
 * takes eight clusters, their chains interleave in the same FAT sectors and
 * run on across several. After a remount -- which forgets anything held --
 * each byte has to be where it was written, and the two FAT copies on the
 * device have to be the same, before the files are deleted and after. */
#define F32W_DEV   "/dev/ramdisk_f32w"
#define F32W_DIR   FS_DIR "/f32"
#define F32W_CHUNKS 32

static int f32w_check(const char *path, unsigned seed) {
	int fd, i, bad = -1;

	pattern_fill(pattern, DATA_SZ, seed);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -2;
	}
	for (i = 0; i < F32W_CHUNKS && bad < 0; i++) {
		size_t got = 0;

		while (got < DATA_SZ) {
			ssize_t n = read(fd, readback + got, DATA_SZ - got);

			if (n <= 0) {
				break;
			}
			got += (size_t)n;
		}
		if (got != DATA_SZ) {
			bad = -2 - i;
			break;
		}
		if (pattern_first_diff(readback, pattern, DATA_SZ) >= 0) {
			bad = i;
		}
	}
	close(fd);
	return bad;
}

/* The board's boot-log follower, in miniature: small appends to another file
 * on the same volume while the download truncates and rewrites its own. */
static volatile int f32w_log_stop;
static volatile int f32w_log_writes;

static void *f32w_logger(void *arg) {
	int fd = open(F32W_DIR "/log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);

	(void)arg;
	while (fd >= 0 && !f32w_log_stop) {
		if (write(fd, "a line of the boot log, as the follower writes it\n", 51)
		    == 51) {
			f32w_log_writes++;
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	return NULL;
}

/* 1 if the first `n` sectors of the two FATs are the same, 0 if not. */
static int f32w_fats_agree(const struct volume *v, unsigned n) {
	struct block_dev *bdev = block_dev_find("ramdisk_f32w");
	static uint8_t a[512], b[512];
	unsigned per = v->bytepersec / bdev->block_size, i;

	for (i = 0; i < n && i < v->secperfat; i++) {
		block_dev_read(bdev, (char *)a, sizeof(a), (v->reserved + i) * per);
		block_dev_read(bdev, (char *)b, sizeof(b),
		    (v->reserved + v->secperfat + i) * per);
		if (memcmp(a, b, sizeof(a)) != 0) {
			printk("fat_ops: FAT sector %u differs between the copies\n", i);
			return 0;
		}
	}
	return 1;
}

TEST_CASE("a FAT32 file grown a cluster at a time survives a remount, both FAT copies alike") {
	enum { F32_BYTES = 140 * 1024 * 1024 };
	struct volume v;
	int fa = -1, fb = -1, i, res, made = 0, mounted = 0;
	int ok_write = 1, bad_a, bad_b, agree_before, agree_after = 0;

	res = ptr2err(ramdisk_create(F32W_DEV, F32_BYTES));
	if (res != 0) {
		printk("fat_ops: no %u MiB ramdisk for the FAT32 write-back case "
		       "(%d) -- skipped\n", (unsigned)(F32_BYTES / (1024 * 1024)), res);
		return;
	}
	made = 1;
	poison_media(F32W_DEV, 8 * 1024 * 1024, 0xf6);
	res = format(F32W_DEV, FS_NAME);
	if (res == 0) {
		mkdir(F32W_DIR, 0777);
		res = mount(F32W_DEV, F32W_DIR, FS_NAME, 0, NULL);
		mounted = res == 0;
	}

	if (mounted) {
		fa = open(F32W_DIR "/a.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		fb = open(F32W_DIR "/b.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		for (i = 0; i < F32W_CHUNKS && fa >= 0 && fb >= 0; i++) {
			pattern_fill(pattern, DATA_SZ, 40);
			ok_write &= write(fa, pattern, DATA_SZ) == DATA_SZ;
			pattern_fill(pattern, DATA_SZ, 41);
			ok_write &= write(fb, pattern, DATA_SZ) == DATA_SZ;
		}
		if (fa >= 0) {
			close(fa);
		}
		if (fb >= 0) {
			close(fb);
		}
		umount(F32W_DIR);
		mounted = 0 == mount(F32W_DEV, F32W_DIR, FS_NAME, 0, NULL);
	}

	/* What the update's download does to its staging file: O_TRUNC over a
	 * file that has a chain, then the new content in pieces the size of a
	 * TCP segment. With the FAT sector held back, the chain is freed and a
	 * new one allocated inside the same few operations. */
	if (mounted) {
		struct thread *logger;
		int fd;

		f32w_log_stop = 0;
		f32w_log_writes = 0;
		logger = thread_create(0, f32w_logger, NULL);
		fd = open(F32W_DIR "/a.bin", O_WRONLY | O_TRUNC);

		printk("fat_ops: FAT32 write-back: O_TRUNC over a chain: open %d\n", fd);
		for (i = 0; i < F32W_CHUNKS && fd >= 0; i++) {
			size_t off;

			pattern_fill(pattern, DATA_SZ, 40);
			for (off = 0; off < DATA_SZ; off += 1460) {
				size_t n = DATA_SZ - off < 1460 ? DATA_SZ - off : 1460;

				ok_write &= write(fd, pattern + off, n) == (ssize_t)n;
			}
		}
		if (fd >= 0) {
			close(fd);
		}
		f32w_log_stop = 1;
		if (!ptr2err(logger)) {
			thread_join(logger, NULL);
		}
		printk("fat_ops: FAT32 write-back: ... rewritten %s, %d log write(s) "
		       "alongside\n", ok_write ? "whole" : "SHORT", f32w_log_writes);
		umount(F32W_DIR);
		mounted = 0 == mount(F32W_DEV, F32W_DIR, FS_NAME, 0, NULL);
	}

	bad_a = mounted ? f32w_check(F32W_DIR "/a.bin", 40) : -9;
	bad_b = mounted ? f32w_check(F32W_DIR "/b.bin", 41) : -9;
	memset(&v, 0, sizeof(v));
	volume_read(block_dev_find("ramdisk_f32w"), &v);
	agree_before = v.secperfat ? f32w_fats_agree(&v, 16) : 0;

	if (mounted) {
		remove(F32W_DIR "/a.bin");
		remove(F32W_DIR "/b.bin");
		umount(F32W_DIR);
		mounted = 0;
		agree_after = f32w_fats_agree(&v, 16);
	}
	rmdir(F32W_DIR);
	if (made) {
		ramdisk_delete(F32W_DEV);
	}

	printk("fat_ops: FAT32 write-back: %u-bit volume, 2 x %u KiB written %s, "
	       "read back after remount %s/%s, FATs alike %s/%s\n",
	    v.bits, (unsigned)(F32W_CHUNKS * DATA_SZ / 1024),
	    ok_write ? "whole" : "SHORT", bad_a == -1 ? "ok" : "WRONG",
	    bad_b == -1 ? "ok" : "WRONG", agree_before ? "yes" : "NO",
	    agree_after ? "yes" : "NO");

	/* Nothing below here touches the device. */
	test_assert_zero(res);
	test_assert_equal(32u, v.bits);
	test_assert_true(fa >= 0 && fb >= 0);
	test_assert_true(ok_write);
	test_assert_equal(-1, bad_a);
	test_assert_equal(-1, bad_b);
	test_assert_true(agree_before);
	test_assert_true(agree_after);
}

/* ------------------------------------------------------------------ *
 * What an EL0 program was missing (USR-11 in docs/EMBOX-REMAINING-WORK.md):
 * the dispatcher worked around every one of these, and the workarounds were
 * knowledge about the kernel's gaps. */

TEST_CASE("readdir says what each entry is, and its number is stat's") {
	struct stat st;
	struct dirent *e;
	DIR *d;
	int file = 0, dir = 0, same_ino = 0;

	test_assert_zero(write_file(FILE_A, 1024, 50));
	mkdir(FS_DIR "/sub", 0777);
	test_assert_zero(stat(FILE_A, &st));

	test_assert_not_null(d = opendir(FS_DIR));
	while ((e = readdir(d)) != NULL) {
		if (0 == strcmp(e->d_name, "a.bin")) {
			file = e->d_type == DT_REG;
			same_ino = e->d_ino != 0 && e->d_ino == st.st_ino;
		}
		if (0 == strcmp(e->d_name, "sub")) {
			dir = e->d_type == DT_DIR;
		}
	}
	closedir(d);
	rmdir(FS_DIR "/sub");
	remove(FILE_A);

	test_assert_true(file);
	test_assert_true(dir);
	test_assert_true(same_ino);
}

static int count_entries(DIR *d) {
	int n = 0;

	while (readdir(d)) {
		n++;
	}
	return n;
}

TEST_CASE("rewinddir starts the directory over, and seekdir goes back to telldir") {
	char second[NAME_MAX];
	struct dirent *e;
	DIR *d;
	long at;
	int first_pass, second_pass;

	test_assert_zero(write_file(FILE_A, 100, 51));
	test_assert_zero(write_file(FILE_B, 100, 52));

	test_assert_not_null(d = opendir(FS_DIR));
	first_pass = count_entries(d);
	/* It was a printk: the second pass read nothing. */
	rewinddir(d);
	second_pass = count_entries(d);

	rewinddir(d);
	readdir(d);
	at = telldir(d);
	e = readdir(d);
	strncpy(second, e ? e->d_name : "", sizeof(second) - 1);
	second[sizeof(second) - 1] = '\0';
	count_entries(d);
	seekdir(d, at);
	e = readdir(d);
	closedir(d);

	remove(FILE_A);
	remove(FILE_B);

	test_assert(first_pass >= 2);
	test_assert_equal(first_pass, second_pass);
	test_assert_equal(1, at);
	test_assert_not_null(e);
	test_assert_zero(strcmp(e->d_name, second));
}

TEST_CASE("stat fills what POSIX asks: dev, ino, nlink, blksize, blocks") {
	struct stat a, b, root;

	test_assert_zero(write_file(FILE_A, 1000, 53));
	test_assert_zero(write_file(FILE_B, 1, 54));
	test_assert_zero(stat(FILE_A, &a));
	test_assert_zero(stat(FILE_B, &b));
	test_assert_zero(stat("/", &root));
	remove(FILE_A);
	remove(FILE_B);

	test_assert_equal(1, (int)a.st_nlink);
	test_assert(a.st_blksize > 0);
	test_assert_equal(2, (int)a.st_blocks);   /* 1000 bytes: two 512s */
	test_assert_equal(a.st_mtime, a.st_atime);
	test_assert(a.st_dev != 0);
	test_assert_equal(a.st_dev, b.st_dev);    /* one volume */
	test_assert(a.st_ino != 0 && b.st_ino != 0 && a.st_ino != b.st_ino);
	test_assert(root.st_dev != a.st_dev);     /* another one */
}

TEST_CASE("getcwd names a working directory deeper than the environment holds") {
	/* 8 + 4 * 16 characters: past the 59 that $PWD could carry. */
	static const char *const parts[] = {
	    "/abcdefghijklmno", "/pqrstuvwxyzabcd", "/efghijklmnopqrs",
	    "/tuvwxyzabcdefgh"};
	char path[PATH_MAX], got[PATH_MAX];
	char *res;
	size_t i;

	strcpy(path, FS_DIR);
	for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		strcat(path, parts[i]);
		mkdir(path, 0777);
	}
	test_assert(strlen(path) > 59);

	test_assert_zero(chdir(path));
	res = getcwd(got, sizeof(got));
	chdir("/");

	for (i = sizeof(parts) / sizeof(parts[0]); i > 0; i--) {
		rmdir(path);
		*strrchr(path, '/') = '\0';
	}

	test_assert_not_null(res);
	test_assert_zero(strcmp(got, FS_DIR "/abcdefghijklmno/pqrstuvwxyzabcd"
	                             "/efghijklmnopqrs/tuvwxyzabcdefgh"));
}

TEST_CASE("ftruncate grows a file with zeros and shrinks it, and a remount agrees") {
	struct stat st;
	int fd, i, zeros = 1, head = 1;
	char c;

	test_assert_zero(write_file(FILE_A, 3000, 55));

	/* Grow, past several clusters: the new bytes read as zeros. */
	fd = open(FILE_A, O_RDWR);
	test_assert(fd >= 0);
	test_assert_zero(ftruncate(fd, 20000));
	close(fd);

	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));

	test_assert_zero(stat(FILE_A, &st));
	test_assert_equal(20000, (int)st.st_size);
	fd = open(FILE_A, O_RDONLY);
	test_assert(fd >= 0);
	pattern_fill(pattern, 3000, 55);
	for (i = 0; i < 20000; i++) {
		if (read(fd, &c, 1) != 1) {
			zeros = head = 0;
			break;
		}
		if (i < 3000 && c != pattern[i]) {
			head = 0;
		}
		if (i >= 3000 && c != 0) {
			zeros = 0;
		}
	}
	close(fd);
	test_assert_true(head);
	test_assert_true(zeros);

	/* Shrink: the length holds across a remount, the head is intact. */
	fd = open(FILE_A, O_RDWR);
	test_assert(fd >= 0);
	test_assert_zero(ftruncate(fd, 100));
	close(fd);

	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));

	test_assert_zero(stat(FILE_A, &st));
	test_assert_equal(100, (int)st.st_size);
	pattern_fill(pattern, 100, 55);
	test_assert_equal(-1, check_file(FILE_A, 100, 100));

	/* And to nothing, which frees the whole chain. */
	fd = open(FILE_A, O_RDWR);
	test_assert(fd >= 0);
	test_assert_zero(ftruncate(fd, 0));
	close(fd);
	test_assert_zero(stat(FILE_A, &st));
	test_assert_zero(st.st_size);
	test_assert_zero(remove(FILE_A));
}

TEST_CASE("a read longer than one sector returns the whole thing") {
	test_assert_zero(write_file(FILE_A, DATA_SZ, 1));

	/* 512 is one sector, and the three above it are what the driver's own
	 * buffer does not cover in one go. */
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ, 512));
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ, 4096));
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ, 16384));
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ, DATA_SZ));
}

TEST_CASE("a read in the same session as the write before it is not stale") {
	int fd;
	ssize_t n;

	pattern_fill(pattern, DATA_SZ, 2);

	test_assert_true(0 <= (fd = open(FILE_A, O_RDWR | O_CREAT | O_TRUNC, 0666)));
	n = write(fd, pattern, DATA_SZ);
	test_assert_equal((ssize_t)DATA_SZ, n);

	/* No close in between: this is the case the board suite calls "read in
	 * the same session", and it is the one that was silently wrong. */
	test_assert_equal(0, lseek(fd, 0, SEEK_SET));
	memset(readback, 0, DATA_SZ);
	n = read(fd, readback, DATA_SZ);
	close(fd);

	test_assert_equal((ssize_t)DATA_SZ, n);
	test_assert_equal(-1, pattern_first_diff(readback, pattern, DATA_SZ));
}

TEST_CASE("lseek lands where it says") {
	int fd;
	char byte;

	test_assert_zero(write_file(FILE_A, DATA_SZ, 3));
	pattern_fill(pattern, DATA_SZ, 3);

	test_assert_true(0 <= (fd = open(FILE_A, O_RDONLY)));

	test_assert_equal(DATA_SZ - 1, lseek(fd, DATA_SZ - 1, SEEK_SET));
	test_assert_equal(1, read(fd, &byte, 1));
	test_assert_equal(pattern[DATA_SZ - 1], byte);

	/* Backwards across a cluster boundary, which is where a cursor that is
	 * only ever advanced gets it wrong. */
	test_assert_equal(1024, lseek(fd, 1024, SEEK_SET));
	test_assert_equal(1, read(fd, &byte, 1));
	test_assert_equal(pattern[1024], byte);

	close(fd);
}

TEST_CASE("O_TRUNC rewrites at the same size, smaller and larger") {
	struct stat st;

	test_assert_zero(write_file(FILE_A, DATA_SZ, 4));
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ, 4096));

	/* Same length: the chain is reused as it is. */
	test_assert_zero(write_file(FILE_A, DATA_SZ, 5));
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ, 4096));

	/* Shorter: the tail of the chain has to be given back. */
	test_assert_zero(write_file(FILE_A, DATA_SZ / 4, 6));
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ / 4, 4096));
	test_assert_zero(stat(FILE_A, &st));
	test_assert_equal(DATA_SZ / 4, (int)st.st_size);

	/* Longer than it has ever been: the chain has to grow. */
	test_assert_zero(write_file(FILE_A, DATA_SZ, 7));
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ, 4096));
	test_assert_zero(stat(FILE_A, &st));
	test_assert_equal(DATA_SZ, (int)st.st_size);
}

TEST_CASE("two files written in turn do not take each other's clusters") {
	int fa, fb;
	size_t off;
	const size_t step = 4096;
	const size_t len = 16 * 1024;

	test_assert_true(0 <= (fa = open(FILE_A, O_WRONLY | O_CREAT | O_TRUNC, 0666)));
	test_assert_true(0 <= (fb = open(FILE_B, O_WRONLY | O_CREAT | O_TRUNC, 0666)));

	for (off = 0; off < len; off += step) {
		pattern_fill(pattern, step, 8);
		test_assert_equal((ssize_t)step, write(fa, pattern, step));
		pattern_fill(pattern, step, 9);
		test_assert_equal((ssize_t)step, write(fb, pattern, step));
	}
	close(fa);
	close(fb);

	/* Rebuild each file's pattern the way it was written -- the same block
	 * repeated -- and compare the whole thing. */
	for (off = 0; off < len; off += step) {
		pattern_fill(pattern + off, step, 8);
	}
	test_assert_equal(-1, check_file(FILE_A, len, 4096));

	for (off = 0; off < len; off += step) {
		pattern_fill(pattern + off, step, 9);
	}
	test_assert_equal(-1, check_file(FILE_B, len, 4096));
}

TEST_CASE("what was written survives a remount") {
	test_assert_zero(write_file(FILE_A, DATA_SZ, 10));

	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));

	pattern_fill(pattern, DATA_SZ, 10);
	test_assert_equal(-1, check_file(FILE_A, DATA_SZ, 4096));
}

TEST_CASE("unlinking a multi-cluster file returns, and gives the space back") {
	int i;

	/* It has to be more than one cluster: a single-cluster file deleted fine,
	 * and that difference was the whole diagnosis. */
	test_assert_zero(write_file(FILE_A, DATA_SZ, 11));
	test_assert_zero(remove(FILE_A));
	test_assert_true(0 > open(FILE_A, O_RDONLY));

	/* And the clusters come back: writing the same file again, repeatedly,
	 * must not run the volume out of space. If the free count is a lie or the
	 * chain was not returned, this is where it shows. */
	for (i = 0; i < 8; i++) {
		test_assert_zero(write_file(FILE_A, DATA_SZ, (unsigned)(20 + i)));
		test_assert_equal(-1, check_file(FILE_A, DATA_SZ, 4096));
		test_assert_zero(remove(FILE_A));
	}
}

TEST_CASE("a directory lists what was put in it") {
	DIR *d;
	struct dirent *e;
	int seen = 0;

	test_assert_zero(write_file(FILE_A, 1024, 12));
	test_assert_zero(write_file(FILE_B, 1024, 13));

	test_assert_not_null(d = opendir(FS_DIR));
	while ((e = readdir(d)) != NULL) {
		if (0 == strcmp(e->d_name, "a.bin")) {
			seen |= 1;
		}
		if (0 == strcmp(e->d_name, "b.bin")) {
			seen |= 2;
		}
	}
	closedir(d);

	test_assert_equal(3, seen);

	test_assert_zero(remove(FILE_A));
	test_assert_zero(remove(FILE_B));
}

static int setup_suite(void) {
	int res;

	res = ptr2err(ramdisk_create(FS_DEV, FS_BYTES));
	if (res != 0) {
		printk("fat_ops: setup: ramdisk_create(%s) failed, %d\n", FS_DEV, res);
		return res;
	}

	/* Format over media that is not blank.
	 *
	 * ramdisk_create() hands out phymem_alloc() pages and does not clear
	 * them, so the volume starts as whatever that memory last held -- an
	 * earlier suite's FAT volume, usually. A card is the same: nothing
	 * erases it between one filesystem and the next. So "the media is
	 * zeroed" is never true, and a driver that needs it to be true fails on
	 * hardware while passing in an emulator whose RAM starts at zero, which
	 * is exactly how the two defects before this one hid.
	 *
	 * 0xf6 is deliberate: as a directory entry's first byte it is neither
	 * free (0x00) nor deleted (0xE5), so a stale entry is a name that a walk
	 * will believe; as a FAT entry it is an in-use cluster. Anything mkfs
	 * fails to clear shows up as a volume with files nobody wrote. */
	poison_media(FS_DEV, FS_BYTES, 0xf6);

	if (0 != (res = format(FS_DEV, FS_NAME))) {
		printk("fat_ops: setup: format(%s) failed, %d\n", FS_DEV, res);
		return res;
	}
	/* FS_DIR is not created here: the root is read-only initfs and mkdir()
	 * there fails. It comes from the module's own @InitFS entry. */
	if (0 != (res = mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL))) {
		return res;
	}

	return 0;
}

static int teardown_suite(void) {
	umount(FS_DIR);
	return ramdisk_delete(FS_DEV);
}

/* ------------------------------------------------------------------ *
 * A DIRECTORY THAT WILL NOT GO AWAY -- reported, not asserted.
 *
 * remove() of a directory whose files have all been deleted returns EPERM.
 * Both shapes do it: files that had content, and files created empty. What
 * does NOT do it is fs/filesystem_test next door, which makes two empty files
 * in a directory on a much smaller volume and removes all three -- so the
 * difference is the volume, the file count, or both, and finding out which is
 * a piece of work this suite is not.
 *
 * It is printed rather than asserted because a suite that fails aborts the
 * runlevel: the board never reaches a shell, and one open defect would cost
 * every other case in this file. The line appears on every boot until
 * somebody fixes it, which is the point.
 *
 * These cases run LAST for the same reason they cannot clean up: a directory
 * that stays holds an inode, and the driver has sixteen of them.
 * ------------------------------------------------------------------ */
#define DIR_FILES 8

static void dir_case(const char *dir, int with_content) {
	int res;
	char path[64];
	int i, fd;

	if (0 != mkdir(dir, 0777)) {
		printk("fat_ops: mkdir(%s) failed, errno %d\n", dir, errno);
		return;
	}

	for (i = 0; i < DIR_FILES; i++) {
		snprintf(path, sizeof(path), "%s/f%02d.bin", dir, i);
		if (with_content) {
			test_assert_zero(write_file(path, 4096, (unsigned)(200 + i)));
		}
		else {
			test_assert_true(0 <= (fd = creat(path, 0666)));
			close(fd);
		}
	}

	if (with_content) {
		for (i = 0; i < DIR_FILES; i++) {
			snprintf(path, sizeof(path), "%s/f%02d.bin", dir, i);
			pattern_fill(pattern, 4096, (unsigned)(200 + i));
			test_assert_equal(-1, check_file(path, 4096, 512));
		}
	}

	for (i = 0; i < DIR_FILES; i++) {
		snprintf(path, sizeof(path), "%s/f%02d.bin", dir, i);
		test_assert_zero(remove(path));
	}

	/* This used to be a report rather than an assertion, because rmdir()
	 * answered EPERM here on every boot. The cause was not the removal: a
	 * directory created by this driver had one sector of its cluster written
	 * and the rest left as the volume found them, so a scan that walked past
	 * the entries -- which eight files with long names make it do -- ran into
	 * old file data and called the directory non-empty. Now it is an
	 * assertion, and the errno is printed before it so that a failure names
	 * itself. */
	res = remove(dir);
	if (res != 0) {
		printk("fat_ops: rmdir(%s) failed with errno %d after deleting %d %s "
		       "files\n",
		    dir, errno, DIR_FILES, with_content ? "written" : "empty");
	}
	test_assert_zero(res);
}

TEST_CASE("files in a subdirectory are written, read back and deleted") {
	dir_case(FS_DIR "/many", 1);
}

TEST_CASE("empty files in a subdirectory are created and deleted") {
	dir_case(FS_DIR "/empties", 0);
}

/* ------------------------------------------------------------------ *
 * ".." of a new directory names the FIRST cluster of its parent, and 0 when
 * the parent is the root (BF-16).
 *
 * It used to name the cluster of the parent the new entry landed in: the
 * same thing while the parent fits in one cluster, a different one after
 * that, and on the root a number that means nothing. The driver never reads
 * ".." back, so nothing here noticed; fsck does. So this reads the media the
 * way fsck does -- the parent's first cluster from its entry in the root, the
 * child's ".." from the first sector of the child's own cluster. */

static int raw_sector(struct block_dev *bdev, const struct volume *v,
    unsigned sec, uint8_t *buf) {
	return (int)v->bytepersec == block_dev_read(bdev, (char *)buf,
	    v->bytepersec, sec * (v->bytepersec / bdev->block_size)) ? 0 : -1;
}

static unsigned clus_sector(const struct volume *v, unsigned clus) {
	return v->dataarea + (clus - 2) * v->secperclus;
}

/* The next cluster of a chain, or 0 at its end. A FAT12 entry is a byte and
 * a half and may straddle two sectors, so two are read. */
static unsigned clus_next(struct block_dev *bdev, const struct volume *v,
    unsigned clus) {
	uint8_t buf[1024];
	unsigned off = v->bits == 12 ? clus + clus / 2 : clus * (v->bits / 8);
	unsigned sec = v->reserved + off / v->bytepersec, next;

	if (raw_sector(bdev, v, sec, buf)
	    || raw_sector(bdev, v, sec + 1, buf + v->bytepersec)) {
		return 0;
	}
	off %= v->bytepersec;
	if (v->bits == 12) {
		next = buf[off] | (buf[off + 1] << 8);
		next = (clus & 1) ? next >> 4 : next & 0xfff;
		return next >= 0xff8 ? 0 : next;
	}
	if (v->bits == 16) {
		next = buf[off] | (buf[off + 1] << 8);
		return next >= 0xfff8 ? 0 : next;
	}
	next = (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16)
	           | ((unsigned)buf[off + 3] << 24)) & 0x0fffffff;
	return next >= 0x0ffffff8 ? 0 : next;
}

static unsigned de_clus(const uint8_t *d) {
	return (d[26] | (d[27] << 8)) | ((unsigned)(d[20] | (d[21] << 8)) << 16);
}

/* Look for the 8.3 name @name (11 bytes, blank-padded, any case) in the
 * directory starting at cluster @clus, 0 for a FAT12/16 root. Returns its
 * first cluster, or 0. */
static unsigned dir_find(struct block_dev *bdev, const struct volume *v,
    unsigned clus, const char *name) {
	uint8_t buf[512];
	unsigned s, e, i, nsec;

	if (clus == 0 && v->bits == 32) {
		clus = v->rootclus;
	}
	while (1) {
		nsec = clus ? v->secperclus : v->rootsecs;
		for (s = 0; s < nsec; s++) {
			if (raw_sector(bdev, v, (clus ? clus_sector(v, clus)
			                               : v->rootstart) + s, buf)) {
				return 0;
			}
			for (e = 0; e < v->bytepersec / 32; e++) {
				const uint8_t *d = buf + e * 32;

				if (d[0] == 0x00) {
					return 0;
				}
				if (d[0] == 0xe5 || (d[11] & 0x0f) == 0x0f) {
					continue;
				}
				for (i = 0; i < 11; i++) {
					if (toupper(d[i]) != toupper((unsigned char)name[i])) {
						break;
					}
				}
				if (i == 11) {
					return de_clus(d);
				}
			}
		}
		if (clus == 0 || (clus = clus_next(bdev, v, clus)) == 0) {
			return 0;
		}
	}
}

/* ".." of the directory at cluster @clus, or ~0 when it cannot be read. */
static unsigned dotdot_of(struct block_dev *bdev, const struct volume *v,
    unsigned clus) {
	uint8_t buf[512];

	if (raw_sector(bdev, v, clus_sector(v, clus), buf)
	    || memcmp(buf + 32, "..         ", 11)) {
		return ~0u;
	}
	return de_clus(buf + 32);
}

/* Every live 8.3 entry of the directory at @clus: how many, how many share
 * a name with an earlier one, and how many are empty files that still hold a
 * cluster. What a host fsck reports as "Duplicate directory entry" and "File
 * size is 0 bytes, cluster chain length is > 0 bytes". */
#define DD_MAX 512
static uint8_t dd_names[DD_MAX][11];

static void dir_audit(struct block_dev *bdev, const struct volume *v,
    unsigned clus, unsigned *live, unsigned *dups, unsigned *empty_clus) {
	uint8_t buf[512];
	unsigned s, e, i;

	*live = *dups = *empty_clus = 0;
	while (clus) {
		for (s = 0; s < v->secperclus; s++) {
			if (raw_sector(bdev, v, clus_sector(v, clus) + s, buf)) {
				return;
			}
			for (e = 0; e < v->bytepersec / 32; e++) {
				const uint8_t *d = buf + e * 32;
				uint32_t size;

				if (d[0] == 0x00) {
					return;
				}
				if (d[0] == 0xe5 || (d[11] & 0x0f) == 0x0f || d[0] == '.') {
					continue;
				}
				for (i = 0; i < *live && i < DD_MAX; i++) {
					if (!memcmp(dd_names[i], d, 11)) {
						printk("fat_ops: \"%.11s\" twice in one directory\n",
						    (const char *)d);
						(*dups)++;
						break;
					}
				}
				if (*live < DD_MAX) {
					memcpy(dd_names[*live], d, 11);
				}
				(*live)++;
				size = d[28] | (d[29] << 8) | (d[30] << 16)
				       | ((uint32_t)d[31] << 24);
				if (size == 0 && !(d[11] & 0x10) && de_clus(d) != 0) {
					(*empty_clus)++;
				}
			}
		}
		clus = clus_next(bdev, v, clus);
	}
}

/* Names that are 8.3 already, so the two directories are found by name on
 * the media; the files have long names sharing their first eighteen
 * characters, which is what gave them one short name between them. */
#define DD_PARENT FS_DIR "/DDPARENT"
#define DD_FILE   DD_PARENT "/fsck-dotdot-entry-%03u"

TEST_CASE("a new directory's .. names its parent's first cluster, 0 for the root") {
	struct block_dev *bdev = fat_bdev();
	struct volume v;
	unsigned pclus, sclus, n, per_clus, i, live, dups, empty_clus;
	char path[64];
	int fd;

	test_assert_zero(volume_read(bdev, &v));

	test_assert_zero(mkdir(DD_PARENT, 0777));
	/* More entries than one cluster holds, so the next one is not in the
	 * first: each name takes two long-name entries and the 8.3 one. */
	per_clus = v.secperclus * v.bytepersec / 32;
	n = per_clus / 3 + 8;
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), DD_FILE, i);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		test_assert(fd >= 0);
		close(fd);
	}
	test_assert_zero(mkdir(DD_PARENT "/ZSUB", 0777));

	pclus = dir_find(bdev, &v, 0, "DDPARENT   ");
	test_assert_not_equal(pclus, 0);
	test_assert_not_equal(clus_next(bdev, &v, pclus), 0); /* really two */
	sclus = dir_find(bdev, &v, pclus, "ZSUB       ");
	test_assert_not_equal(sclus, 0);

	printk("fat_ops: parent at cluster %u, its .. %u; child at %u, its .. %u\n",
	    pclus, dotdot_of(bdev, &v, pclus), sclus, dotdot_of(bdev, &v, sclus));
	test_assert_equal(dotdot_of(bdev, &v, pclus), 0);
	test_assert_equal(dotdot_of(bdev, &v, sclus), pclus);

	dir_audit(bdev, &v, pclus, &live, &dups, &empty_clus);
	printk("fat_ops: %u entries, %u duplicate 8.3 names, %u empty files "
	       "holding a cluster\n", live, dups, empty_clus);
	test_assert_equal(live, n + 1);
	test_assert_zero(dups);
	test_assert_zero(empty_clus);

	/* Every one found by its long name, which a duplicate short name would
	 * not stop -- but the next open of a host would. */
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), DD_FILE, i);
		test_assert_zero(unlink(path));
	}
	test_assert_zero(rmdir(DD_PARENT "/ZSUB"));
	test_assert_zero(rmdir(DD_PARENT));
}

/* ------------------------------------------------------------------ *
 * A mount that fails must leave the tree as it found it.
 *
 * kmount() allocates the superblock, walks the volume making an inode
 * per file, and only then asks the mount table to take it. Every early return
 * between those steps used to leave something behind -- a superblock, or a
 * whole subtree of inodes hanging off a root nobody can reach, which the next
 * path walk finds. The record calls it "worth its own look before anything
 * ships that mounts a volume it does not control", and an update does exactly
 * that.
 *
 * The loop is the point: a single leak is invisible, eight of them empty a
 * pool, and the mount at the end is what notices.
 */
TEST_CASE("a mount that fails leaves the tree as it was") {
	struct stat st;
	int i;

	/* The lookup fails AFTER the superblock has been allocated, which is the
	 * shape that leaks one. The pool holds 32, so forty attempts empty it if
	 * nothing is given back -- and one attempt would show nothing at all. */
	for (i = 0; i < 40; i++) {
		test_assert_not_zero(mount(FS_DEV, "/no/such/place", FS_NAME, 0, NULL));
	}

	/* The tree still resolves and the mounted volume is still there. */
	test_assert_zero(stat("/", &st));
	test_assert_zero(stat(FS_DIR, &st));
	test_assert_zero(access(FILE_A, F_OK));

	/* And this is where a leak shows: a mount needs a superblock, and forty
	 * failed ones must not have spent them. */
	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));
	test_assert_zero(access(FILE_A, F_OK));
}

/* The root directory is not a cluster chain.
 *
 * The root directory of a FAT12/16 volume is a fixed run of sectors --
 * 512 entries is 32 sectors here, where a cluster is 4 -- and it is not a
 * cluster chain. A driver that walks it by following the FAT leaves the root
 * after the first cluster and carries on reading whichever data cluster that
 * FAT entry happens to name.
 *
 * On an empty volume the data area is zeroes, so the walk stops there and
 * nothing looks wrong. Once anything has been written, the walk reads file
 * data as directory entries. That is why this case remounts: the in-memory
 * tree answers open() correctly no matter what is on the disk, so only a walk
 * of the volume itself asks the question. */
TEST_CASE("the root directory holds more than one cluster of entries") {
	enum { ROOT_FILES = 96 };
	char path[64];
	int i, found = 0, missing = -1;
	unsigned file_denied = fat_pool_use.file_denied;
	unsigned dirinfo_denied = fat_pool_use.dirinfo_denied;

	for (i = 0; i < ROOT_FILES; i++) {
		int fd;

		snprintf(path, sizeof(path), FS_DIR "/r%03d.bin", i);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) {
			printk("fat_ops: creating root file %d failed, errno %d\n", i,
			    errno);
		}
		test_assert(fd >= 0);
		test_assert_equal(write(fd, &i, sizeof(i)), (ssize_t)sizeof(i));
		test_assert_zero(close(fd));
	}

	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));

	for (i = 0; i < ROOT_FILES; i++) {
		int fd, got = -1;

		snprintf(path, sizeof(path), FS_DIR "/r%03d.bin", i);
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			if (missing < 0) {
				missing = i;
			}
			continue;
		}
		if (sizeof(got) == read(fd, &got, sizeof(got)) && got == i) {
			found++;
		}
		else if (missing < 0) {
			missing = i;
		}
		close(fd);
	}
	if (found != ROOT_FILES) {
		struct volume v;
		int j, run;

		printk("fat_ops: %d of %d root files came back after the remount, "
		       "first one lost is %d\n",
		    found, ROOT_FILES, missing);

		/* WHICH ones are gone, as runs rather than a list: "62 of 96" says
		 * nothing about whether the walk stopped at a point or lost entries
		 * scattered through the area, and those are different defects. */
		printk("fat_ops: missing:");
		for (j = 0; j < ROOT_FILES; j = run) {
			int fd;

			snprintf(path, sizeof(path), FS_DIR "/r%03d.bin", j);
			fd = open(path, O_RDONLY);
			if (fd >= 0) {
				close(fd);
				run = j + 1;
				continue;
			}
			for (run = j; run < ROOT_FILES; run++) {
				snprintf(path, sizeof(path), FS_DIR "/r%03d.bin", run);
				fd = open(path, O_RDONLY);
				if (fd >= 0) {
					close(fd);
					break;
				}
			}
			printk(" %d-%d", j, run - 1);
		}
		printk("\n");

		/* And the decisive question: are those names on the media at all?
		 * If they are, the write worked and the walk is wrong; if they are
		 * not, the write never landed. Nothing reachable through open() can
		 * tell these apart. */
		if (volume_read(fat_bdev(), &v) == 0) {
			volume_print(&v, "the volume they were written to");
			root_dump(fat_bdev(), &v, ROOT_FILES);
		}
	}

	/* A pool that ran out during the walk is a different failure from a walk
	 * that went to the wrong sectors, and from here the two look identical:
	 * entries that are on the disk are not in the tree. Telling those two
	 * apart once took a matrix of negative controls. Said out loud, so that
	 * the next one does not. */
	if (fat_pool_use.file_denied != file_denied
	    || fat_pool_use.dirinfo_denied != dirinfo_denied) {
		printk("fat_ops: the FAT pools refused %u file(s) and %u dirinfo(s) "
		       "during this case; live %u/%u and %u/%u, peak %u and %u\n",
		    fat_pool_use.file_denied - file_denied,
		    fat_pool_use.dirinfo_denied - dirinfo_denied,
		    fat_pool_use.file_live, fat_pool_use.file_max,
		    fat_pool_use.dirinfo_live, fat_pool_use.dirinfo_max,
		    fat_pool_use.file_peak, fat_pool_use.dirinfo_peak);
	}
	test_assert_equal(fat_pool_use.file_denied, file_denied);
	test_assert_equal(fat_pool_use.dirinfo_denied, dirinfo_denied);

	test_assert_equal(found, ROOT_FILES);

	for (i = 0; i < ROOT_FILES; i++) {
		snprintf(path, sizeof(path), FS_DIR "/r%03d.bin", i);
		test_assert_zero(remove(path));
	}
}

/* The live 8.3 entries of the root, off the media: how many, and whether
 * each one's first cluster and write date are ones a reader can believe. */
static int root_live_entries(int *bad_clus, int *bad_date) {
	struct block_dev *bdev = fat_bdev();
	struct volume v;
	uint8_t buf[512];
	unsigned s, e;
	int live = 0;

	*bad_clus = *bad_date = 0;
	if (bdev == NULL || volume_read(bdev, &v) != 0 || v.rootentries == 0) {
		return -1;
	}
	for (s = 0; s < v.rootsecs; s++) {
		if ((int)sizeof(buf) != block_dev_read(bdev, (char *)buf, sizeof(buf),
		        (v.rootstart + s) * (v.bytepersec / bdev->block_size))) {
			return -1;
		}
		for (e = 0; e < sizeof(buf) / 32; e++) {
			const uint8_t *d = buf + e * 32;
			unsigned clus, date, month, day;

			if (d[0] == 0x00 || d[0] == 0xe5 || (d[11] & 0x0f) == 0x0f
			    || (d[11] & 0x08)) {   /* free, deleted, long name, label */
				continue;
			}
			live++;
			clus = d[26] | (d[27] << 8);
			if (clus != 0 && (clus < 2 || clus >= v.clusters + 2)) {
				printk("fat_ops: \"%.11s\" starts at cluster %#x, outside "
				       "the volume's 2..%u\n", (const char *)d, clus,
				    v.clusters + 1);
				(*bad_clus)++;
			}
			date = d[24] | (d[25] << 8);
			month = (date >> 5) & 0xf;
			day = date & 0x1f;
			if (month < 1 || month > 12 || day < 1) {
				printk("fat_ops: \"%.11s\" is dated %u-%02u-%02u\n",
				    (const char *)d, 1980 + (date >> 9), month, day);
				(*bad_date)++;
			}
		}
	}
	return live;
}

/* A full volume has to say so. fat_create_file() wrote the name first and
 * asked for a cluster after, so on a full volume the entry went down pointing
 * at DFS_BAD_CLUS and create reported success; and the directory search
 * returned its errors through a uint32_t that no caller could see as
 * negative.
 *
 * Where it says so moved since. An empty file owns no cluster -- cluster 0,
 * size 0, as FAT has it and as Linux makes them -- so creating one needs
 * room in the directory and nothing else, and succeeds on a full volume;
 * the first byte written is what needs a cluster, and that write fails. The
 * entry must still be sane: cluster 0, not DFS_BAD_CLUS. The dates are
 * checked on the way: every entry used to be written with month 0. */
TEST_CASE("a file on a full volume takes no cluster, and its first write fails") {
	int fd, n, bad_clus, bad_date;
	ssize_t w;

	size_t total = 0;

	pattern_fill(pattern, DATA_SZ, 40);
	fd = open(FILE_A, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	test_assert(fd >= 0);
	/* Bounded by the volume: a write that goes on past it is going round a
	 * chain that has closed on itself, which is how a FAT12 entry straddling
	 * two FAT sectors was first seen -- this loop never ended. */
	do {
		w = write(fd, pattern, DATA_SZ);
		if (w > 0) {
			total += (size_t)w;
		}
	} while (w == DATA_SZ && total <= FS_BYTES);
	test_assert_zero(close(fd));
	if (total > FS_BYTES) {
		printk("fat_ops: %u bytes written to a %u-byte volume\n",
		    (unsigned)total, (unsigned)FS_BYTES);
	}
	test_assert(total <= FS_BYTES);

	fd = open(FILE_B, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	test_assert(fd >= 0);
	w = write(fd, pattern, 1);
	if (w > 0) {
		printk("fat_ops: a byte was written to a full volume\n");
	}
	test_assert(w <= 0);
	test_assert_zero(close(fd));

	n = root_live_entries(&bad_clus, &bad_date);
	test_assert_equal(2, n);
	test_assert_zero(bad_clus);
	test_assert_zero(bad_date);

	/* And the volume is whole: freed, it takes a file again, and a remount
	 * sees exactly that. */
	test_assert_zero(remove(FILE_A));
	test_assert_zero(write_file(FILE_B, DATA_SZ, 41));
	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));
	pattern_fill(pattern, DATA_SZ, 41);
	test_assert_equal(-1, check_file(FILE_B, DATA_SZ, 4096));
	test_assert(0 > open(FILE_A, O_RDONLY));
	test_assert_zero(remove(FILE_B));
}
