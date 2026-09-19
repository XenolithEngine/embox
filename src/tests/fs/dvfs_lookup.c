/**
 * @file
 * @brief DVFS name semantics, one thread: what lookup, create, remove, rename
 *        and flock answer, errno by errno.
 *
 * Every case here is something DVFS got wrong before it was given a lock and
 * reference counts, and none of them needs a second core to show it. The
 * SMP half is dvfs_smp.c.
 *
 * @date 19.09.26
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drivers/block_dev.h>
#include <drivers/block_dev/ramdisk/ramdisk.h>
#include <embox/test.h>
#include <fs/dvfs.h>
#include <fs/fsop.h>
#include <kernel/printk.h>
#include <util/atomic_rmw.h>
#include <util/err.h>

EMBOX_TEST_SUITE("dvfs: lookup, create, remove, rename, flock");

TEST_SETUP_SUITE(setup_suite);
TEST_TEARDOWN_SUITE(teardown_suite);

#define FS_NAME    "vfat"
#define FS_DEV     "/dev/ramdisk_dl"
#define FS_BYTES   (2 * 1024 * 1024)
#define FS_DIR     "/tmp_dl"

extern unsigned fat_unlocked_scratch __attribute__((weak));

static char buf[128];

static int file_make(const char *path, const char *text) {
	int fd;
	ssize_t len = strlen(text);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		return -errno;
	}
	if (len != write(fd, text, len)) {
		close(fd);
		return -EIO;
	}
	return close(fd);
}

/* The file's whole content, or "" if it cannot be read. */
static const char *file_text(const char *path) {
	int fd, len;

	buf[0] = '\0';
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return buf;
	}
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[len > 0 ? len : 0] = '\0';
	return buf;
}

static int exists(const char *path) {
	struct stat st;

	return 0 == stat(path, &st);
}

/* How many times NAME is listed in DIR. */
static int listed(const char *dir, const char *name) {
	struct dirent *de;
	DIR *d;
	int n = 0;

	d = opendir(dir);
	if (d == NULL) {
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, name)) {
			n++;
		}
	}
	closedir(d);
	return n;
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

TEST_CASE("a missing directory in the middle of a path is ENOENT") {
	/* It used to be taken for the parent: this made FS_DIR/c. */
	test_assert_equal(-1, open(FS_DIR "/no/such/c", O_WRONLY | O_CREAT, 0666));
	test_assert_equal(errno, ENOENT);
	test_assert_false(exists(FS_DIR "/c"));

	test_assert_equal(-1, mkdir(FS_DIR "/no/such", 0777));
	test_assert_equal(errno, ENOENT);
	test_assert_false(exists(FS_DIR "/such"));

	test_assert_false(exists(FS_DIR "/no/such/c"));
	test_assert_equal(errno, ENOENT);
}

TEST_CASE("a path through a file is ENOTDIR") {
	test_assert_zero(file_make(FS_DIR "/plain", "x"));
	test_assert_equal(-1, open(FS_DIR "/plain/x", O_RDONLY));
	test_assert_equal(errno, ENOTDIR);
	test_assert_zero(unlink(FS_DIR "/plain"));
}

TEST_CASE("O_CREAT|O_EXCL on a name that exists is EEXIST") {
	int fd;

	fd = open(FS_DIR "/excl", O_WRONLY | O_CREAT | O_EXCL, 0666);
	test_assert(fd >= 0);
	test_assert_zero(close(fd));

	test_assert_equal(-1, open(FS_DIR "/excl", O_WRONLY | O_CREAT | O_EXCL, 0666));
	test_assert_equal(errno, EEXIST);

	/* Without O_EXCL the same open is fine. */
	fd = open(FS_DIR "/excl", O_WRONLY | O_CREAT, 0666);
	test_assert(fd >= 0);
	test_assert_zero(close(fd));

	test_assert_equal(1, listed(FS_DIR, "excl"));
	test_assert_zero(unlink(FS_DIR "/excl"));
}

TEST_CASE("O_DIRECTORY on a file is ENOTDIR, a directory to read is EISDIR") {
	test_assert_zero(file_make(FS_DIR "/f", "x"));
	test_assert_zero(mkdir(FS_DIR "/d", 0777));

	test_assert_equal(-1, open(FS_DIR "/f", O_RDONLY | O_DIRECTORY));
	test_assert_equal(errno, ENOTDIR);

	test_assert_equal(-1, open(FS_DIR "/d", O_RDWR));
	test_assert_equal(errno, EISDIR);

	test_assert_zero(unlink(FS_DIR "/f"));
	test_assert_zero(rmdir(FS_DIR "/d"));
}

