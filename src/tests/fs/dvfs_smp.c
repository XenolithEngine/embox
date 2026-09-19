/**
 * @file
 * @brief DVFS on several cores: the races the tree lock and the reference
 *        counts exist for, each one made to happen.
 *
 * Each case is bounded in iterations, not time, and short enough to run on
 * every boot. On one core the same threads interleave at the tick; the cases
 * pass there too, they just prove less. dvfs_hammer runs the same shapes for
 * as long as it is asked to.
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
#include <fs/dvfs.h>
#include <fs/file_desc.h>
#include <fs/fsop.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <util/atomic_rmw.h>
#include <util/err.h>

EMBOX_TEST_SUITE("dvfs: the tree against itself on several cores");

TEST_SETUP_SUITE(setup_suite);
TEST_TEARDOWN_SUITE(teardown_suite);

#define FS_NAME    "vfat"
#define FS_DEV     "/dev/ramdisk_ds"
#define FS_BYTES   (4 * 1024 * 1024)
#define FS_DIR     "/tmp_ds"

#define THREADS    4

static unsigned long load(unsigned long *p) {
	return atomic_rmw_load(p, __ATOMIC_RELAXED);
}

static void bump(unsigned long *p) {
	atomic_rmw_add_fetch(p, 1, __ATOMIC_RELAXED);
}

static int file_make(const char *path, const void *data, size_t len) {
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		return -errno;
	}
	if ((ssize_t) len != write(fd, data, len)) {
		close(fd);
		return -EIO;
	}
	return close(fd);
}

/* Calls FN(i) on THREADS threads and waits for all of them. */
static int run_threads(void *(*fn)(void *)) {
	pthread_t th[THREADS];
	int i, res = 0;

	for (i = 0; i < THREADS; i++) {
		if (pthread_create(&th[i], NULL, fn, (void *)(intptr_t) i)) {
			res = -1;
			th[i] = 0;
		}
	}
	for (i = 0; i < THREADS; i++) {
		if (th[i]) {
			pthread_join(th[i], NULL);
		}
	}
	return res;
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
 * One name, four creators. "Is it there?" and "make it" were two steps,
 * so two cores could both see it missing and both write a directory entry --
 * and O_EXCL was never looked at.
 */
#define CREATE_NAMES 24
#define CREATE_DIR   FS_DIR "/create"

static unsigned long create_wins[CREATE_NAMES];
static unsigned long create_other_err;

static void *create_main(void *arg) {
	char path[64];
	int i, fd;

	(void) arg;

	for (i = 0; i < CREATE_NAMES; i++) {
		snprintf(path, sizeof(path), CREATE_DIR "/n%02d", i);
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd >= 0) {
			bump(&create_wins[i]);
			close(fd);
		} else if (errno != EEXIST) {
			bump(&create_other_err);
		}
	}
	return NULL;
}

TEST_CASE("one name created by four threads at once: one wins, one entry") {
	struct dirent *de;
	DIR *d;
	int seen[CREATE_NAMES] = { 0 };
	int i, n, other = 0;

	memset(create_wins, 0, sizeof(create_wins));
	create_other_err = 0;

	test_assert_zero(mkdir(CREATE_DIR, 0777));
	test_assert_zero(run_threads(create_main));

	for (i = 0; i < CREATE_NAMES; i++) {
		test_assert_equal(1, load(&create_wins[i]));
	}
	test_assert_zero(load(&create_other_err));

	/* And the disk agrees: each name listed exactly once. */
	d = opendir(CREATE_DIR);
	test_assert_not_null(d);
	while ((de = readdir(d)) != NULL) {
		if (1 == sscanf(de->d_name, "n%d", &n) && n >= 0 && n < CREATE_NAMES) {
			seen[n]++;
		} else {
			other++;
		}
	}
	closedir(d);

	for (i = 0; i < CREATE_NAMES; i++) {
		test_assert_equal(1, seen[i]);
	}
	test_assert_zero(other);
}

/* ------------------------------------------------------------------ *
 * Open and read against unlink and recreate. A descriptor whose file went
 * answers EBADF and is counted as dead; one that ever reached ANOTHER file's
 * inode would be counted as stale, and must never be.
 */
