/**
 * @file
 *
 * @date 21.10.13
 * @author Alexander Kalmuk
 */

#ifndef COMPAT_LIBC_WCHAR_H_
#define COMPAT_LIBC_WCHAR_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h> /* WCHAR_MAX and WCHAR_MIN */
#include <stdio.h>
#include <sys/cdefs.h>
#include <time.h>

/* The types as Linux and musl have them. wint_t was __WINT_TYPE__, which is
 * int for this compiler, and wctype_t an int where the others pass an
 * unsigned long -- in a 64-bit register, whose upper half AAPCS64 leaves
 * undefined for a 32-bit argument. mbstate_t stays an int: every libc here
 * keeps its multibyte state in the first four bytes. */
#define WEOF 0xffffffffU

typedef unsigned int wint_t;
typedef unsigned long wctype_t;
typedef int mbstate_t;

__BEGIN_DECLS

extern size_t wcslen(const wchar_t *s);
extern int wcscmp(const wchar_t *s1, const wchar_t *s2);
extern wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
extern int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n);
extern size_t wcslen(const wchar_t *s);
extern size_t wcsnlen(const wchar_t *s, size_t maxlen);
extern wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
extern wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n);
extern wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
extern wchar_t *wmemset(wchar_t *wcs, wchar_t wc, size_t n);

extern int swprintf(wchar_t *s, size_t n, const wchar_t *format, ...);
extern int vswprintf(wchar_t *wcs, size_t maxlen, const wchar_t *format,
    va_list args);

extern long wcstol(const wchar_t *nptr, wchar_t **endptr, int base);
extern long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base);
extern unsigned long wcstoul(const wchar_t *nptr, wchar_t **endptr, int base);
extern unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr,
    int base);

extern float wcstof(const wchar_t *str, wchar_t **str_end);
extern double wcstod(const wchar_t *str, wchar_t **str_end);
extern long double wcstold(const wchar_t *str, wchar_t **endptr);

extern int wctob(wint_t);
extern wint_t btowc(int);
extern wint_t getwc(FILE *);
extern wint_t ungetwc(wint_t, FILE *);
extern wint_t putwc(wchar_t, FILE *);
extern size_t wcrtomb(char *, wchar_t, mbstate_t *);
extern size_t mbrtowc(wchar_t *, const char *, size_t, mbstate_t *);
extern int wcscoll(const wchar_t *, const wchar_t *);
extern size_t wcsxfrm(wchar_t *, const wchar_t *, size_t);
extern size_t wcsftime(wchar_t *, size_t, const wchar_t *, const struct tm *);

extern int wcsncasecmp(const wchar_t *ws1, const wchar_t *ws2, size_t n);
extern int wcscasecmp(const wchar_t *ws1, const wchar_t *ws2);

/* The rest of the wide API, declared here whichever implementation provides
 * it (it used to come from the wchar_extended module's header, so a build
 * without that module did not declare it at all). */
extern wint_t getwchar(void);
extern wint_t fgetwc(FILE *stream);
extern wchar_t *fgetws(wchar_t *ws, int n, FILE *stream);
extern wint_t fputwc(wchar_t wc, FILE *stream);
extern int fputws(const wchar_t *ws, FILE *stream);
extern wint_t putwchar(wchar_t wc);
extern int fwide(FILE *stream, int mode);
extern int wprintf(const wchar_t *format, ...);
extern int fwprintf(FILE *stream, const wchar_t *format, ...);
extern int vwprintf(const wchar_t *format, va_list arg);
extern int vfwprintf(FILE *stream, const wchar_t *format, va_list arg);
extern int wscanf(const wchar_t *format, ...);
extern int fwscanf(FILE *stream, const wchar_t *format, ...);
extern int swscanf(const wchar_t *s, const wchar_t *format, ...);
extern int vwscanf(const wchar_t *format, va_list arg);
extern int vfwscanf(FILE *stream, const wchar_t *format, va_list arg);
extern int vswscanf(const wchar_t *s, const wchar_t *format, va_list arg);
extern int mbsinit(const mbstate_t *st);
extern size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
extern size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
extern size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);
extern size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len,
    mbstate_t *ps);
extern size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
    mbstate_t *ps);
extern wchar_t *wcscat(wchar_t *ws1, const wchar_t *ws2);
extern wchar_t *wcschr(const wchar_t *ws, wchar_t wc);
extern wchar_t *wcscpy(wchar_t *ws1, const wchar_t *ws2);
extern size_t wcscspn(const wchar_t *ws1, const wchar_t *ws2);
extern wchar_t *wcsncat(wchar_t *ws1, const wchar_t *ws2, size_t n);
extern int wcsncmp(const wchar_t *ws1, const wchar_t *ws2, size_t n);
extern wchar_t *wcspbrk(const wchar_t *ws1, const wchar_t *ws2);
extern wchar_t *wcsrchr(const wchar_t *ws, wchar_t wc);
extern size_t wcsspn(const wchar_t *ws1, const wchar_t *ws2);
extern wchar_t *wcsstr(const wchar_t *ws1, const wchar_t *ws2);
extern wchar_t *wcstok(wchar_t *ws1, const wchar_t *ws2, wchar_t **ptr);
extern wchar_t *wcsdup(const wchar_t *ws);
extern wchar_t *wcpcpy(wchar_t *dst, const wchar_t *src);
extern wchar_t *wcpncpy(wchar_t *dst, const wchar_t *src, size_t n);
extern int wcwidth(wchar_t wc);
extern int wcswidth(const wchar_t *ws, size_t n);

__END_DECLS

#endif /* COMPAT_LIBC_WCHAR_H_ */
