/**
 * @file
 * @brief CPU definitions
 *
 * @date 24.07.13
 * @author Ilia Vaprol
 */

#ifndef HAL_CPU_H_
#define HAL_CPU_H_

#include <framework/mod/options.h>

#include <module/embox/arch/cpu.h>
#include <module/embox/arch/smp.h>

#include <module/embox/kernel/stack.h>

#define NCPU OPTION_MODULE_GET(embox__arch__cpu, NUMBER, cpu_count)

#define KERNEL_AP_STACK_SZ OPTION_MODULE_GET(embox__kernel__stack, NUMBER, ap_stack_size)
#define KERNEL_BSP_STACK_SZ OPTION_MODULE_GET(embox__kernel__stack, NUMBER, stack_size)

#ifndef NOSMP
#define SMP
#endif /* NOSMP */

#ifndef __ASSEMBLER__

#include <sys/cdefs.h>

__BEGIN_DECLS

/**
 * @brief Get current CPU id.
 */
extern unsigned int cpu_get_id(void);

#ifdef SMP
/**
 * Halt every CPU but this one, and record what each was doing.  Called from
 * the abort path; safe to call more than once and from more than one CPU.
 *
 * Weak, and only some architectures provide it: a port that has not
 * implemented stopping its cores links and behaves exactly as it did before,
 * which is what every caller has to be ready for anyway -- a core that has
 * already parked in an earlier fault cannot answer either.
 */
extern void smp_stop_others(void) __attribute__((weak));

/** Print what the stopped CPUs reported. Prints once. Weak, as above. */
extern void smp_print_stopped(void) __attribute__((weak));
#endif /* SMP */

__END_DECLS

#endif /* !__ASSEMBLER__ */

#endif /* HAL_CPU_H_ */
