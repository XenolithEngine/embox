/**
 * @file
 * @brief Convert a string to a long integer.
 *
 * @see stdlib.h
 * @see strtox.h
 */

#include <limits.h>
#include <stdlib.h>

#include "strtox.h"

long strtol(const char *nptr, char **endptr, int base) {
	int neg, ovf;
	unsigned long long m;

	m = strtox_scan(nptr, endptr, base, LONG_MAX, -(unsigned long long)LONG_MIN,
	    &neg, &ovf);
	if (ovf) {
		return neg ? LONG_MIN : LONG_MAX;
	}
	return neg ? (long)(0 - (unsigned long)m) : (long)m;
}