TEST_CASE("unlink, rmdir and remove answer what POSIX says") {
	test_assert_zero(mkdir(FS_DIR "/dir", 0777));
	test_assert_zero(file_make(FS_DIR "/dir/in", "x"));
	test_assert_zero(file_make(FS_DIR "/file", "x"));

	/* These two were stubs that answered 0 and did nothing. */
	test_assert_equal(-1, unlink(FS_DIR "/dir"));
	test_assert_equal(errno, EISDIR);
	test_assert_equal(-1, rmdir(FS_DIR "/file"));
	test_assert_equal(errno, ENOTDIR);

	test_assert_equal(-1, rmdir(FS_DIR "/dir"));
	test_assert_equal(errno, ENOTEMPTY);
	test_assert(exists(FS_DIR "/dir/in"));

	test_assert_zero(unlink(FS_DIR "/dir/in"));
	test_assert_zero(rmdir(FS_DIR "/dir"));
	test_assert_false(exists(FS_DIR "/dir"));

	test_assert_zero(unlink(FS_DIR "/file"));
	test_assert_false(exists(FS_DIR "/file"));

	test_assert_equal(-1, unlink(FS_DIR "/file"));
	test_assert_equal(errno, ENOENT);

	/* A mount point is not the file system's to remove. */
	test_assert_equal(-1, rmdir(FS_DIR));
	test_assert_equal(errno, EBUSY);

	test_assert_zero(file_make(FS_DIR "/viaremove", "x"));
	test_assert_zero(remove(FS_DIR "/viaremove"));
	test_assert_false(exists(FS_DIR "/viaremove"));
}

TEST_CASE("rename moves a file, and replaces a file that is there") {
	test_assert_zero(file_make(FS_DIR "/a", "alpha"));
	test_assert_zero(rename(FS_DIR "/a", FS_DIR "/b"));
	test_assert_false(exists(FS_DIR "/a"));
	test_assert_str_equal(file_text(FS_DIR "/b"), "alpha");

	/* It used to lose the target before it had checked anything. */
	test_assert_zero(file_make(FS_DIR "/c", "gamma"));
	test_assert_zero(rename(FS_DIR "/c", FS_DIR "/b"));
	test_assert_false(exists(FS_DIR "/c"));
	test_assert_str_equal(file_text(FS_DIR "/b"), "gamma");
	test_assert_equal(1, listed(FS_DIR, "b"));

	/* A long name, which FAT keeps in extra entries, both ways. */
	test_assert_zero(rename(FS_DIR "/b", FS_DIR "/a-rather-longer-name.data"));
	test_assert_str_equal(file_text(FS_DIR "/a-rather-longer-name.data"),
	    "gamma");
	test_assert_zero(rename(FS_DIR "/a-rather-longer-name.data", FS_DIR "/b"));
	test_assert_equal(0, listed(FS_DIR, "a-rather-longer-name.data"));
	test_assert_str_equal(file_text(FS_DIR "/b"), "gamma");

	test_assert_zero(unlink(FS_DIR "/b"));
}

TEST_CASE("rename of a directory takes what is in it, and its .. follows") {
	test_assert_zero(mkdir(FS_DIR "/p1", 0777));
	test_assert_zero(mkdir(FS_DIR "/p2", 0777));
	test_assert_zero(mkdir(FS_DIR "/p1/d", 0777));
	test_assert_zero(file_make(FS_DIR "/p1/d/x", "inside"));
	test_assert_zero(file_make(FS_DIR "/p2/marker", "p2"));

	test_assert_zero(rename(FS_DIR "/p1/d", FS_DIR "/p2/e"));
	test_assert_false(exists(FS_DIR "/p1/d"));
	test_assert_str_equal(file_text(FS_DIR "/p2/e/x"), "inside");
	test_assert_str_equal(file_text(FS_DIR "/p2/e/../marker"), "p2");

	/* Into itself is refused; onto a file, from a file onto a directory. */
	test_assert_equal(-1, rename(FS_DIR "/p2", FS_DIR "/p2/e/p2"));
	test_assert_equal(errno, EINVAL);
	test_assert_equal(-1, rename(FS_DIR "/p2/e", FS_DIR "/p2/marker"));
	test_assert_equal(errno, ENOTDIR);
	test_assert_equal(-1, rename(FS_DIR "/p2/marker", FS_DIR "/p2/e"));
	test_assert_equal(errno, EISDIR);

	/* Across file systems it is EXDEV, which is what makes mv copy. */
	test_assert_equal(-1, rename(FS_DIR "/p2/marker", "/marker"));
	test_assert_equal(errno, EXDEV);

	/* The on-disk result, not only the cached one: remount and look again. */
	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));
	test_assert_str_equal(file_text(FS_DIR "/p2/e/x"), "inside");
	test_assert_str_equal(file_text(FS_DIR "/p2/e/../marker"), "p2");
	test_assert_false(exists(FS_DIR "/p1/d"));

	test_assert_zero(unlink(FS_DIR "/p2/e/x"));
	test_assert_zero(rmdir(FS_DIR "/p2/e"));
	test_assert_zero(unlink(FS_DIR "/p2/marker"));
	test_assert_zero(rmdir(FS_DIR "/p2"));
	test_assert_zero(rmdir(FS_DIR "/p1"));
}

