/**
 * @file
 * @brief Convert a string to an unsigned long integer.
 *
 * @see stdlib.h
 * @see strtox.h
 */

#include <limits.h>
#include <stdlib.h>

#include "strtox.h"

/* A minus sign negates in the unsigned type, as C says: "-1" is ULONG_MAX. */
unsigned long strtoul(const char *nptr, char **endptr, int base) {
	int neg, ovf;
	unsigned long long m;

	m = strtox_scan(nptr, endptr, base, ULONG_MAX, ULONG_MAX, &neg, &ovf);
	if (ovf) {
		return ULONG_MAX;
	}
	return neg ? 0 - (unsigned long)m : (unsigned long)m;
}
