/**
 * @file
 * @brief
 *
 * @date 19.11.10
 * @author Anton Bondarev
 * @author Ilia Vaprol
 */

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <framework/mod/options.h>
#include <util/math.h>

#include "printf_impl.h"
#include "print_fp.h"

#define SUPPORT_FLOATING OPTION_GET(NUMBER, support_floating)

/* clang-format off */

/**
 * Format specifiers
 */
#define OPS_FLAG_LEFT_ALIGN  0x00000001 /* left alignment */
#define OPS_FLAG_WITH_SIGN   0x00000002 /* print sign */
#define OPS_FLAG_EXTRA_SPACE 0x00000004 /* add extra space before digit */
#define OPS_FLAG_WITH_SPEC   0x00000008 /* print a prefix for non-decimal systems */
#define OPS_FLAG_ZERO_PAD    0x00000010 /* padding with zeroes */
#define OPS_PREC_IS_GIVEN    0x00000020 /* precision is specified */
#define OPS_LEN_MIN          0x00000040 /* s_char (d, i); u_char (u, o, x, X); s_char* (n) */
#define OPS_LEN_SHORT        0x00000080 /* short (d, i); u_short (u, o, x, X); short* (n) */
#define OPS_LEN_LONG         0x00000100 /* long (d, i); u_long (u, o, x, X); wint_t (c); wchar_t(s); long* (n) */
#define OPS_LEN_LONGLONG     0x00000200 /* llong (d, i); u_llong (u, o, x, X); llong* (n) */
#define OPS_LEN_MAX          0x00000400 /* intmax_t (d, i); uintmax_t (u, o, x, X); intmax_t* (n) */
#define OPS_LEN_SIZE         0x00000800 /* size_t (d, i, u, o, x, X); size_t* (n) */
#define OPS_LEN_PTRDIFF      0x00001000 /* ptrdiff_t (d, i, u, o, x, X); ptrdiff_t* (n) */
#define OPS_LEN_LONGFP       0x00002000 /* long double (f, F, e, E, g, G, a, A) */
#define OPS_SPEC_UPPER_CASE  0x00004000 /* specifier is tall */

/**
 * Options for print_s
 */
#define PRINT_S_NULL_STR "(null)" /* default value of NULL */

/**
 * Options for print_i
 */
/* size of buffer for long long int (64bit) -- that's enough for oct, dec and hex base systems */
#define PRINT_I_BUFF_SZ 23

/* clang-format on */

static int
print_s(int (*printchar_handler)(struct printchar_handler_data *d, int c),
    struct printchar_handler_data *printchar_data, const char *str, int width,
    int max_len, unsigned int ops) {
	int pc, len, space_count, ret;

	assert(printchar_handler != NULL);
	assert(str != NULL);
	assert(width >= 0);
	assert(max_len >= 0);

	pc = 0;
	len = strlen(str);
	if (ops & OPS_PREC_IS_GIVEN) {
		len = min(max_len, len);
	}
	space_count = width > len ? width - len : 0;

	if (!(ops & OPS_FLAG_LEFT_ALIGN)) {
		pc += space_count;
		for (; space_count; --space_count) {
			ret = printchar_handler(printchar_data, ' ');
			if (ret < 0) {
				return ret;
			}
		}
	}

	pc += len;
	while (len--) {
		ret = printchar_handler(printchar_data, *str++);
		if (ret < 0) {
			return ret;
		}
	}

	pc += space_count;
	while (space_count--) {
		ret = printchar_handler(printchar_data, ' ');
		if (ret < 0) {
			return ret;
		}
	}

	return pc;
}

