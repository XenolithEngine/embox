/**
 * @file
 * @brief
 *
 * @date 12.08.2023
 * @author Denis Deryugin <deryugin.denis@gmail.com>
 */
#ifndef HAL_CACHE_H_
#define HAL_CACHE_H_

#include <stddef.h>

extern void dcache_inval(const void *data, size_t size);
extern void dcache_flush(const void *data, size_t size);

/* Make instructions just written to [DATA, DATA + SIZE) the ones every core
 * fetches, as a JIT needs. Only architectures that need it define it. */
extern void icache_sync_range(const void *data, size_t size);

#endif /* HAL_CACHE_H_ */
