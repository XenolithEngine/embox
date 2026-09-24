/**
 * @file
 * @brief printf_impl's floating-point conversions (print_fp.c)
 */

#ifndef COMPAT_LIBC_STDIO_PRINT_FP_H_
#define COMPAT_LIBC_STDIO_PRINT_FP_H_

struct printchar_handler_data;

/* musl's flag bits, which fmt_fp takes as they are. */
#define ALT_FORM (1U << ('#' - ' '))
#define ZERO_PAD (1U << ('0' - ' '))
#define LEFT_ADJ (1U << ('-' - ' '))
#define PAD_POS  (1U << (' ' - ' '))
#define MARK_POS (1U << ('+' - ' '))

/* musl's state for an 'L' length modifier; anything else is a double. */
#define BIGLPRE 5

/* One conversion: `conv` is the letter (f F e E g G a A), `precision` -1 when
 * none was given. Returns the characters written, or the handler's error. */
extern int __print_fp(int (*handler)(struct printchar_handler_data *d, int c),
		struct printchar_handler_data *data, long double y, int width,
		int precision, unsigned int flags, int conv, int is_long_double);

#endif /* COMPAT_LIBC_STDIO_PRINT_FP_H_ */
