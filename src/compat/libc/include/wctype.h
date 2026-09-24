/**
 * @file
 * @brief wide-character classification and mapping utilities
 * @details TODO: Now wchar set is a char set only (see ctype.h). Any wchar greater than 255
 * does not belong to any charclass and will be recognized as invalid.
 * @date 21.10.13
 * @author Alexander Kalmuk
 */

#ifndef COMPAT_LIBC_WCTYPE_H_
#define COMPAT_LIBC_WCTYPE_H_

#include <wchar.h>
#include <ctype.h>

/* As Linux and musl have it (it was an int). */
typedef const int *wctrans_t;

#include <sys/cdefs.h>

__BEGIN_DECLS

extern wint_t towlower(wint_t wc);
extern wint_t towupper(wint_t wc);

extern int iswctype(wint_t, wctype_t);
extern wint_t towctrans(wint_t, wctrans_t);

extern wctype_t wctype(const char *property);
extern wctrans_t wctrans(const char *charclass);

/* Functions, not the static inlines they were: an inline answers only below
 * 0x100 and leaves no symbol for a libc that would answer for the rest. The
 * native implementation defines them in wchar/wctype.c. */
extern int iswalnum(wint_t wc);
extern int iswalpha(wint_t wc);
extern int iswblank(wint_t wc);
extern int iswcntrl(wint_t wc);
extern int iswdigit(wint_t wc);
extern int iswgraph(wint_t wc);
extern int iswlower(wint_t wc);
extern int iswprint(wint_t wc);
extern int iswpunct(wint_t wc);
extern int iswspace(wint_t wc);
extern int iswupper(wint_t wc);
extern int iswxdigit(wint_t wc);

__END_DECLS

#endif /* COMPAT_LIBC_WCTYPE_H_ */