#define UNLINK_FILE   FS_DIR "/victim"
#define UNLINK_ROUNDS 60
#define PATTERN_SZ    64

static volatile int unlink_run;
static unsigned long unlink_opens, unlink_reads_ok, unlink_badf, unlink_wrong;

static void pattern(char *p, unsigned seed) {
	int i;

	for (i = 0; i < PATTERN_SZ; i++) {
		p[i] = (char) ((i * 7 + seed) & 0xff);
	}
}

static void *unlink_reader(void *arg) {
	char got[PATTERN_SZ];
	int fd, len;

	if ((intptr_t) arg == 0) {
		/* thread 0 is the writer */
		char data[PATTERN_SZ];
		int i;

		for (i = 0; i < UNLINK_ROUNDS; i++) {
			pattern(data, 5);
			file_make(UNLINK_FILE, data, sizeof(data));
			usleep(500);
			remove(UNLINK_FILE);
		}
		unlink_run = 0;
		return NULL;
	}

	while (unlink_run) {
		fd = open(UNLINK_FILE, O_RDONLY);
		if (fd < 0) {
			continue;
		}
		bump(&unlink_opens);
		len = read(fd, got, sizeof(got));
		if (len < 0 && errno == EBADF) {
			bump(&unlink_badf);
		} else if (len == PATTERN_SZ) {
			char want[PATTERN_SZ];

			pattern(want, 5);
			if (memcmp(want, got, PATTERN_SZ)) {
				bump(&unlink_wrong);
			} else {
				bump(&unlink_reads_ok);
			}
		} else if (len > PATTERN_SZ) {
			bump(&unlink_wrong);
		}
		close(fd);
	}
	return NULL;
}

TEST_CASE("open and read against unlink: refused or right, never another file") {
	unsigned long stale_before = load(&fdesc_stale_gen);

	/* The writer's file_make() of the first round races the readers' first
	 * open; they simply miss until it is there. */
	unlink_run = 1;
	unlink_opens = unlink_reads_ok = unlink_badf = unlink_wrong = 0;

	test_assert_zero(run_threads(unlink_reader));

	test_assert_zero(load(&unlink_wrong));
	test_assert_equal(load(&fdesc_stale_gen), stale_before);

	printk("dvfs_smp: unlink race: %lu open(s), %lu read(s) right, "
	       "%lu refused as removed\n",
	    load(&unlink_opens), load(&unlink_reads_ok), load(&unlink_badf));
}

/* ------------------------------------------------------------------ *
 * Walkers on deep paths while the FAT driver's own inode pool runs dry, so
 * that lookups have to give cached dentries back to go on. The walk holds
 * what it stands on; before that, the allocation for the next step could
 * reclaim the step it stood on.
 */
#define WALK_DEPTH  6
#define WALK_FILES  48   /* per branch: 4 x (6 + 48) is past fat's 128 */
#define WALK_PASSES 3

static unsigned long walk_misses;

static void walk_path(char *path, size_t sz, int branch, int depth) {
	int i, len;

	len = snprintf(path, sz, FS_DIR "/w%d", branch);
	for (i = 0; i < depth; i++) {
		len += snprintf(path + len, sz - len, "/l%d", i);
	}
}

static void *walk_main(void *arg) {
	int branch = (int)(intptr_t) arg;
	char path[160];
	struct stat st;
	int pass, i, base;

	walk_path(path, sizeof(path), branch, WALK_DEPTH);
	base = strlen(path);

	for (pass = 0; pass < WALK_PASSES; pass++) {
		for (i = 0; i < WALK_FILES; i++) {
			/* a different branch's files each pass, crossing the others */
			int b = (branch + pass) % THREADS;

			walk_path(path, sizeof(path), b, WALK_DEPTH);
			base = strlen(path);
			snprintf(path + base, sizeof(path) - base, "/f%02d", i);
			if (0 != stat(path, &st)) {
				bump(&walk_misses);
			}
		}
	}
	return NULL;
}

