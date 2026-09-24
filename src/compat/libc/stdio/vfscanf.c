/**
 * @file
 * @brief The scanf family: scanf, fscanf, sscanf and their v* forms
 *
 * musl's (src/stdio/vfscanf.c and src/internal/intscan.c, musl 1.2.x) over
 * the string-or-FILE reader of ../stdlib/scan_src.h, with floating point by
 * the same __floatscan strtod uses. What it replaces had no v* forms at all,
 * scanned every integer into an int (so %lld lost the upper half), read %f as
 * an integer and knew no scansets.
 *
 * Wide conversions (%lc, %ls, %l[) store each byte as one wchar_t: the libc
 * has no multibyte state to decode with.
 *
 * musl is Copyright (c) 2005-2020 Rich Felker, et al., under the MIT license:
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions: The
 * above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
 * "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../stdlib/scan_src.h"

/* The value of c as a digit, or 255 when it is none (EOF included). */
static unsigned digit_val(int c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'Z') {
		return c - 'A' + 10;
	}
	return 255;
}

static unsigned long long intscan(struct fs_src *f, unsigned base, int pok,
    unsigned long long lim) {
	int c, neg = 0;
	unsigned x;
	unsigned long long y;

	if (base > 36 || base == 1) {
		errno = EINVAL;
		return 0;
	}
	while (isspace((c = shgetc(f))))
		;
	if (c == '+' || c == '-') {
		neg = -(c == '-');
		c = shgetc(f);
	}
	if ((base == 0 || base == 16) && c == '0') {
		c = shgetc(f);
		if ((c | 32) == 'x') {
			c = shgetc(f);
			if (digit_val(c) >= 16) {
				shunget(f);
				if (pok) {
					shunget(f);
				}
				else {
					shlim(f, 0);
				}
				return 0;
			}
			base = 16;
		}
		else if (base == 0) {
			base = 8;
		}
	}
	else {
		if (base == 0) {
			base = 10;
		}
		if (digit_val(c) >= base) {
			shunget(f);
			shlim(f, 0);
			errno = EINVAL;
			return 0;
		}
	}
	if (base == 10) {
		for (x = 0; c - '0' < 10U && x <= UINT_MAX / 10 - 1; c = shgetc(f)) {
			x = x * 10 + (c - '0');
		}
		for (y = x; c - '0' < 10U && y <= ULLONG_MAX / 10
		            && 10 * y <= ULLONG_MAX - (c - '0');
		     c = shgetc(f)) {
			y = y * 10 + (c - '0');
		}
		if (c - '0' >= 10U) {
			goto done;
		}
	}
	else if (!(base & (base - 1))) {
		int bs = "\0\1\2\4\7\3\6\5"[(0x17 * base) >> 5 & 7];
		for (x = 0; digit_val(c) < base && x <= UINT_MAX / 32; c = shgetc(f)) {
			x = x << bs | digit_val(c);
		}
		for (y = x; digit_val(c) < base && y <= ULLONG_MAX >> bs; c = shgetc(f)) {
			y = y << bs | digit_val(c);
		}
	}
	else {
		for (x = 0; digit_val(c) < base && x <= UINT_MAX / 36 - 1;
		     c = shgetc(f)) {
			x = x * base + digit_val(c);
		}
		for (y = x; digit_val(c) < base && y <= ULLONG_MAX / base
		            && base * y <= ULLONG_MAX - digit_val(c);
		     c = shgetc(f)) {
			y = y * base + digit_val(c);
		}
	}
	if (digit_val(c) < base) {
		for (; digit_val(c) < base; c = shgetc(f))
			;
		errno = ERANGE;
		y = lim;
		if (lim & 1) {
			neg = 0;
		}
	}
done:
	shunget(f);
	if (y >= lim) {
		if (!(lim & 1) && !neg) {
			errno = ERANGE;
			return lim - 1;
		}
		else if (y > lim) {
			errno = ERANGE;
			return lim;
		}
	}
	return (y ^ neg) - neg;
}

#define SIZE_hh -2
#define SIZE_h  -1
#define SIZE_def 0
#define SIZE_l  1
#define SIZE_L  2
#define SIZE_ll 3

static void store_int(void *dest, int size, unsigned long long i) {
	if (!dest) {
		return;
	}
	switch (size) {
	case SIZE_hh:
		*(char *)dest = i;
		break;
	case SIZE_h:
		*(short *)dest = i;
		break;
	case SIZE_def:
		*(int *)dest = i;
		break;
	case SIZE_l:
		*(long *)dest = i;
		break;
	case SIZE_ll:
		*(long long *)dest = i;
		break;
	}
}