static int
print_i(int (*printchar_handler)(struct printchar_handler_data *d, int c),
    struct printchar_handler_data *printchar_data, unsigned long long int u,
    int is_signed, int width, int min_len, unsigned int ops, int base) {
	char buff[PRINT_I_BUFF_SZ], *str, *end, *prefix;
	int pc, ch, len, prefix_len, zero_count, space_count, letter_base, ret;

	assert(printchar_handler != NULL);
	assert(width >= 0);
	assert(min_len >= 0);

	str = end = &buff[0] + sizeof buff / sizeof buff[0] - 1;
	*end = '\0';

	if (is_signed && ((long long int)u < 0)) {
		u = -u;
		prefix = "-";
	}
	else if (is_signed && (ops & OPS_FLAG_WITH_SIGN)) {
		prefix = "+";
	}
	else if (is_signed && (ops & OPS_FLAG_EXTRA_SPACE)) {
		prefix = " ";
	}
	else if ((base == 16) && (ops & OPS_FLAG_WITH_SPEC) && u != 0) {
		/* "%#x" of 0 is a bare "0" */
		prefix = (ops & OPS_SPEC_UPPER_CASE) ? "0X" : "0x";
	}
	else {
		prefix = "";
	}

	pc = 0;
	prefix_len = strlen(prefix);
	letter_base = ops & OPS_SPEC_UPPER_CASE ? 'A' : 'a';

	/* An explicit precision of 0 prints no digits for 0 at all. */
	if (u != 0 || !(ops & OPS_PREC_IS_GIVEN) || min_len != 0) {
		do {
			ch = u % base;
			if (ch >= 10) {
				ch += letter_base - 10 - '0';
			}
			*--str = ch + '0';
			u /= base;
		} while (u);
	}

	len = end - str;
	/* "%#o" makes the first digit a 0, by precision if it has to. */
	if ((base == 8) && (ops & OPS_FLAG_WITH_SPEC) && (len == 0 || *str != '0')) {
		min_len = max(min_len, len + 1);
		ops |= OPS_PREC_IS_GIVEN;
	}
	/* The precision is the least number of digits, and when there is one the
	 * '0' flag is ignored; without one, '0' pads to the width after the sign
	 * or prefix. */
	if (ops & OPS_PREC_IS_GIVEN) {
		zero_count = min_len - len;
	}
	else if ((ops & OPS_FLAG_ZERO_PAD) && !(ops & OPS_FLAG_LEFT_ALIGN)) {
		zero_count = width - len - prefix_len;
	}
	else {
		zero_count = 0;
	}
	zero_count = max(zero_count, 0);
	space_count = width - len - prefix_len - zero_count;
	space_count = max(space_count, 0);

	if (!(ops & OPS_FLAG_LEFT_ALIGN)) {
		pc += space_count;
		for (; space_count; --space_count) {
			ret = printchar_handler(printchar_data, ' ');
			if (ret < 0) {
				return ret;
			}
		}
	}

	pc += prefix_len;
	while (prefix_len--) {
		ret = printchar_handler(printchar_data, *prefix++);
		if (ret < 0) {
			return ret;
		}
	}

	pc += zero_count;
	while (zero_count--) {
		ret = printchar_handler(printchar_data, '0');
		if (ret < 0) {
			return ret;
		}
	}

	pc += len;
	while (len--) {
		ret = printchar_handler(printchar_data, *str++);
		if (ret < 0) {
			return ret;
		}
	}

	pc += space_count;
	while (space_count--) {
		ret = printchar_handler(printchar_data, ' ');
		if (ret < 0) {
			return ret;
		}
	}

	return pc;
}

#if SUPPORT_FLOATING
/* The conversion itself is in print_fp.c (musl's fmt_fp); this only says in
 * musl's terms what the format asked for. */
static int
print_f(int (*printchar_handler)(struct printchar_handler_data *d, int c),
    struct printchar_handler_data *printchar_data, long double r, int width,
    int precision, unsigned int ops, int conv) {
	unsigned int fl = 0;

	assert(printchar_handler != NULL);

	if (ops & OPS_FLAG_LEFT_ALIGN) {
		fl |= LEFT_ADJ;
	}
	if (ops & OPS_FLAG_WITH_SIGN) {
		fl |= MARK_POS;
	}
	if (ops & OPS_FLAG_EXTRA_SPACE) {
		fl |= PAD_POS;
	}
	if (ops & OPS_FLAG_WITH_SPEC) {
		fl |= ALT_FORM;
	}
	if ((ops & OPS_FLAG_ZERO_PAD) && !(ops & OPS_FLAG_LEFT_ALIGN)) {
		fl |= ZERO_PAD;
	}

	return __print_fp(printchar_handler, printchar_data, r, width,
	    (ops & OPS_PREC_IS_GIVEN) ? precision : -1, fl, conv,
	    (ops & OPS_LEN_LONGFP) != 0);
}
#else
static int
print_f(int (*printchar_handler)(struct printchar_handler_data *d, int c),
    struct printchar_handler_data *printchar_data, long double r, int width,
    int precision, unsigned int ops, int conv) {
	return print_s(printchar_handler, printchar_data, "%f", 0, 0, 0);
}
#endif

