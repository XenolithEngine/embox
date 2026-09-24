/**
 * @file
 * @brief The one integer scanner behind strtol, strtoul, strtoll and strtoull
 *
 * The four used to be separate BSD copies, and all four got the same two
 * things wrong: "0x" followed by no hex digit consumed the "x" and then
 * reported that nothing was converted (C wants the "0" converted and the end
 * pointer on the "x"), and an overflow clamped the value without saying
 * ERANGE.
 */

#ifndef COMPAT_LIBC_STDLIB_STRTOX_H_
#define COMPAT_LIBC_STDLIB_STRTOX_H_

#include <ctype.h>
#include <errno.h>

/* The magnitude of the number at @nptr, and whether it had a minus sign.
 * @max_pos and @max_neg are the largest magnitudes the caller's type holds for
 * each sign; past it the scan still consumes digits, sets *@ovf and returns
 * the limit. */
static inline unsigned long long strtox_scan(const char *nptr, char **endptr,
    int base, unsigned long long max_pos, unsigned long long max_neg,
    int *neg, int *ovf) {
	const unsigned char *s = (const unsigned char *)nptr;
	unsigned long long acc = 0, max;
	int any = 0;

	*neg = 0;
	*ovf = 0;
	if (base < 0 || base == 1 || base > 36) {
		errno = EINVAL;
		if (endptr) {
			*endptr = (char *)nptr;
		}
		return 0;
	}

	while (isspace(*s)) {
		s++;
	}
	if (*s == '-' || *s == '+') {
		*neg = *s++ == '-';
	}
	/* The prefix counts only when a hex digit follows it; otherwise the "0"
	 * alone is the number. */
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 0x20) == 'x'
	    && isxdigit(s[2])) {
		s += 2;
		base = 16;
	}
	else if (base == 0) {
		base = s[0] == '0' ? 8 : 10;
	}

	max = *neg ? max_neg : max_pos;
	for (;; s++) {
		unsigned d;

		if (isdigit(*s)) {
			d = *s - '0';
		}
		else if (isalpha(*s)) {
			d = (*s | 0x20) - 'a' + 10;
		}
		else {
			break;
		}
		if (d >= (unsigned)base) {
			break;
		}
		any = 1;
		if (*ovf) {
			continue;
		}
		if (acc > (max - d) / (unsigned)base) {
			*ovf = 1;
			acc = max;
		}
		else {
			acc = acc * base + d;
		}
	}

	if (endptr) {
		*endptr = (char *)(any ? (const char *)s : nptr);
	}
	if (*ovf) {
		errno = ERANGE;
	}
	return acc;
}

#endif /* COMPAT_LIBC_STDLIB_STRTOX_H_ */
