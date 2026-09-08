/**
 * @file
 * @brief Inter-processor interrupts on aarch64.
 *
 */
#ifndef AARCH64_SMP_H_
#define AARCH64_SMP_H_

#ifndef __ASSEMBLER__

#include <sys/cdefs.h>

/**
 * The SGI that carries "come and reschedule".
 *
 * Software-generated interrupts are the only ones a core can raise on another
 * core, and which of the sixteen to use is ours to pick -- nothing else in the
 * image claims one. Zero, because Linux has used it for the same message since
 * before GICv3, and a kernel developer reading a GIC trace will read it right.
 */
#define AARCH64_SGI_RESCHED 0

/**
 * The SGI that means "stop what you are doing and say what it was".
 *
 * Sent by whichever core is on its way into platform_shutdown() with an abort:
 * a panic or a failed assertion. Without it the other three keep running while
 * the diagnosis is being printed -- overwriting the log, taking further faults,
 * and in the worst case scribbling over the very state that explains the first
 * one.
 */
#define AARCH64_SGI_STOP    1

__BEGIN_DECLS

/**
 * Raise the reschedule IPI on another CPU. The scheduler's own entry point --
 * sched.c and sched_ticker.c declare this themselves and call it under SMP.
 */
extern void smp_send_resched(int cpu_id);

/**
 * How many reschedule IPIs a CPU has taken since it came up.
 *
 * Per-CPU and monotonic, which is what makes it a measurement rather than a
 * flag: a caller compares it against itself over an interval. Used by the
 * `smptest` command; also the cheapest evidence that a core is alive and
 * taking interrupts, which nothing else on a parked core can tell you.
 */
extern unsigned long aarch64_smp_ipi_count(unsigned int cpu_id);

/**
 * Halt every other CPU and print what each was doing.
 *
 * Called from the abort path, so it is written to be survivable rather than
 * correct: every wait is bounded, a CPU that does not answer is reported as
 * not answering, and nothing here takes a lock that a stopped CPU might be
 * holding. Runs once -- a second caller (another core racing into the same
 * panic) returns immediately.
 *
 * Declared in <hal/cpu.h> as well, which is where generic code reaches it.
 */
extern void smp_stop_others(void);

/** Print what smp_stop_others() collected. Prints once, however often called. */
extern void smp_print_stopped(void);

/**
 * Report and park. Called from the interrupt entry when AARCH64_SGI_STOP
 * arrives, *before* the handler takes any lock, and never returns.
 *
 * Takes the interrupted context as four numbers rather than a struct so that
 * the SMP module does not have to include a header from the exceptions module
 * for four fields.
 */
extern void aarch64_smp_stop_self(unsigned long pc, unsigned long lr,
    unsigned long sp, unsigned long psr);

__END_DECLS

#endif /* !__ASSEMBLER__ */

#endif /* AARCH64_SMP_H_ */
