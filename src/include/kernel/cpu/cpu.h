/**
 * @file
 * @brief
 *
 * @date 26.12.12
 * @author Anton Bulychev
 * @author Ilia Vaprol
 */

#ifndef KERNEL_CPU_CPU_H_
#define KERNEL_CPU_CPU_H_

#include <sys/types.h>

struct thread;

/**
 * Common CPU functions
 */
extern void cpu_init(unsigned int cpu_id, struct thread *idle);
extern void cpu_bind(unsigned int cpu_id, struct thread *t);

/* T may run on the cores in MASK (bit i = core i) that have started; 0, or
 * -EINVAL when none of them has. A thread narrowing its own mask leaves a
 * core it may no longer use at once. */
extern int thread_set_affinity(struct thread *t, int mask);
/* The cores T may run on, of those that have started. */
extern int thread_get_affinity(struct thread *t);
extern struct thread * cpu_get_idle(unsigned int cpu_id);
extern clock_t cpu_get_started(unsigned int cpu_id);

/**
 * CPU statistic
 */
extern clock_t cpu_get_total_time(unsigned int cpu_id);
extern clock_t cpu_get_idle_time(unsigned int cpu_id);

#endif /* !KERNEL_CPU_CPU_H_ */
