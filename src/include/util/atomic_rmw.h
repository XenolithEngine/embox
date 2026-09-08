/**
 * @file
 * @brief Read-modify-write atomics that degrade instead of failing to link.
 *
 * The compiler turns a __atomic_* read-modify-write into an instruction where
 * the target has one, and into a call to libatomic where it does not.  Embox
 * links no libatomic, so the second case is a build that fails at the very end
 * with an undefined reference.  It is not hypothetical: x86 built for
 * -march=i386 is in the tree, and the 80386 has neither cmpxchg nor xadd.
 *
 * Where the operation is lock-free, it is used.  Where it is not, the fallback
 * is the plain read-modify-write -- which is what every one of these sites did
 * before an atomic was put there, so nothing is worse than it was: correct on
 * one core, and a machine that is both multiprocessor and without a lock-free
 * word-sized RMW is not one this kernel runs on.
 *
 * Loads and stores are here for the same reason.  "A word-sized load is one
 * instruction" is true of the instruction, not of __atomic_load_n(): an
 * ARMv5 has no ldrex, so clang reports a maximum lock-free size of zero bytes
 * there and turns even a four-byte acquire load into a call.  The fallback is
 * a volatile access with the barrier the order asked for.
 *
 * Fences are not here: __atomic_thread_fence() is a barrier instruction or
 * nothing at all, on every target.
 */

#ifndef UTIL_ATOMIC_RMW_H_
#define UTIL_ATOMIC_RMW_H_

#include <linux/compiler.h>

#if defined(__GCC_ATOMIC_INT_LOCK_FREE) && defined(__GCC_ATOMIC_LONG_LOCK_FREE) \
    && (__GCC_ATOMIC_INT_LOCK_FREE == 2) && (__GCC_ATOMIC_LONG_LOCK_FREE == 2)
#define ATOMIC_RMW_LOCK_FREE 1
#else
#define ATOMIC_RMW_LOCK_FREE 0
#endif

#if ATOMIC_RMW_LOCK_FREE

#define atomic_add_fetch(ptr, val, order) __atomic_add_fetch(ptr, val, order)
#define atomic_sub_fetch(ptr, val, order) __atomic_sub_fetch(ptr, val, order)
#define atomic_or_fetch(ptr, val, order)  __atomic_or_fetch(ptr, val, order)
#define atomic_and_fetch(ptr, val, order) __atomic_and_fetch(ptr, val, order)
#define atomic_exchange(ptr, val, order)  __atomic_exchange_n(ptr, val, order)
#define atomic_load(ptr, order)           __atomic_load_n(ptr, order)
#define atomic_store(ptr, val, order)     __atomic_store_n(ptr, val, order)

/** Acquire on success, relaxed on failure: a try that failed orders nothing. */
#define atomic_try_lock(ptr, unlocked, locked)                                \
	({                                                                        \
		__typeof__(*(ptr)) __expected = (unlocked);                           \
		__atomic_compare_exchange_n(ptr, &__expected, locked, 0,              \
		    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);                              \
	})

#else /* !ATOMIC_RMW_LOCK_FREE */

/* The barrier is what is left of the memory order: the compiler is stopped
 * from moving the access, and a machine that cannot run two of these at once
 * has nothing else to reorder against. */
#define atomic_add_fetch(ptr, val, order) \
	({ __barrier(); *(ptr) += (val); __barrier(); *(ptr); })
#define atomic_sub_fetch(ptr, val, order) \
	({ __barrier(); *(ptr) -= (val); __barrier(); *(ptr); })
#define atomic_or_fetch(ptr, val, order) \
	({ __barrier(); *(ptr) |= (val); __barrier(); *(ptr); })
#define atomic_and_fetch(ptr, val, order) \
	({ __barrier(); *(ptr) &= (val); __barrier(); *(ptr); })
#define atomic_exchange(ptr, val, order)                                      \
	({                                                                        \
		__typeof__(*(ptr)) __old;                                             \
		__barrier();                                                          \
		__old = *(ptr);                                                       \
		*(ptr) = (val);                                                       \
		__barrier();                                                          \
		__old;                                                                \
	})

/* Acquire on a load is "nothing after this may be hoisted above it", release
 * on a store is "nothing before it may sink below": one barrier each, on the
 * side the order names. */
#define atomic_load(ptr, order)                                               \
	({                                                                        \
		__typeof__(*(ptr)) __v = *(volatile __typeof__(*(ptr)) *)(ptr);        \
		__barrier();                                                          \
		__v;                                                                  \
	})
#define atomic_store(ptr, val, order)                                         \
	do {                                                                      \
		__barrier();                                                          \
		*(volatile __typeof__(*(ptr)) *)(ptr) = (val);                        \
	} while (0)

#define atomic_try_lock(ptr, unlocked, locked)                                \
	({                                                                        \
		int __got;                                                            \
		__barrier();                                                          \
		__got = (*(ptr) == (unlocked));                                       \
		if (__got) {                                                          \
			*(ptr) = (locked);                                                \
		}                                                                     \
		__barrier();                                                          \
		__got;                                                                \
	})

#endif /* ATOMIC_RMW_LOCK_FREE */

#endif /* UTIL_ATOMIC_RMW_H_ */
