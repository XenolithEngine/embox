/**
 * @file
 * @brief Convert a string to an unsigned long long integer.
 *
 * @see stdlib.h
 * @see strtox.h
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "strtox.h"

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
	int neg, ovf;
	unsigned long long m;

	m = strtox_scan(nptr, endptr, base, ULLONG_MAX, ULLONG_MAX, &neg, &ovf);
	if (ovf) {
		return ULLONG_MAX;
	}
	return neg ? 0 - m : m;
}

uint64_t strtouq(const char *nptr, char **endptr, int base) {
	return (uint64_t)strtoull(nptr, endptr, base);
}
