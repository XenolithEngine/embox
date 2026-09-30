/* This file is derived from newlib-1.19.0 arm port. */

#ifndef AARCH64_SETJMP_H_
#define AARCH64_SETJMP_H_

/**
 * All callee preserved registers:
 * SP, LR, x19-x28
 */
/* x19-x28, x29, x30, an unused word, SP, d8-d15: musl's aarch64 layout, so
 * an EL1 and an EL0 jmp_buf are the same (setjmp.S). The engine's libraries
 * take this size from the sysroot copy of this header: changing it means
 * rebuilding them against the new export. */
#define _JBLEN 22

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef	uint64_t __jmp_buf[_JBLEN];

#endif

#endif /*AARCH64_SETJMP_H_*/
