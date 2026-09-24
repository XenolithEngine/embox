/**
 * @file
 * @brief Convert a string to a long long integer.
 *
 * @see stdlib.h
 * @see strtox.h
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "strtox.h"

long long strtoll(const char *nptr, char **endptr, int base) {
	int neg, ovf;
	unsigned long long m;

	m = strtox_scan(nptr, endptr, base, LLONG_MAX,
	    -(unsigned long long)LLONG_MIN, &neg, &ovf);
	if (ovf) {
		return neg ? LLONG_MIN : LLONG_MAX;
	}
	return neg ? (long long)(0 - m) : (long long)m;
}

int64_t strtoq(const char *nptr, char **endptr, int base) {
	return (int64_t)strtoll(nptr, endptr, base);
}
