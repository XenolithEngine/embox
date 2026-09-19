/**
 * @file
 * @brief DVFS soak: the dvfs_smp shapes, all at once, for as long as asked.
 *
 * Not run at boot. `test -t dvfs_hammer` runs it; the rounds option says for
 * how long, and the dir option where: empty is a ramdisk of its own mounted at
 * /tmp_hammer, anything else a directory made under that path on a volume
 * that is already mounted -- on the Pi 4, the SD card's second partition, so
 * the same races meet a real controller and a card that answers slowly.
 *
 * Every worker does a different thing to the same directory at the same
 * time: creators with O_EXCL, a remover, a renamer, readers of whatever is
 * there, a lister. What is checked is what may never happen whatever the
 * interleaving: a read that returns another file's bytes, a name nobody
 * touches listed other than once, a create that two threads both won, a
 * descriptor answered after its file changed hands, and a volume that is
 * still held when everyone has gone. (A name that is removed and made again
 * during a listing may be listed twice -- POSIX leaves that open -- so the
 * listing is judged by the names nobody touches.)
 *
 * @date 19.09.26
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drivers/block_dev.h>
#include <drivers/block_dev/ramdisk/ramdisk.h>
#include <embox/test.h>
#include <framework/mod/options.h>
#include <fs/dvfs.h>
#include <fs/file_desc.h>
#include <fs/fsop.h>
#include <kernel/printk.h>
#include <util/atomic_rmw.h>
#include <util/err.h>

#define ROUNDS   OPTION_GET(NUMBER, rounds)
#define DIR_OPT  OPTION_STRING_GET(dir)

EMBOX_TEST_SUITE_NOAUTO("dvfs: every race at once, for a while");

TEST_SETUP_SUITE(setup_suite);
TEST_TEARDOWN_SUITE(teardown_suite);

#define FS_NAME    "vfat"
#define FS_DEV     "/dev/ramdisk_hammer"
#define FS_BYTES   (4 * 1024 * 1024)
#define FS_DIR     "/tmp_hammer"

#define NAMES      16
#define STABLE     8
#define DATA_SZ    96
#define WORKERS    6

extern unsigned fat_unlocked_scratch __attribute__((weak));

static char work_dir[96];
static int own_volume;

static volatile int run;
static unsigned long st_creates, st_create_dups, st_reads, st_wrong,
                     st_badf, st_renames, st_lists, st_list_dups, st_removes;

static unsigned long load(unsigned long *p) {
	return atomic_rmw_load(p, __ATOMIC_RELAXED);
}

static void bump(unsigned long *p) {
	atomic_rmw_add_fetch(p, 1, __ATOMIC_RELAXED);
}

/* The content of every file is its name's number, repeated: a reader can
 * tell from the bytes alone whether it got the file it opened. */
static void content(char *buf, int n) {
	int i;

	for (i = 0; i < DATA_SZ; i++) {
		buf[i] = (char) ('A' + n);
	}
}

static void name_of(char *path, size_t sz, const char *prefix, int n) {
	snprintf(path, sz, "%s/%s%02d", work_dir, prefix, n);
}

/* Creation claims: a name that two creators both won is the defect. */
static unsigned long claims[NAMES];

static void *creator(void *arg) {
	unsigned seed = (unsigned)(intptr_t) arg * 7919;
	char path[128], buf[DATA_SZ];
	int n, fd;

	while (run) {
		seed = seed * 1103515245 + 12345;
		n = (seed >> 16) % NAMES;
		name_of(path, sizeof(path), "h", n);

		fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd < 0) {
			continue;
		}
		if (atomic_rmw_add_fetch(&claims[n], 1, __ATOMIC_ACQ_REL) != 1) {
			bump(&st_create_dups);
		}
		content(buf, n);
		(void) write(fd, buf, sizeof(buf));
		close(fd);
		bump(&st_creates);
	}
	return NULL;
}

static void *remover(void *arg) {
	unsigned seed = 4242;
	char path[128];
	int n;

	(void) arg;

	while (run) {
		seed = seed * 1103515245 + 12345;
		n = (seed >> 16) % NAMES;
		name_of(path, sizeof(path), "h", n);

		/* Release the claim before the name goes, so a creator that wins
		 * the name next finds it free. */
		if (atomic_rmw_load(&claims[n], __ATOMIC_ACQUIRE) == 1) {
			atomic_rmw_sub_fetch(&claims[n], 1, __ATOMIC_ACQ_REL);
			if (0 == remove(path)) {
				bump(&st_removes);
			} else {
				atomic_rmw_add_fetch(&claims[n], 1, __ATOMIC_ACQ_REL);
			}
		}
		usleep(200);
	}
	return NULL;
}

static void *reader(void *arg) {
	unsigned seed = (unsigned)(intptr_t) arg * 31337;
	char path[128], buf[DATA_SZ], want[DATA_SZ];
	int n, fd, len;

	while (run) {
		seed = seed * 1103515245 + 12345;
		n = (seed >> 16) % NAMES;
		name_of(path, sizeof(path), "h", n);

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			continue;
		}
		len = read(fd, buf, sizeof(buf));
		if (len < 0 && errno == EBADF) {
			bump(&st_badf);
		} else if (len > 0) {
			content(want, n);
			if (len > DATA_SZ || memcmp(buf, want, len)) {
				bump(&st_wrong);
			}
			bump(&st_reads);
		}
		close(fd);
	}
	return NULL;
}

