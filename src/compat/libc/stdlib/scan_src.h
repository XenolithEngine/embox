/**
 * @file
 * @brief The input that musl's scanners read: a string or a FILE
 *
 * floatscan (strtod.c) and the scanf family (../stdio/vfscanf.c) are musl's,
 * and musl's scanners read through four operations on a FILE:
 *
 *   shgetc(f)      the next byte, or EOF;
 *   shunget(f)     give the last byte back;
 *   shlim(f, lim)  start counting again from here, and read at most lim bytes
 *                  (0: no limit) -- a scanf field width;
 *   shcnt(f)       bytes read since the last shlim.
 *
 * Here they work over either a NUL-terminated string or an Embox FILE, whose
 * own pushback is one byte deep. Reaching the end or the limit reads EOF and
 * from then on shunget does nothing, as in musl: a scanner can always
 * shunget after the byte that stopped it, whether or not there was one.
 *
 * A stream needs to give back more than one byte only on the rare paths
 * where a scan backs out of a partial match ("nan(" with no closing
 * parenthesis, "infin"). The bytes read are remembered SCAN_SRC_HIST deep for
 * that; scan_src_end() hands the one byte C lets a stream keep back to
 * ungetc() and drops any others, which is the most C promises.
 */

#ifndef COMPAT_LIBC_STDLIB_SCAN_SRC_H_
#define COMPAT_LIBC_STDLIB_SCAN_SRC_H_

#include <stdio.h>

#define SCAN_SRC_HIST 32

struct fs_src {
	const unsigned char *str; /* the string, or NULL for a stream */
	FILE *file;
	long cnt;                 /* bytes read since the last shlim */
	long lim;                 /* limit for cnt; 0 none, -1 at EOF */
	/* stream only: bytes read, the latest last (a ring), and bytes given
	 * back, to be read again before the FILE is asked */
	unsigned char hist[SCAN_SRC_HIST];
	unsigned hist_n;
	unsigned char back[SCAN_SRC_HIST];
	unsigned back_n;
};

static inline void scan_src_string(struct fs_src *f, const char *s) {
	f->str = (const unsigned char *)s;
	f->file = NULL;
	f->cnt = 0;
	f->lim = 0;
	f->hist_n = f->back_n = 0;
}

static inline void scan_src_file(struct fs_src *f, FILE *file) {
	f->str = NULL;
	f->file = file;
	f->cnt = 0;
	f->lim = 0;
	f->hist_n = f->back_n = 0;
}

/* A stream's unread bytes go back to it: the first one, as ungetc allows. */
static inline void scan_src_end(struct fs_src *f) {
	if (!f->str && f->back_n) {
		ungetc(f->back[f->back_n - 1], f->file);
		f->back_n = 0;
	}
}

static inline int shgetc(struct fs_src *f) {
	int c;

	if (f->lim < 0 || (f->lim > 0 && f->cnt >= f->lim)) {
		f->lim = -1;
		return EOF;
	}
	if (f->str) {
		c = *f->str;
		if (c == '\0') {
			f->lim = -1;
			return EOF;
		}
		f->str++;
	}
	else {
		if (f->back_n) {
			c = f->back[--f->back_n];
		}
		else {
			c = getc(f->file);
			if (c == EOF) {
				f->lim = -1;
				return EOF;
			}
		}
		f->hist[f->hist_n++ % SCAN_SRC_HIST] = (unsigned char)c;
	}
	f->cnt++;
	return c;
}

static inline void shunget(struct fs_src *f) {
	if (f->lim < 0 || f->cnt == 0) {
		return;
	}
	f->cnt--;
	if (f->str) {
		f->str--;
	}
	else if (f->hist_n && f->back_n < SCAN_SRC_HIST) {
		f->back[f->back_n++] = f->hist[--f->hist_n % SCAN_SRC_HIST];
	}
}

static inline void shlim(struct fs_src *f, long lim) {
	f->cnt = 0;
	f->lim = lim;
}

static inline long shcnt(struct fs_src *f) {
	return f->cnt;
}

/* strtod.c: musl's __floatscan. prec 0, 1, 2: float, double, long double;
 * pok: whether a sign alone may be taken back (strtod) or not (scanf). */
extern long double __floatscan(struct fs_src *f, int prec, int pok);

#endif /* COMPAT_LIBC_STDLIB_SCAN_SRC_H_ */
