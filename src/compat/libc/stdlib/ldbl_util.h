/**
 * @file
 * @brief long double helpers for strtod and printf, with no libm
 *
 * floatscan.c and ../stdio/print_fp.c need scalbnl, frexpl and an exact
 * truncation, and nothing else from libm. An image may link a header-only libm
 * (math_builtins, when an application brings its own) or none worth trusting
 * (math_simple's series), so these are done here with plain arithmetic:
 * multiplications by powers of two, which are exact while the result is a
 * normal number. They work for every long double this tree is built with --
 * 64-bit (arm), 80-bit (x86) and 128-bit (aarch64) -- without looking at the
 * bits.
 */

#ifndef COMPAT_LIBC_STDLIB_LDBL_UTIL_H_
#define COMPAT_LIBC_STDLIB_LDBL_UTIL_H_

#include <float.h>

/* 2^(2^i) for i = 0..13: 2^1 .. 2^8192, the largest a long double of any of
 * these formats can hold in its exponent range (LDBL_MAX_EXP is 16384). */
static inline long double ldbl_pow2_pow2(int i) {
	long double p = 2.0L;
	while (i--) {
		p *= p;
	}
	return p;
}

/* x * 2^n, rounding at most once: every step but the last is exact. */
static inline long double ldbl_scalbn(long double x, int n) {
	int i;

	if (x == 0 || x != x || x - x != 0) {
		return x; /* zero, nan, infinity */
	}
	if (n > 0) {
		for (i = 13; i >= 0 && n > 0; i--) {
			while (n >= (1 << i)) {
				x *= ldbl_pow2_pow2(i);
				n -= 1 << i;
				if (x - x != 0) {
					return x; /* overflowed to infinity */
				}
			}
		}
	}
	else {
		n = -n;
		for (i = 13; i >= 0 && n > 0; i--) {
			while (n >= (1 << i)) {
				x /= ldbl_pow2_pow2(i);
				n -= 1 << i;
				if (x == 0) {
					return x;
				}
			}
		}
	}
	return x;
}

/* The mantissa in [0.5, 1) and the exponent, as frexpl() does. */
static inline long double ldbl_frexp(long double x, int *e) {
	long double ax;
	int i, neg;

	*e = 0;
	if (x == 0 || x != x || x - x != 0) {
		return x;
	}
	neg = x < 0;
	ax = neg ? -x : x;
	for (i = 13; i >= 0; i--) {
		long double p = ldbl_pow2_pow2(i);
		while (ax >= p) {
			ax /= p;
			*e += 1 << i;
		}
	}
	for (i = 13; i >= 0; i--) {
		long double p = ldbl_pow2_pow2(i);
		while (ax * p < 1.0L) {
			ax *= p;
			*e -= 1 << i;
		}
	}
	/* The loop stops short of 1 when one more doubling lands on it exactly
	 * (0.5 stays 0.5); ax is in [0.5, 1) or [1, 2) here. */
	if (ax < 1.0L) {
		ax *= 2.0L;
		*e -= 1;
	}
	/* ax is in [1, 2) now */
	ax *= 0.5L;
	*e += 1;
	return neg ? -ax : ax;
}

/* Rounded toward zero to an integer, exactly. */
static inline long double ldbl_trunc(long double x) {
	/* Past 2^(MANT_DIG-1) every long double is an integer already. Below it,
	 * adding and taking away that power of two leaves x rounded to an integer
	 * (to nearest, the mode this runs in); one step back if that went up. */
	volatile long double big = ldbl_scalbn(1.0L, LDBL_MANT_DIG - 1);
	volatile long double t;
	long double ax = x < 0 ? -x : x;

	if (!(ax < big)) {
		return x;
	}
	t = (ax + big) - big;
	if (t > ax) {
		t -= 1.0L;
	}
	return x < 0 ? -t : t;
}

/* fmodl(x, 2^k): what floatscan asks of fmodl, and all it asks. */
static inline long double ldbl_fmod_pow2(long double x, long double p2) {
	return x - ldbl_trunc(x / p2) * p2;
}

#endif /* COMPAT_LIBC_STDLIB_LDBL_UTIL_H_ */
