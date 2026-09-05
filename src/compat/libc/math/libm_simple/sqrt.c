/* XENOLITH_FSQRT — see board/embox-qemu/patches/libm-sqrt-fsqrt.py */
#include <math.h>

float sqrtf(float x) {
	if (isnan(x) || x < 0.0f)
		return NAN;
	if (isinf(x))
		return INFINITY;
	if (x == 0.0f)
		return x;
	float r;
	__asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
	return r;
}

double sqrt(double x) {
	if (isnan(x) || x < 0.0)
		return NAN;
	if (isinf(x))
		return INFINITY;
	if (x == 0.0)
		return x;
	double r;
	__asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
	return r;
}

long double sqrtl(long double x) {
	return (long double)sqrt((double)x);
}
