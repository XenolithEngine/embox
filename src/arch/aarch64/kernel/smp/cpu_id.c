/****************************************************************************
 * Which core am I, and which MPIDR is core N? (aarch64)
 *
 *
 * cpu_get_id() is not a rare call. cpudata_var() goes through it, and
 * __critical_count is a cpudata variable, so every critical_enter() and every
 * critical_leave() asks -- that is every interrupt, every sched_lock, and
 * every malloc. Searching an MPIDR table there, the way the RISC-V port
 * searches its hart map, would put a loop in the hottest path in the kernel.
 *
 * So the logical id lives in TPIDR_EL1, which exists for exactly this: a
 * per-PE scratch register readable at EL1. One `mrs` per call, no memory
 * touched, nothing to get wrong under a lock. Two things make it safe to use:
 * Embox does not touch it anywhere (only ARM32's CP15 counterparts appear in
 * the tree), and it is not TPIDR_EL0, which is where an application's TLS
 * pointer lives. Its reset value is UNKNOWN, so the reset handler writes it
 * before anything can ask -- see the reset handler.
 *
 * The table is the other direction, logical id -> MPIDR, and it is not hot:
 * it names the target of a PSCI CPU_ON and of an IPI, both rare.
 ****************************************************************************/

#include <stdint.h>

#include <hal/cpu.h>

#include <aarch64/cpu_id.h>

/* TPIDR_EL1 accessors. Local to this file: nothing else in the port reads
 * or writes the register, and the reset handler writes it in assembly. */
#define TPIDR_EL1_GET()                                             \
	({                                                              \
		uint64_t __v;                                               \
		__asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(__v));      \
		__v;                                                        \
	})

#define TPIDR_EL1_SET(v) \
	__asm__ __volatile__("msr tpidr_el1, %0" : : "r"((uint64_t)(v)))

#define MPIDR_EL1_GET()                                             \
	({                                                              \
		uint64_t __v;                                               \
		__asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(__v));      \
		__v;                                                        \
	})

/* Not __cpudata__: this is the map between the CPUs, so every core reads the
 * same copy. Written once per core while that core is the only one that can
 * write its slot. */
static uint64_t cpu_mpidr[NCPU];

uint64_t aarch64_mpidr(void) {
	return MPIDR_EL1_GET() & MPIDR_AFF_MASK;
}

unsigned int cpu_get_id(void) {
	return (unsigned int)TPIDR_EL1_GET();
}

void aarch64_cpu_id_register(unsigned int cpu_id) {
	if (cpu_id >= NCPU) {
		return;
	}

	TPIDR_EL1_SET(cpu_id);
	cpu_mpidr[cpu_id] = aarch64_mpidr();
}

uint64_t aarch64_cpu_mpidr(unsigned int cpu_id) {
	return (cpu_id < NCPU) ? cpu_mpidr[cpu_id] : 0;
}
