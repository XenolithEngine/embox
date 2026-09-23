/**
 * @file
 * @brief
 *
 * @author  Anton Kozlov
 * @date    09.08.2013
 */

#include <string.h>
#include <errno.h>

#include <util/err.h>
#include <util/log.h>
#include <lib/libds/indexator.h>
#include <lib/libds/dlist.h>
#include <lib/libds/ring_buff.h>
#include <lib/libds/array.h>

#include <kernel/irq.h>
#include <mem/misc/pool.h>
#include <drivers/serial/uart_dev.h>

ARRAY_SPREAD_DEF(struct uart *const, __uart_device_registry);

DLIST_DEFINE(uart_list);

struct dlist_head *uart_get_list(void) {
	return &uart_list;
}

static int uart_attach_irq(struct uart *uart) {
	int r;

	if (!(uart->params.uart_param_flags & UART_PARAM_FLAGS_USE_IRQ)) {
		return 0;
	}

	if (!uart->irq_handler) {
		return -EINVAL;
	}

	r = irq_attach(uart->irq_num, uart->irq_handler, 0, uart, uart->dev_name);

	log_debug("setup tty %s irq num %d result %d", uart->dev_name , uart->irq_num, r);

	return r;
}

static int uart_detach_irq(struct uart *uart) {

	if (uart->params.uart_param_flags & UART_PARAM_FLAGS_USE_IRQ) {
		return irq_detach(uart->irq_num, uart);
	}

	return 0;
}

static int uart_setup(struct uart *uart) {
	const struct uart_ops *uops = uart->uart_ops;

	if (uops->uart_setup) {
		log_debug("setup tty %s", uart->dev_name);
		return uops->uart_setup(uart, &uart->params);
	}

	return 0;
}

static void uart_internal_init(struct uart *uart) {
	if (uart_state_test(uart, UART_STATE_INITED)) {
		return;
	}

	uart_state_set(uart, UART_STATE_INITED);

	ring_buff_init(&uart->uart_rx_ring, sizeof(uart->uart_rx_buff[0]),
			UART_RX_BUFF_SZ, uart->uart_rx_buff);

	dlist_add_next(&uart->uart_lnk, &uart_list);
}

int uart_open(struct uart *uart) {
	const struct uart_ops *uops = uart->uart_ops;
	int ret;

	if (uart_state_test(uart, UART_STATE_OPEN)) {
		return -EINVAL;
	}
	uart_state_set(uart, UART_STATE_OPEN);

	uart_internal_init(uart);

	/* XENOLITH_SERIAL_CLEAR: the Zero 3E boot stall, solved 2026-09-23.
	 * The DW 16550's RX-timeout latch (IIR 0xCC, Character Timeout
	 * Indication) survives a boot with the irq line dead: U-Boot leaves
	 * the FIFOs enabled, and CTI -- the one 16550 source that reading
	 * LSR never acknowledges -- holds the level line high the moment
	 * uart_attach_irq() enables it at the GIC. The handler saw DR=0 and
	 * had nothing to acknowledge: millions of entries, nothing else
	 * scheduled, boot wedged between "runlevel is 3" and "Default IO
	 * device" until a real byte forced an RHR read. So: mask the source,
	 * run setup while the GIC line is still dead -- ns16550_setup resets
	 * both FIFOs (which also resets the RX timeout counter) and reads
	 * RHR/LSR/MSR before raising IER.DR -- then drain any stale bytes,
	 * and only then make the line live. A byte arriving between setup
	 * and attach costs one clean DR irq, which the handler acknowledges
	 * with its RHR reads. */
	if (uops->uart_irq_dis) {
		uops->uart_irq_dis(uart, &uart->params);
	}

	ret = uart_setup(uart);
	if (ret) {
		return ret;
	}

	if (uops->uart_hasrx) {
		while (uops->uart_hasrx(uart)) {
			uops->uart_getc(uart);
		}
	}

	return uart_attach_irq(uart);
}

int uart_close(struct uart *uart) {
	if (!uart_state_test(uart, UART_STATE_OPEN)) {
		return -EINVAL;
	}

	uart_state_clear(uart, UART_STATE_OPEN);

	return uart_detach_irq(uart);
}

int uart_set_params(struct uart *uart, const struct uart_params *params) {

	if (uart_state_test(uart, UART_STATE_OPEN)) {
		uart_detach_irq(uart);
	}

	memcpy(&uart->params, params, sizeof(struct uart_params));

	if (uart_state_test(uart, UART_STATE_OPEN)) {
		uart_attach_irq(uart);
		uart_setup(uart);
	}

	return 0;
}

int uart_get_params(struct uart *uart, struct uart_params *params) {

	memcpy(params, &uart->params, sizeof(struct uart_params));

	return 0;
}
