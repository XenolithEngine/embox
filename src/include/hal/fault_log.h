/**
 * @file
 * @brief Hooks that save a fatal report where it outlives the machine
 *
 * A fatal exception or an abort prints its report and then stops for good,
 * often with interrupts masked and the other cores parked. On a board whose
 * console nobody is reading, the report is then only in RAM. These hooks let
 * a board write it somewhere that survives the reset. Both are weak: a board
 * that implements neither links and behaves exactly as without them.
 *
 * @date 23.09.2026
 */

#ifndef HAL_FAULT_LOG_H_
#define HAL_FAULT_LOG_H_

/**
 * Save the report from any context: an exception handler with interrupts
 * masked, or platform_shutdown() after smp_stop_others(). It must not sleep,
 * take a lock or allocate -- a lock may be held by a core that will never
 * run again.
 */
extern void fault_log_flush(void) __attribute__((weak));

/**
 * Save the report from thread context, where the filesystem may be used.
 * Called only where that is true, and always after fault_log_flush().
 */
extern void fault_log_flush_thread(void) __attribute__((weak));

#endif /* HAL_FAULT_LOG_H_ */