TEST_CASE("a relative path starts at the working directory") {
	int fd;

	test_assert_zero(chdir(FS_DIR));
	fd = open("rel", O_WRONLY | O_CREAT, 0666);
	test_assert(fd >= 0);
	test_assert_zero(close(fd));
	test_assert(exists(FS_DIR "/rel"));

	/* .. from the root of a mount is the directory the mount is in */
	test_assert(exists("../" FS_DIR));
	test_assert_zero(chdir("/"));

	test_assert_zero(unlink(FS_DIR "/rel"));
}

TEST_CASE("a directory is listed once, the mount point included") {
	test_assert_zero(file_make(FS_DIR "/l1", "x"));
	test_assert_zero(file_make(FS_DIR "/l2", "x"));

	test_assert_equal(1, listed(FS_DIR, "l1"));
	test_assert_equal(1, listed(FS_DIR, "l2"));
	/* tmp_dl is in the initfs AND a mount point */
	test_assert_equal(1, listed("/", &FS_DIR[1]));

	test_assert_zero(unlink(FS_DIR "/l1"));
	test_assert_zero(unlink(FS_DIR "/l2"));
	test_assert_equal(0, listed(FS_DIR, "l1"));
}

TEST_CASE("flock belongs to the open file, and closing lets it go") {
	int fd1, fd2;

	test_assert_zero(file_make(FS_DIR "/lock", "x"));
	fd1 = open(FS_DIR "/lock", O_RDONLY);
	fd2 = open(FS_DIR "/lock", O_RDONLY);
	test_assert(fd1 >= 0 && fd2 >= 0);

	/* It was a stub that said yes to everything. */
	test_assert_zero(flock(fd1, LOCK_EX));
	test_assert_equal(-1, flock(fd2, LOCK_EX | LOCK_NB));
	test_assert_equal(errno, EWOULDBLOCK);
	test_assert_equal(-1, flock(fd2, LOCK_SH | LOCK_NB));
	test_assert_equal(errno, EWOULDBLOCK);

	/* Down to shared: the other may share now, and not take it whole. */
	test_assert_zero(flock(fd1, LOCK_SH));
	test_assert_zero(flock(fd2, LOCK_SH | LOCK_NB));
	test_assert_equal(-1, flock(fd2, LOCK_EX | LOCK_NB));
	test_assert_equal(errno, EWOULDBLOCK);

	test_assert_zero(close(fd1));
	test_assert_zero(flock(fd2, LOCK_EX | LOCK_NB));
	test_assert_zero(flock(fd2, LOCK_UN));
	test_assert_zero(close(fd2));

	test_assert_zero(unlink(FS_DIR "/lock"));
}

TEST_CASE("mknod says no rather than yes") {
	test_assert_equal(-1, mknod(FS_DIR "/node", S_IFCHR | 0666, 0));
	test_assert_false(exists(FS_DIR "/node"));
}

TEST_CASE("nothing above is still holding the volume, and nothing ran unlocked") {
	/* Every descriptor, DIR and lookup above has been given back, or this is
	 * EBUSY: the umount counts the references on the volume's dentries. */
	test_assert_zero(umount(FS_DIR));
	test_assert_zero(mount(FS_DEV, FS_DIR, FS_NAME, 0, NULL));

	test_assert_zero(atomic_rmw_load(&dvfs_unlocked_tree, __ATOMIC_RELAXED));
	if (&fat_unlocked_scratch) {
		test_assert_zero(fat_unlocked_scratch);
	}
}