static void *renamer(void *arg) {
	char a[128], b[128], buf[DATA_SZ];
	int fd;

	(void) arg;

	/* Its own pair, so the content check stays exact: r00 <-> r01 hold
	 * the same bytes whichever name they are under. */
	name_of(a, sizeof(a), "r", 0);
	name_of(b, sizeof(b), "r", 1);
	fd = open(a, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd >= 0) {
		content(buf, 17);
		(void) write(fd, buf, sizeof(buf));
		close(fd);
	}

	while (run) {
		if (0 == rename(a, b) && 0 == rename(b, a)) {
			bump(&st_renames);
		}
	}
	return NULL;
}

static void *lister(void *arg) {
	struct dirent *de;
	DIR *d;
	int seen[STABLE];
	int n, k;

	(void) arg;

	while (run) {
		memset(seen, 0, sizeof(seen));
		d = opendir(work_dir);
		if (d == NULL) {
			continue;
		}
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == 's' && 1 == sscanf(de->d_name + 1, "%d", &n)
			    && n >= 0 && n < STABLE) {
				seen[n]++;
			}
		}
		closedir(d);
		for (k = 0; k < STABLE; k++) {
			if (seen[k] != 1) {
				bump(&st_list_dups);
				break;
			}
		}
		bump(&st_lists);
	}
	return NULL;
}

/* The names nobody touches, which every listing must show once each. */
static int stable_make(void) {
	char path[128], buf[DATA_SZ];
	int n, fd;

	for (n = 0; n < STABLE; n++) {
		name_of(path, sizeof(path), "s", n);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) {
			return -errno;
		}
		content(buf, n);
		(void) write(fd, buf, sizeof(buf));
		close(fd);
	}
	return 0;
}

static int setup_suite(void) {
	int res;

	if (DIR_OPT[0] != '\0') {
		own_volume = 0;
		snprintf(work_dir, sizeof(work_dir), "%s", DIR_OPT);
		if (mkdir(work_dir, 0777) && errno != EEXIST) {
			return -errno;
		}
		return stable_make();
	}

	own_volume = 1;
	snprintf(work_dir, sizeof(work_dir), "%s", FS_DIR);

	res = ptr2err(ramdisk_create(FS_DEV, FS_BYTES));
	if (res != 0) {
		return res;
	}
	if (0 != (res = format(FS_DEV, FS_NAME))) {
		return res;
	}

	if (0 != (res = mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL))) {
		return res;
	}

	return stable_make();
}

static int teardown_suite(void) {
	char path[128];
	int n;

	if (!own_volume) {
		/* Leave the card as it was found, less one empty directory. */
		for (n = 0; n < NAMES; n++) {
			name_of(path, sizeof(path), "h", n);
			(void) remove(path);
		}
		for (n = 0; n < STABLE; n++) {
			name_of(path, sizeof(path), "s", n);
			(void) remove(path);
		}
		name_of(path, sizeof(path), "r", 0);
		(void) remove(path);
		name_of(path, sizeof(path), "r", 1);
		(void) remove(path);
		(void) rmdir(work_dir);
		return 0;
	}

	umount(FS_DIR);
	return ramdisk_delete(FS_DEV);
}

TEST_CASE("creators, a remover, a renamer, readers and a lister in one directory") {
	static void *(*const roles[WORKERS])(void *) = {
		creator, creator, remover, reader, renamer, lister,
	};
	unsigned long contended_before = load(&vfs_tree_contended);
	unsigned long reclaimed_before = load(&dvfs_reclaimed);
	unsigned long stale_before = load(&fdesc_stale_gen);
	pthread_t th[WORKERS];
	int round, i;

	memset(claims, 0, sizeof(claims));
	st_creates = st_create_dups = st_reads = st_wrong = st_badf = 0;
	st_renames = st_lists = st_list_dups = st_removes = 0;

	for (round = 0; round < ROUNDS; round++) {
		run = 1;
		for (i = 0; i < WORKERS; i++) {
			test_assert_zero(pthread_create(&th[i], NULL, roles[i],
			    (void *)(intptr_t)(i + 1)));
		}

		usleep(1000 * 1000);
		run = 0;

		for (i = 0; i < WORKERS; i++) {
			test_assert_zero(pthread_join(th[i], NULL));
		}

		/* Never: two winners of one name, another file's bytes, an
		 * untouched name listed other than once, a descriptor answered after
		 * its inode moved on. */
		test_assert_zero(load(&st_create_dups));
		test_assert_zero(load(&st_wrong));
		test_assert_zero(load(&st_list_dups));
		test_assert_equal(load(&fdesc_stale_gen), stale_before);
	}

	/* The workers must have done something, or this proved nothing. */
	test_assert_not_zero(load(&st_creates));
	test_assert_not_zero(load(&st_reads));
	test_assert_not_zero(load(&st_lists));

	if (own_volume) {
		/* Everyone has let go: the volume comes off. */
		test_assert_zero(umount(FS_DIR));
		test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));
	}

	test_assert_zero(load(&dvfs_unlocked_tree));
	if (&fat_unlocked_scratch) {
		test_assert_zero(fat_unlocked_scratch);
	}

	printk("dvfs_hammer: %d round(s) in %s: %lu create(s), %lu remove(s), "
	       "%lu rename pair(s), %lu read(s), %lu list(s)\n",
	    ROUNDS, work_dir, load(&st_creates), load(&st_removes),
	    load(&st_renames), load(&st_reads), load(&st_lists));
	printk("dvfs_hammer: %lu read(s) refused as removed, tree contended %lu "
	       "time(s), %lu dentries reclaimed, %lu unlocked\n",
	    load(&st_badf), load(&vfs_tree_contended) - contended_before,
	    load(&dvfs_reclaimed) - reclaimed_before, load(&dvfs_unlocked_tree));
	printk("dvfs_hammer: passed\n");
}