int __print(int (*printchar_handler)(struct printchar_handler_data *d, int c),
    struct printchar_handler_data *printchar_data, const char *format,
    va_list args) {
	int pc, width, precision, ret;
	unsigned int ops;
	const char *begin;
	union {
		void *vp;
		char ca[2];
		char *cp;
		unsigned long long int ulli;
		long double ld;
	} tmp;

	assert(printchar_handler != NULL);
	assert(format != NULL);

	pc = 0;

	for (begin = format; *format; begin = ++format) {
		/**
		 * %[flags][width][.precision][length]specifier
		 */

		/* check first symbol */
		if (*format != '%') {
single_print:
			++pc;
			ret = printchar_handler(printchar_data, *format);
			if (ret < 0) {
				return ret;
			}
			continue;
		}

		ops = 0;

		/* get flags */
		while (*++format) {
			switch (*format) {
			default:
				goto after_flags;
			case '-':
				ops |= OPS_FLAG_LEFT_ALIGN;
				break;
			case '+':
				ops |= OPS_FLAG_WITH_SIGN;
				break;
			case ' ':
				ops |= OPS_FLAG_EXTRA_SPACE;
				break;
			case '#':
				ops |= OPS_FLAG_WITH_SPEC;
				break;
			case '0':
				ops |= OPS_FLAG_ZERO_PAD;
				break;
			}
		}
after_flags:

		/* get width */
		if (*format == '*') {
			width = va_arg(args, int);
			++format;
		}
		else {
			width = atoi(format);
			while (isdigit(*format))
				++format;
		}
		width = max(width, 0);

		/* get precision */
		ops |= *format == '.' ? OPS_PREC_IS_GIVEN : 0;
		if ((*format == '.') && (*++format == '*')) {
			precision = va_arg(args, int);
			++format;
		}
		else {
			precision = atoi(format);
			while (isdigit(*format))
				++format;
		}
		precision = precision >= 0 ? precision : (ops &= ~OPS_PREC_IS_GIVEN, 0);

		/* get length */
		switch (*format) {
		case 'h':
			ops |= *++format != 'h' ? OPS_LEN_SHORT : (++format, OPS_LEN_MIN);
			break;
		case 'l':
			ops |= *++format != 'l' ? OPS_LEN_LONG
			                        : (++format, OPS_LEN_LONGLONG);
			break;
		case 'j':
			ops |= OPS_LEN_MAX;
			++format;
			break;
		case 'z':
			ops |= OPS_LEN_SIZE;
			++format;
			break;
		case 't':
			ops |= OPS_LEN_PTRDIFF;
			++format;
			break;
		case 'L':
			ops |= OPS_LEN_LONGFP;
			++format;
			break;
		}

		/* handle specifier */
		ops |= isupper(*format) ? OPS_SPEC_UPPER_CASE : 0;
		switch (*format) {
		default:
			pc += format - begin + 1;
			do {
				ret = printchar_handler(printchar_data, *begin);
				if (ret < 0) {
					return ret;
				}
			} while (++begin <= format);
			break;
		case '%':
			goto single_print;
		case 'd':
		case 'i':
			tmp.ulli = ops & OPS_LEN_MIN        ? (signed char)va_arg(args, int)
			           : ops & OPS_LEN_SHORT    ? (short int)va_arg(args, int)
			           : ops & OPS_LEN_LONG     ? va_arg(args, long int)
			           : ops & OPS_LEN_LONGLONG ? va_arg(args, long long int)
			           : ops & OPS_LEN_MAX      ? va_arg(args, intmax_t)
			           : ops & OPS_LEN_SIZE     ? va_arg(args, ssize_t)
			           : ops & OPS_LEN_PTRDIFF  ? va_arg(args, ptrdiff_t)
			                                    : va_arg(args, int);
			ret = print_i(printchar_handler, printchar_data, tmp.ulli, 1, width,
			    precision, ops, 10);
			if (ret < 0) {
				return ret;
			}
			pc += ret;
			break;
		case 'u':
		case 'o':
		case 'x':
		case 'X':
			tmp.ulli = ops & OPS_LEN_MIN
			               ? (unsigned char)va_arg(args, unsigned int)
			           : ops & OPS_LEN_SHORT
			               ? (unsigned short int)va_arg(args, unsigned int)
			           : ops & OPS_LEN_LONG ? va_arg(args, unsigned long int)
			           : ops & OPS_LEN_LONGLONG
			               ? va_arg(args, unsigned long long int)
			           : ops & OPS_LEN_MAX     ? va_arg(args, uintmax_t)
			           : ops & OPS_LEN_SIZE    ? va_arg(args, size_t)
			           : ops & OPS_LEN_PTRDIFF ? va_arg(args, ptrdiff_t)
			                                   : va_arg(args, unsigned int);
			ret = print_i(printchar_handler, printchar_data, tmp.ulli, 0, width,
			    precision, ops,
			    *format == 'u' ? 10 : (*format == 'o' ? 8 : 16));
			if (ret < 0) {
				return ret;
			}
			pc += ret;
			break;
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A':
			tmp.ld = ops & OPS_LEN_LONGFP ? va_arg(args, long double)
			                              : va_arg(args, double);
			ret = print_f(printchar_handler, printchar_data, tmp.ld, width,
			    precision, ops, *format);
			if (ret < 0) {
				return ret;
			}
			pc += ret;
			break;
		case 'c':
			/* TODO handle (ops & OPS_LEN_LONG) for wint_t */
			tmp.ca[0] = (char)va_arg(args, int);
			tmp.ca[1] = '\0';
			ret = print_s(printchar_handler, printchar_data, &tmp.ca[0], width,
			    precision, ops);
			if (ret < 0) {
				return ret;
			}
			pc += ret;
			break;
		case 's':
			/* TODO handle (ops & OPS_LEN_LONG) for wchar_t* */
			tmp.cp = va_arg(args, char *);
			ret = print_s(printchar_handler, printchar_data,
			    tmp.cp ? tmp.cp : PRINT_S_NULL_STR, width, precision, ops);
			if (ret < 0) {
				return ret;
			}
			pc += ret;
			break;
		case 'p':
			/* As glibc prints it: "0x" and the digits, "(nil)" for NULL. */
			tmp.vp = va_arg(args, void *);
			if (tmp.vp == NULL) {
				ret = print_s(printchar_handler, printchar_data, "(nil)", width,
				    0, ops & OPS_FLAG_LEFT_ALIGN);
			}
			else {
				ret = print_i(printchar_handler, printchar_data,
				    (uintptr_t)tmp.vp, 0, width, precision,
				    ops | OPS_FLAG_WITH_SPEC, 16);
			}
			if (ret < 0) {
				return ret;
			}
			pc += ret;
			break;
		case 'n':
			if (ops & OPS_LEN_MIN)
				*va_arg(args, signed char *) = (signed char)pc;
			else if (ops & OPS_LEN_SHORT)
				*va_arg(args, short int *) = (short int)pc;
			else if (ops & OPS_LEN_LONG)
				*va_arg(args, long int *) = (long int)pc;
			else if (ops & OPS_LEN_LONGLONG)
				*va_arg(args, long long int *) = (long long int)pc;
			else if (ops & OPS_LEN_MAX)
				*va_arg(args, intmax_t *) = (intmax_t)pc;
			else if (ops & OPS_LEN_SIZE)
				*va_arg(args, size_t *) = (size_t)pc;
			else if (ops & OPS_LEN_PTRDIFF)
				*va_arg(args, ptrdiff_t *) = (ptrdiff_t)pc;
			else
				*va_arg(args, int *) = pc;
			break;
		}
	}

	return pc;
}
