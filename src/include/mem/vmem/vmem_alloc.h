/**
 * @file
 * @brief
 *
 * @date 24.09.2012
 * @author Anton Bulychev
 */

#ifndef VMEM_ALLOC_H_
#define VMEM_ALLOC_H_

#include <stdint.h>

extern uintptr_t *vmem_alloc_table(int lvl);
extern void vmem_free_table(int lvl, uintptr_t *table);

/* Translation tables the allocator still has to give, or -1 if it cannot
 * tell. For accounting: a count that falls from one program to the next is a
 * leak (A1, docs/EMBOX-USER-WM.md A4). */
extern long vmem_tables_free(void);

#endif /* VMEM_ALLOC_H_ */
