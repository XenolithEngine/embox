/**
 * @file
 * @brief
 *
 * @date 27.10.11
 * @author Anton Kozlov
 * @author Anton Bondarev
 * @author Ilia Vaprol
 */
#include <util/log.h>

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <lib/libds/dlist.h>

#include <hal/ipl.h>
#include <net/netdevice.h>
#include <net/skbuff.h>
#include <net/l0/net_rx.h>
#include <kernel/sched/schedee_priority.h>
#include <kernel/lthread/lthread.h>

#define NETIF_RX_HND_PRIORITY OPTION_GET(NUMBER, hnd_priority)

/* Deliberate receive loss and reordering. See board/embox-qemu/patches/
 * netif_rx-drop-inject.py -- off unless mods.conf asks for them. */
#define NETIF_RX_DROP_ONE_IN    OPTION_GET(NUMBER, drop_one_in)
#define NETIF_RX_REORDER_ONE_IN OPTION_GET(NUMBER, reorder_one_in)
#define NETIF_RX_REORDER_DELAY  OPTION_GET(NUMBER, reorder_delay)

#if NETIF_RX_DROP_ONE_IN > 0
static unsigned long netif_rx_drop_state = 2463534242ul; /* fixed seed */
static unsigned long netif_rx_drop_count = 0;
static unsigned long netif_rx_drop_report = 1;

static int netif_rx_drop_one_in(void) {
	unsigned long x = netif_rx_drop_state;

	/* xorshift32: cheap, and the same sequence every boot, so two runs of
	 * the same image lose the same frames and can be compared. */
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	netif_rx_drop_state = x;

	if ((x % (unsigned long)NETIF_RX_DROP_ONE_IN) != 0) {
		return 0;
	}

	netif_rx_drop_count++;
	if (netif_rx_drop_count >= netif_rx_drop_report) {
		log_info("injected receive loss: %lu frames dropped (1 in %d)",
		    netif_rx_drop_count, NETIF_RX_DROP_ONE_IN);
		netif_rx_drop_report *= 2;
	}
	return 1;
}
#endif /* NETIF_RX_DROP_ONE_IN */

#if NETIF_RX_REORDER_ONE_IN > 0
static struct sk_buff *netif_rx_held = NULL;
static unsigned netif_rx_held_wait = 0;
static unsigned long netif_rx_reorder_state = 88675123ul; /* fixed seed */
static unsigned long netif_rx_reorder_count = 0;
static unsigned long netif_rx_reorder_report = 1;

static int netif_rx_reorder_pick(void) {
	unsigned long x = netif_rx_reorder_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	netif_rx_reorder_state = x;

	if ((x % (unsigned long)NETIF_RX_REORDER_ONE_IN) != 0) {
		return 0;
	}

	netif_rx_reorder_count++;
	if (netif_rx_reorder_count >= netif_rx_reorder_report) {
		log_info("injected reordering: %lu frames delayed by %d (1 in %d)",
		    netif_rx_reorder_count, NETIF_RX_REORDER_DELAY,
		    NETIF_RX_REORDER_ONE_IN);
		netif_rx_reorder_report *= 2;
	}
	return 1;
}
#endif /* NETIF_RX_REORDER_ONE_IN */

static DLIST_DEFINE(netif_rx_list);

static int netif_rx_action(struct lthread *self);
static LTHREAD_DEF(netif_rx_irq_handler, netif_rx_action, NETIF_RX_HND_PRIORITY);

static int netif_tx_action(struct lthread *self);
static LTHREAD_DEF(netif_tx_handler, netif_tx_action, NETIF_RX_HND_PRIORITY);

static int netif_rx_action(struct lthread *self) {
	struct net_device *dev = NULL;
	ipl_t ipl;

	ipl= ipl_save();
	{
		dlist_foreach_entry_safe(dev, &netif_rx_list, rx_lnk) {
			struct sk_buff *skb;

			while ((skb = skb_queue_pop(&dev->dev_queue)) != NULL) {
				ipl_restore(ipl);
				{
					net_rx(skb);
				}
				ipl= ipl_save();
			}
			dlist_del_init(&dev->rx_lnk);
		}
	}
	ipl_restore(ipl);

	return 0;
}

/* we can be in irq mode */
/* The normal hand-off, factored out so a held frame can take the same path
 * when it is finally released. */
static void netif_rx_enqueue(struct sk_buff *skb) {
	struct net_device *dev = skb->dev;
	ipl_t ipl;

	ipl = ipl_save();
	{
		skb_queue_push(&dev->dev_queue, skb);

		if (dlist_empty(&dev->rx_lnk)) {
			dlist_add_prev(&dev->rx_lnk, &netif_rx_list);
		}

		lthread_launch(&netif_rx_irq_handler);
	}
	ipl_restore(ipl);
}

int netif_rx(void *data) {
	struct sk_buff *skb = data;
	struct net_device *dev;

	assert(skb != NULL);
	assert(skb->dev != NULL);

	dev = skb->dev;
	(void)dev;

#if NETIF_RX_DROP_ONE_IN > 0
	if (netif_rx_drop_one_in()) {
		skb_free(skb);
		return NET_RX_DROP;
	}
#endif

#if NETIF_RX_REORDER_ONE_IN > 0
	if ((netif_rx_held == NULL) && netif_rx_reorder_pick()) {
		/* Step aside and let the next few frames past. */
		netif_rx_held = skb;
		netif_rx_held_wait = NETIF_RX_REORDER_DELAY;
		return NET_RX_SUCCESS;
	}

	netif_rx_enqueue(skb);

	if ((netif_rx_held != NULL) && (--netif_rx_held_wait == 0)) {
		struct sk_buff *release = netif_rx_held;

		netif_rx_held = NULL;
		netif_rx_enqueue(release);
	}

	return NET_RX_SUCCESS;
#else
	netif_rx_enqueue(skb);

	return NET_RX_SUCCESS;
#endif
}

static DLIST_DEFINE(netif_tx_list);

static struct lthread netif_tx_handler;

static int netif_tx_action(struct lthread *self) {
	struct net_device *dev = NULL;

	sched_lock();
	{
		dlist_foreach_entry_safe(dev, &netif_tx_list, tx_lnk) {
			struct sk_buff * skb;
			int ret;

			while ((skb = skb_queue_pop(&dev->dev_queue_tx)) != NULL) {
				assert(dev->drv_ops != NULL);
				assert(dev->drv_ops->xmit != NULL);
				ret = dev->drv_ops->xmit(dev, skb);
				if (ret != 0) {
					log_debug("xmit = %d", ret);
					skb_free(skb);
					dev->stats.tx_err++;
					continue;
				}

				dev->stats.tx_packets++;
				dev->stats.tx_bytes += skb->len;
			}

			dlist_del_init(&dev->tx_lnk);
		}
	}
	sched_unlock();


	return 0;
}

int netif_tx(struct net_device *dev,  struct sk_buff *skb) {
	sched_lock();
	{
		skb_queue_push(&dev->dev_queue_tx, skb);

		if (dlist_empty(&dev->tx_lnk)) {
			dlist_add_prev(&dev->tx_lnk, &netif_tx_list);
		}
		lthread_launch(&netif_tx_handler);
	}
	sched_unlock();

	return 0;
}
