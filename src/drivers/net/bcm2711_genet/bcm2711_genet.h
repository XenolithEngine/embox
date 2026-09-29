/**
 * @file
 * @brief What the BCM2711 GENET driver found and what it has done since.
 *
 * For a status page or a benchmark to show next to its own numbers: the
 * block's identity, the link as the PHY reports it, and the frame counters.
 * The driver writes it and nothing else should; readers take it as it is,
 * without a lock, since every field is a single word or byte.
 */

#ifndef DRIVERS_NET_BCM2711_GENET_H_
#define DRIVERS_NET_BCM2711_GENET_H_

#include <stdint.h>

struct bcm2711_genet_status {
	/* The block and the PHY */
	uintptr_t base;
	uint32_t rev;        /* SYS_REV_CTRL as read */
	uint8_t major;       /* decoded GENET generation */
	uint8_t minor;
	uint8_t probed;      /* the unit init ran */
	uint8_t enabled;     /* this machine is expected to have the block */
	uint8_t alive;       /* the revision register looks like a GENET */
	uint8_t mdio_ok;     /* a PHY answered */
	uint8_t phy_addr;
	uint32_t phy_id;     /* PHYSID1:PHYSID2 */

	uint8_t mac[6];      /* from the VideoCore OTP, or the fallback */
	uint8_t mac_ok;      /* the mailbox answered */
	uint32_t mac_bus_base; /* which VideoCore alias of the buffer it took */

	/* The link */
	uint8_t link_pending; /* negotiation still running, no answer yet */
	uint8_t link;        /* up now */
	uint8_t aneg_done;
	uint8_t full_duplex;
	uint16_t speed;      /* 10, 100 or 1000 */
	uint16_t bmsr;       /* PHY status, the second read: the state now */
	uint16_t bmsr_first; /* the first read: latched history since the last */
	uint16_t lpa;        /* link partner ability */
	uint16_t stat1000;   /* gigabit status */
	uint32_t aneg_ms;    /* how long the first negotiation took */
	uint16_t link_drops;   /* times the link went down after it was up */
	uint16_t link_returns; /* times it came back, or came up late */

	/* The receive path's configuration */
	uint8_t use_irq;     /* the interrupt carries traffic, not the poller */
	uint8_t promisc;
	uint32_t mdf_ctrl;
	uint32_t mdf_mask;   /* which MDF enable bits the register really has */

	/* Frames */
	uint32_t tx_sent;    /* handed to the ring */
	uint32_t tx_done;    /* acknowledged by the hardware */
	uint32_t tx_fail;
	uint32_t rx_packets;
	uint32_t rx_bytes;
	uint32_t rx_dropped; /* no skb to put them in */
	uint32_t rx_errors;  /* the descriptor said the frame was bad */
	uint32_t rx_frag;    /* not start-and-end in one descriptor */
	uint32_t rx_resync;  /* times the ring indices were found out of step */
	uint32_t rx_by_irq;  /* frames the interrupt took */
	uint32_t rx_by_poll; /* frames the backstop poller took */
	uint32_t irq_count;
	uint32_t irq_rx;
};

extern const struct bcm2711_genet_status *bcm2711_genet_status(void);

#endif /* DRIVERS_NET_BCM2711_GENET_H_ */