TEST_CASE("deep walks on four cores while the driver's pool runs dry") {
	unsigned long reclaimed_before = load(&dvfs_reclaimed);
	char path[160];
	int b, d, i, base;

	for (b = 0; b < THREADS; b++) {
		for (d = 0; d <= WALK_DEPTH; d++) {
			walk_path(path, sizeof(path), b, d);
			test_assert_zero(mkdir(path, 0777));
		}
		walk_path(path, sizeof(path), b, WALK_DEPTH);
		base = strlen(path);
		for (i = 0; i < WALK_FILES; i++) {
			snprintf(path + base, sizeof(path) - base, "/f%02d", i);
			test_assert_zero(file_make(path, "w", 1));
		}
	}

	walk_misses = 0;
	test_assert_zero(run_threads(walk_main));

	test_assert_zero(load(&walk_misses));
	/* The point of the sizes: the tree had to give something back. */
	test_assert_not_zero(load(&dvfs_reclaimed) - reclaimed_before);

	printk("dvfs_smp: walks: %lu dentries reclaimed on the way\n",
	    load(&dvfs_reclaimed) - reclaimed_before);
}

/* ------------------------------------------------------------------ *
 * readdir against create and unlink in the same directory. Every pass must
 * see each of the names nobody touches exactly once. FAT used to keep a
 * reader's position partly in the directory's shared data, so two readers,
 * or a reader and a create, moved each other.
 */
#define RD_DIR     FS_DIR "/rd"
#define RD_STABLE  30
#define RD_PASSES  12

static volatile int rd_run;
static unsigned long rd_bad_pass, rd_passes;

static void *readdir_main(void *arg) {
	int idx = (int)(intptr_t) arg;
	char path[64];
	int i = 0;

	if (idx < 2) {
		/* writers: churn names alongside the stable ones */
		while (rd_run) {
			snprintf(path, sizeof(path), RD_DIR "/t%d_%d", idx, i % 8);
			if (i & 8) {
				remove(path);
			} else {
				file_make(path, "t", 1);
			}
			i++;
		}
		return NULL;
	}

	for (i = 0; i < RD_PASSES; i++) {
		int seen[RD_STABLE] = { 0 };
		struct dirent *de;
		DIR *d;
		int n, k, bad = 0;

		d = opendir(RD_DIR);
		if (!d) {
			bump(&rd_bad_pass);
			continue;
		}
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == 's' && 1 == sscanf(de->d_name, "s%d", &n)
			    && n >= 0 && n < RD_STABLE) {
				seen[n]++;
			}
		}
		closedir(d);

		for (k = 0; k < RD_STABLE; k++) {
			if (seen[k] != 1) {
				bad = 1;
			}
		}
		if (bad) {
			bump(&rd_bad_pass);
		}
		bump(&rd_passes);
	}

	if (idx == THREADS - 1) {
		rd_run = 0;
	}
	return NULL;
}

TEST_CASE("readdir against create and unlink: the untouched names, once each") {
	char path[64];
	int i;

	test_assert_zero(mkdir(RD_DIR, 0777));
	for (i = 0; i < RD_STABLE; i++) {
		snprintf(path, sizeof(path), RD_DIR "/s%02d", i);
		test_assert_zero(file_make(path, "s", 1));
	}

	rd_run = 1;
	rd_bad_pass = rd_passes = 0;
	test_assert_zero(run_threads(readdir_main));

	test_assert_not_zero(load(&rd_passes));
	test_assert_zero(load(&rd_bad_pass));
}

/* ------------------------------------------------------------------ *
 * rename a <-> b in a loop against readers. Whatever the readers find must
 * be the file's content; at the end exactly one of the names is there.
 */
#define RN_A      FS_DIR "/ren_a"
#define RN_B      FS_DIR "/ren_b"
#define RN_ROUNDS 80

static volatile int rn_run;
static unsigned long rn_found, rn_wrong, rn_fail;

static void *rename_main(void *arg) {
	char data[PATTERN_SZ], got[PATTERN_SZ];
	int i, fd;

	if ((intptr_t) arg == 0) {
		for (i = 0; i < RN_ROUNDS; i++) {
			if (rename(RN_A, RN_B) || rename(RN_B, RN_A)) {
				bump(&rn_fail);
			}
		}
		rn_run = 0;
		return NULL;
	}

	pattern(data, 9);
	while (rn_run) {
		fd = open(((intptr_t) arg & 1) ? RN_A : RN_B, O_RDONLY);
		if (fd < 0) {
			continue;
		}
		if (PATTERN_SZ == read(fd, got, sizeof(got))) {
			bump(memcmp(got, data, PATTERN_SZ) ? &rn_wrong : &rn_found);
		}
		close(fd);
	}
	return NULL;
}

