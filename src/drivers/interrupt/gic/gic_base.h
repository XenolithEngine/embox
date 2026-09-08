/**
 * @file
 * @brief
 *
 * @author Aleksey Zhmulin
 * @date 20.10.23
 */
#ifndef DRIVERS_INTERRUPT_GIC_GIC_UTIL_H_
#define DRIVERS_INTERRUPT_GIC_GIC_UTIL_H_

#include <framework/mod/options.h>

#if OPTION_DEFINED(NUMBER, gicd_base)
#define GICD_BASE OPTION_GET(NUMBER, gicd_base)
#endif

#if OPTION_DEFINED(NUMBER, gicr_base)
#define GICR_BASE OPTION_GET(NUMBER, gicr_base)
#endif

#if OPTION_DEFINED(NUMBER, gicc_base)
#define GICC_BASE OPTION_GET(NUMBER, gicc_base)
#endif

/* Which GIC implementation this is, where the difference is a register that
 * only exists on one of them. Absent for a GICv1/v2 build, which has no
 * redistributors at all. */
#if OPTION_DEFINED(NUMBER, gic600_pwrr)
#define GIC600_PWRR OPTION_GET(NUMBER, gic600_pwrr)
#else
#define GIC600_PWRR 0
#endif

#endif /* DRIVERS_INTERRUPT_GIC_GIC_UTIL_H_ */