static void *arg_n(va_list ap, unsigned int n) {
	void *p;
	unsigned int i;
	va_list ap2;

	va_copy(ap2, ap);
	for (i = n; i > 1; i--) {
		va_arg(ap2, void *);
	}
	p = va_arg(ap2, void *);
	va_end(ap2);
	return p;
}

static int scan(struct fs_src *f, const char *fmt, va_list ap) {
	int width;
	int size;
	int alloc = 0;
	int base;
	const unsigned char *p;
	int c, t;
	char *s = NULL;
	wchar_t *wcs = NULL;
	void *dest = NULL;
	int invert;
	int matches = 0;
	unsigned long long x;
	long double y;
	long pos = 0;
	unsigned char scanset[257];
	size_t i, k;

	for (p = (const unsigned char *)fmt; *p; p++) {
		alloc = 0;

		if (isspace(*p)) {
			while (isspace(p[1])) {
				p++;
			}
			shlim(f, 0);
			while (isspace(shgetc(f)))
				;
			shunget(f);
			pos += shcnt(f);
			continue;
		}
		if (*p != '%' || p[1] == '%') {
			shlim(f, 0);
			if (*p == '%') {
				p++;
				while (isspace((c = shgetc(f))))
					;
			}
			else {
				c = shgetc(f);
			}
			if (c != *p) {
				shunget(f);
				if (c < 0) {
					goto input_fail;
				}
				goto match_fail;
			}
			pos += shcnt(f);
			continue;
		}

		p++;
		if (*p == '*') {
			dest = 0;
			p++;
		}
		else if (isdigit(*p) && p[1] == '$') {
			dest = arg_n(ap, *p - '0');
			p += 2;
		}
		else {
			dest = va_arg(ap, void *);
		}

		for (width = 0; isdigit(*p); p++) {
			width = 10 * width + *p - '0';
		}

		if (*p == 'm') {
			wcs = 0;
			s = 0;
			alloc = !!dest;
			p++;
		}
		else {
			alloc = 0;
		}

		size = SIZE_def;
		switch (*p++) {
		case 'h':
			if (*p == 'h') {
				p++, size = SIZE_hh;
			}
			else {
				size = SIZE_h;
			}
			break;
		case 'l':
			if (*p == 'l') {
				p++, size = SIZE_ll;
			}
			else {
				size = SIZE_l;
			}
			break;
		case 'j':
			size = SIZE_ll;
			break;
		case 'z':
		case 't':
			size = SIZE_l;
			break;
		case 'L':
			size = SIZE_L;
			break;
		case 'd':
		case 'i':
		case 'o':
		case 'u':
		case 'x':
		case 'a':
		case 'e':
		case 'f':
		case 'g':
		case 'A':
		case 'E':
		case 'F':
		case 'G':
		case 'X':
		case 's':
		case 'c':
		case '[':
		case 'S':
		case 'C':
		case 'p':
		case 'n':
			p--;
			break;
		default:
			goto fmt_fail;
		}

		t = *p;

		/* C or S */
		if ((t & 0x2f) == 3) {
			t |= 32;
			size = SIZE_l;
		}

		switch (t) {
		case 'c':
			if (width < 1) {
				width = 1;
			}
			/* fallthrough */
		case '[':
			break;
		case 'n':
			store_int(dest, size, pos);
			/* do not increment match count, etc! */
			continue;
		default:
			shlim(f, 0);
			while (isspace(shgetc(f)))
				;
			shunget(f);
			pos += shcnt(f);
		}

		shlim(f, width);
		if (shgetc(f) < 0) {
			goto input_fail;
		}
		shunget(f);

		switch (t) {
		case 's':
		case 'c':
		case '[':
			if (t == 'c' || t == 's') {
				memset(scanset, -1, sizeof scanset);
				scanset[0] = 0;
				if (t == 's') {
					scanset[1 + '\t'] = 0;
					scanset[1 + '\n'] = 0;
					scanset[1 + '\v'] = 0;
					scanset[1 + '\f'] = 0;
					scanset[1 + '\r'] = 0;
					scanset[1 + ' '] = 0;
				}
			}
			else {
				if (*++p == '^') {
					p++, invert = 1;
				}
				else {
					invert = 0;
				}
				memset(scanset, invert, sizeof scanset);
				scanset[0] = 0;
				if (*p == '-') {
					p++, scanset[1 + '-'] = 1 - invert;
				}
				else if (*p == ']') {
					p++, scanset[1 + ']'] = 1 - invert;
				}
				for (; *p != ']'; p++) {
					if (!*p) {
						goto fmt_fail;
					}
					if (*p == '-' && p[1] && p[1] != ']') {
						for (c = p++[-1]; c < *p; c++) {
							scanset[1 + c] = 1 - invert;
						}
					}
					scanset[1 + *p] = 1 - invert;
				}
			}
			wcs = 0;
			s = 0;
			i = 0;
			k = t == 'c' ? width + 1U : 31;
			if (size == SIZE_l) {
				/* one byte, one wchar_t: see the note at the top */
				if (alloc) {
					wcs = malloc(k * sizeof(wchar_t));
					if (!wcs) {
						goto alloc_fail;
					}
				}
				else {
					wcs = dest;
				}
				while (scanset[(c = shgetc(f)) + 1]) {
					if (wcs) {
						wcs[i] = (unsigned char)c;
					}
					i++;
					if (alloc && i == k) {
						wchar_t *tmp;
						k += k + 1;
						tmp = realloc(wcs, k * sizeof(wchar_t));
						if (!tmp) {
							goto alloc_fail;
						}
						wcs = tmp;
					}
				}
			}
			else if (alloc) {
				s = malloc(k);
				if (!s) {
					goto alloc_fail;
				}
				while (scanset[(c = shgetc(f)) + 1]) {
					s[i++] = c;
					if (i == k) {
						char *tmp;
						k += k + 1;
						tmp = realloc(s, k);
						if (!tmp) {
							goto alloc_fail;
						}
						s = tmp;
					}
				}
			}
			else if ((s = dest)) {
				while (scanset[(c = shgetc(f)) + 1]) {
					s[i++] = c;
				}
			}
			else {
				while (scanset[(c = shgetc(f)) + 1])
					;
			}
			shunget(f);
			if (!shcnt(f)) {
				goto match_fail;
			}
			if (t == 'c' && shcnt(f) != width) {
				goto match_fail;
			}
			if (alloc) {
				if (size == SIZE_l) {
					*(wchar_t **)dest = wcs;
				}
				else {
					*(char **)dest = s;
				}
			}
			if (t != 'c') {
				if (wcs) {
					wcs[i] = 0;
				}
				if (s) {
					s[i] = 0;
				}
			}
			break;
		case 'p':
		case 'X':
		case 'x':
			base = 16;
			goto int_common;
		case 'o':
			base = 8;
			goto int_common;
		case 'd':
		case 'u':
			base = 10;
			goto int_common;
		case 'i':
			base = 0;
		int_common:
			x = intscan(f, base, 0, ULLONG_MAX);
			if (!shcnt(f)) {
				goto match_fail;
			}
			if (t == 'p' && dest) {
				*(void **)dest = (void *)(uintptr_t)x;
			}
			else {
				store_int(dest, size, x);
			}
			break;
		case 'a':
		case 'A':
		case 'e':
		case 'E':
		case 'f':
		case 'F':
		case 'g':
		case 'G':
			y = __floatscan(f, size, 0);
			if (!shcnt(f)) {
				goto match_fail;
			}
			if (dest) {
				switch (size) {
				case SIZE_def:
					*(float *)dest = y;
					break;
				case SIZE_l:
					*(double *)dest = y;
					break;
				case SIZE_L:
					*(long double *)dest = y;
					break;
				}
			}
			break;
		}

		pos += shcnt(f);
		if (dest) {
			matches++;
		}
	}
	if (0) {
fmt_fail:
alloc_fail:
input_fail:
		if (!matches) {
			matches--;
		}
match_fail:
		if (alloc) {
			free(s);
			free(wcs);
		}
	}
	return matches;
}

int vfscanf(FILE *restrict file, const char *restrict fmt, va_list ap) {
	struct fs_src f;
	int ret;

	scan_src_file(&f, file);
	ret = scan(&f, fmt, ap);
	scan_src_end(&f);
	return ret;
}

int vscanf(const char *restrict fmt, va_list ap) {
	return vfscanf(stdin, fmt, ap);
}

int vsscanf(const char *restrict str, const char *restrict fmt, va_list ap) {
	struct fs_src f;

	scan_src_string(&f, str);
	return scan(&f, fmt, ap);
}

int fscanf(FILE *restrict file, const char *restrict fmt, ...) {
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vfscanf(file, fmt, ap);
	va_end(ap);
	return ret;
}

int scanf(const char *restrict fmt, ...) {
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vfscanf(stdin, fmt, ap);
	va_end(ap);
	return ret;
}

int sscanf(const char *restrict str, const char *restrict fmt, ...) {
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vsscanf(str, fmt, ap);
	va_end(ap);
	return ret;
}
