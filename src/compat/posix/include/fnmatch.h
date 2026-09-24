/**
 * @file
 * @brief fnmatch.h
 *
 * @date 20.12.18
 * @author Filipp Chubukov
 */
#ifndef POSIX_FNMATCH_H_
#define POSIX_FNMATCH_H_

#include <sys/cdefs.h>

#define FNM_NOMATCH 3
#define FNM_PATHNAME 1

__BEGIN_DECLS

/* In a C++ unit this was a declaration of a C++ function: the reference got
 * mangled and never reached the definition here. */
extern int fnmatch(const char *pattern, const char *string, int flags);

__END_DECLS
#endif /* POSIX_FNMATCH_H_ */