TEST_CASE("rename back and forth against readers: one name, the same bytes") {
	char data[PATTERN_SZ];
	struct stat st;
	int a, b;

	pattern(data, 9);
	test_assert_zero(file_make(RN_A, data, sizeof(data)));

	rn_run = 1;
	rn_found = rn_wrong = rn_fail = 0;
	test_assert_zero(run_threads(rename_main));

	test_assert_zero(load(&rn_fail));
	test_assert_zero(load(&rn_wrong));

	a = (0 == stat(RN_A, &st));
	b = (0 == stat(RN_B, &st));
	test_assert_equal(1, a + b);
	test_assert(a);

	test_assert_zero(remove(RN_A));
}

/* ------------------------------------------------------------------ *
 * Descriptors opened on one thread and closed on another, over and over,
 * with lookups going on: whatever the interleaving, every reference taken is
 * given back -- which the umount at the end is what checks.
 */
#define CL_FILE   FS_DIR "/shared"
#define CL_ROUNDS 150

static int cl_slots[THREADS];

static void *close_main(void *arg) {
	int me = (int)(intptr_t) arg;
	int other = (me + 1) % THREADS;
	int i, fd;
	struct stat st;

	for (i = 0; i < CL_ROUNDS; i++) {
		fd = open(CL_FILE, O_RDONLY);
		if (fd >= 0) {
			/* Hand it on; close whatever was handed to us. */
			fd = atomic_rmw_exchange(&cl_slots[other], fd, __ATOMIC_ACQ_REL);
			if (fd >= 0) {
				close(fd);
			}
		}
		fd = atomic_rmw_exchange(&cl_slots[me], -1, __ATOMIC_ACQ_REL);
		if (fd >= 0) {
			close(fd);
		}
		(void) stat(CL_FILE, &st);
	}
	return NULL;
}

TEST_CASE("open on one thread, close on another: every reference comes back") {
	int i;

	test_assert_zero(file_make(CL_FILE, "c", 1));
	for (i = 0; i < THREADS; i++) {
		cl_slots[i] = -1;
	}

	test_assert_zero(run_threads(close_main));

	for (i = 0; i < THREADS; i++) {
		if (cl_slots[i] >= 0) {
			close(cl_slots[i]);
		}
	}

	test_assert_zero(remove(CL_FILE));
}

/* ------------------------------------------------------------------ *
 * A task's working directory holds what it names. Umount must say EBUSY
 * while a task stands in the volume, and must work once it has gone -- the
 * working directory used to hold nothing, and was never let go either.
 */
#define PWD_DIR FS_DIR "/pwd"

static volatile int pwd_release;
static volatile int pwd_ready;

static void *pwd_task(void *arg) {
	if (chdir(PWD_DIR)) {
		return (void *) 1;
	}
	pwd_ready = 1;
	while (!pwd_release) {
		usleep(1000);
	}
	return NULL;
}

TEST_CASE("a task standing in a volume keeps it mounted until it leaves") {
	pid_t pid;

	test_assert_zero(mkdir(PWD_DIR, 0777));

	pwd_ready = pwd_release = 0;
	pid = new_task("dvfs_pwd", pwd_task, NULL);
	test_assert(pid >= 0);
	while (!pwd_ready) {
		usleep(1000);
	}

	test_assert_equal(-1, umount(FS_DIR));
	test_assert_equal(errno, EBUSY);

	pwd_release = 1;
	test_assert_zero(task_waitpid(pid));

	/* Everything in this suite has let go of the volume by now. */
	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));

	test_assert_zero(load(&dvfs_unlocked_tree));

	printk("dvfs_smp: tree contended %lu time(s), %lu umount(s) refused busy\n",
	    load(&vfs_tree_contended), load(&dvfs_umount_busy));
}
