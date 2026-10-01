// SPDX-License-Identifier: GPL-2.0+
/*
 * Embox glue for the U-Boot xHCI port: struct usb_hcd_ops, the DWC3 host
 * bring-up sequence from U-Boot's xhci-dwc3.c probe, and the translation
 * between Embox's usb_dev/usb_endp/request model and U-Boot's usb_device/
 * pipe model the ported core speaks.
 *
 * The request path is U-Boot's: synchronous, poll the event ring, complete
 * the request before returning. IRQ wiring is H3c work; the handler here
 * only acks and counts so a shared IRQ cannot storm the box.
 *
 * Single controller per image: the ported core keeps its state in one
 * struct xhci_ctrl, so the module does too.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include <hal/cache.h>

#include <drivers/common/memory.h>
#include <drivers/usb/usb.h>
#include <embox/unit.h>
#include <framework/mod/options.h>
#include <hal/mem_barriers.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/task/kernel_task.h>
#include <kernel/task/resource/mmap.h>
#include <mem/mmap.h>
#include <mem/vmem.h>
#include <util/binalign.h>

#include "xhci_compat.h"
#include "dwc3_regs.h"
#include "xhci_uboot.h"
#include "xhci_int.h"

#define DMA_ARENA_SIZE OPTION_GET(NUMBER, dma_arena_size)

static struct xhci_ctrl *xhci_ctrl;

/* ------------------------------------------------------------------ */
/* DMA arena                                                           */

struct xhci_dma_arena xhci_dma;

/* Page-aligned so the NOCACHE window below is this buffer and nothing
 * in front of it. aligned(64) put it at 0x2112d00; rounding the map
 * start down ate BSS page 0x2112000 (sys_timecounter, irq_table, the
 * kernel pgd). */
static uint8_t dma_arena[DMA_ARENA_SIZE]
		__attribute__((aligned(4096)));

void *xhci_dma_alloc(size_t size, size_t align) {
	uintptr_t p = binalign_bound((uintptr_t)xhci_dma.base + xhci_dma.used, align);

	if (p + size > (uintptr_t)xhci_dma.base + xhci_dma.size) {
		log_error("xhci: dma arena exhausted (%zu + %zu > %zu)",
				xhci_dma.used, size, xhci_dma.size);
		return NULL;
	}
	xhci_dma.used = p + size - (uintptr_t)xhci_dma.base;
	memset((void *)p, 0, size);
	return (void *)p;
}

void *xhci_dma_memalign(size_t align, size_t size) {
	return xhci_dma_alloc(size, align);
}

/* ------------------------------------------------------------------ */
/* usb_device shim registry                                            */

#define XHCI_SHIM_MAX 16

static struct usb_device shims[XHCI_SHIM_MAX];
static struct usb_dev *shim_owner[XHCI_SHIM_MAX];

/* Embox enum is HIGH=0 FULL=1 LOW=2. xhci_compat.h #defines the same
 * names to the U-Boot numbers, so this file must not use the names. */
static int shim_speed_of(struct usb_dev *dev) {
	switch ((int)dev->speed) {
	case 0: return 2; /* embox HIGH -> U-Boot HIGH */
	case 1: return 0; /* embox FULL -> U-Boot FULL */
	case 2: return 1; /* embox LOW  -> U-Boot LOW */
	default: return USB_SPEED_UNKNOWN;
	}
}

/* Hub enumeration on this tree was written for OHCI and stamps FS/LS.
 * The port status register already has the speed after reset. */
static void xhci_apply_port_speed(struct usb_device *shim) {
	uint32_t reg;
	int port = shim->portnr;

	if (!xhci_ctrl || !xhci_ctrl->hcor || port < 1) {
		return;
	}
	reg = xhci_readl(&xhci_ctrl->hcor->portregs[port - 1].or_portsc);
	switch (reg & DEV_SPEED_MASK) {
	case XDEV_LS:
		shim->speed = 1; /* U-Boot LOW */
		break;
	case XDEV_FS:
		shim->speed = 0; /* U-Boot FULL */
		break;
	case XDEV_HS:
		shim->speed = 2; /* U-Boot HIGH */
		break;
	case XDEV_SS:
		shim->speed = 3; /* U-Boot SUPER */
		break;
	default:
		break;
	}
}

static struct usb_device *shim_of(struct usb_dev *dev) {
	struct usb_device *shim;
	int i;

	for (i = 0; i < XHCI_SHIM_MAX; i++) {
		if (shim_owner[i] == dev) {
			return &shims[i];
		}
	}
	for (i = 0; i < XHCI_SHIM_MAX; i++) {
		if (shim_owner[i] == NULL) {
			break;
		}
	}
	if (i == XHCI_SHIM_MAX) {
		return NULL;
	}

	shim = &shims[i];
	memset(shim, 0, sizeof(*shim));
	shim->controller = xhci_ctrl;
	/* ep0 defaults by speed; re-read from the 8-byte descriptor once
	 * the device hands it over (xhci_check_maxpacket path). */
	shim->speed = shim_speed_of(dev);
	shim->ep[0].wMaxPacketSize = 64;
	/* check_maxpacket compares this, not ep[0]. Leave it 0 and a
	 * full-speed transfer Evaluate-Contexts EP0 down to 0 bytes. */
	shim->epmaxpacketin[0] = 64;
	shim_owner[i] = dev;
	return shim;
}

/* Embox's type enum is not the USB bmAttributes encoding. */
static uint8_t xhci_desc_attr(enum usb_comm_type type) {
	switch (type) {
	case USB_COMM_ISOCHRON:
		return USB_DESC_ENDP_TYPE_ISOCHR;
	case USB_COMM_BULK:
		return USB_DESC_ENDP_TYPE_BULK;
	case USB_COMM_INTERRUPT:
		return USB_DESC_ENDP_TYPE_INTR;
	default:
		return USB_DESC_ENDP_TYPE_CTRL;
	}
}

/* SET_CONFIGURATION builds endpoint contexts from config.if_desc.
 * Only the HID interface is copied: DualSense ifaces 0-2 are
 * isochronous audio, and a bad iso context fails the command for
 * every endpoint. The active-interface cap is 2, so HID is packed
 * into the front of if_desc regardless of its interface number. */
static void shim_sync_config(struct usb_device *shim, struct usb_dev *dev) {
	struct usb_dev_config *conf = dev->current_config;
	int nif = 0;
	int i;

	shim->devnum = dev->addr;
	shim->config.no_of_if = 0;
	for (i = 0; i < USB_MAXINTERFACES; i++) {
		shim->config.if_desc[i].no_of_ep = 0;
	}
	if (!conf) {
		return;
	}
	for (i = 0; i < conf->usb_iface_num && nif < USB_MAX_ACTIVE_INTERFACES; i++) {
		struct usb_interface *iface = conf->usb_iface[i];
		struct xhci_ub_interface *ud;
		int nep = 0;
		int e;

		if (!iface || !iface->iface_desc[0]) {
			continue;
		}
		if (iface->iface_desc[0]->b_interface_class != USB_CLASS_HID) {
			continue;
		}
		ud = &shim->config.if_desc[nif];
		memset(ud, 0, sizeof(*ud));
		/* endp_n is bNumEndpoints, but the parser stores the first
		 * endpoint at [1]. Walking only to endp_n drops the OUT
		 * at [2] (DualSense HID is IN ep4 + OUT ep3). */
		for (e = 0; e < USB_DEV_MAX_ENDP && nep < USB_MAXENDPOINTS; e++) {
			struct usb_endp *endp = iface->endpoints[e];
			struct usb_endpoint_descriptor *desc;
			unsigned epnum;
			int in;
			int idx;

			if (!endp || endp->type == USB_COMM_ISOCHRON) {
				continue;
			}
			epnum = endp->address & 0x0fu;
			in = endp->direction == USB_DIRECTION_IN;
			desc = &ud->ep_desc[nep];
			desc->bLength = sizeof(*desc);
			desc->bDescriptorType = USB_DT_ENDPOINT;
			desc->bEndpointAddress = (uint8_t)(epnum | (in ? USB_DIR_IN : 0));
			desc->bmAttributes = xhci_desc_attr(endp->type);
			desc->wMaxPacketSize = endp->max_packet_size;
			desc->bInterval = endp->interval;
			/* Same index usb_pipe_ep_index / xhci_get_ep_index use.
			 * IN is ep*2, OUT is ep*2-1. ep[] has 31 slots. */
			if (epnum == 0) {
				idx = 0;
			} else {
				idx = in ? (int)epnum * 2 : (int)epnum * 2 - 1;
			}
			if (idx < (int)(sizeof(shim->ep) / sizeof(shim->ep[0]))) {
				shim->ep[idx].wMaxPacketSize = desc->wMaxPacketSize;
			}
			nep++;
			log_info("xhci: cfg hid ep=%02x attr=%u mps=%u iv=%u",
					desc->bEndpointAddress, desc->bmAttributes,
					desc->wMaxPacketSize, desc->bInterval);
		}
		if (!nep) {
			continue;
		}
		ud->no_of_ep = (uint8_t)nep;
		ud->desc.bInterfaceNumber = iface->iface_desc[0]->b_interface_number;
		ud->desc.bNumEndpoints = (uint8_t)nep;
		ud->desc.bInterfaceClass = USB_CLASS_HID;
		nif++;
	}
	shim->config.no_of_if = (uint8_t)nif;
	log_info("xhci: cfg hid ifaces=%d", nif);
}

static void shim_sync(struct usb_device *shim, struct usb_dev *dev) {
	shim->devnum = dev->addr;
	/* Slot speed was taken from PORTSC at Address Device. The hub
	 * keeps the device at FULL afterwards (its own enum is 1). Writing
	 * that back makes the next ctrl_tx run the full-speed max-packet
	 * check against a high-speed slot. */
	if (!shim->addressed) {
		shim->speed = shim_speed_of(dev);
	}
}

/* ------------------------------------------------------------------ */
/* Pipe construction (U-Boot encoding, see xhci_compat.h)              */

static unsigned long pipe_of(struct usb_endp *endp, unsigned long type) {
	struct usb_device *shim = shim_of(endp->dev);
	unsigned long pipe = create_pipe(shim, endp->address & 0xf);

	pipe |= type;
	if (endp->direction == USB_DIRECTION_IN) {
		pipe |= PIPE_DIR_IN;
	}
	return pipe;
}

/* ------------------------------------------------------------------ */
/* usb_hcd_ops                                                         */

static void *xhci_hcd_alloc(struct usb_hcd *hcd, void *args) {
	/* struct xhci_ctrl embeds the DCBAA the hardware reads: it lives in
	 * the NOCACHE arena, not on a heap. */
	if (!xhci_dma.base) {
		return NULL;
	}
	xhci_ctrl = xhci_dma_alloc(sizeof(*xhci_ctrl), 64);
	xhci_ctrl->hccr = args;
	return xhci_ctrl;
}

static void xhci_hcd_free(struct usb_hcd *hcd, void *spec) {
	xhci_cleanup(xhci_ctrl);
	xhci_ctrl = NULL;
}

/* usb_get_ep0 treats a NULL hci_specific as allocation failure.
 * Endpoint state lives in the slot's eps[], so this is only a token. */
static int xhci_endp_cookie;

static void *xhci_endp_alloc(struct usb_endp *endp) {
	(void)endp;
	return &xhci_endp_cookie;
}

static void xhci_endp_free(struct usb_endp *endp, void *spec) {
}

static int xhci_hcd_start(struct usb_hcd *hcd) {
	struct xhci_hccr *hccr = xhci_ctrl->hccr;
	struct xhci_hcor *hcor;
	struct dwc3 *dwc3_reg;
	uint32_t reg;
	int ret;

	log_info("xhci: hccr=%p", hccr);

	/* U-Boot xhci_dwc3_probe sequence, minus the DT plumbing. */
	dwc3_reg = (struct dwc3 *)((char *)hccr + DWC3_REG_OFFSET);

	ret = dwc3_core_init(dwc3_reg);
	if (ret) {
		return ret;
	}
	log_info("xhci: GSNPSID=%08x", readl(&dwc3_reg->g_snpsid));

	/* usb@fd000000 is phy_type=utmi_wide. USBTRDTIM=5 with PHYIF
	 * left at 0 is an 8-bit UTMI and a 16-bit turnaround: the chirp
	 * still reports HS (PORTSC 0xe03), then SET_ADDRESS completes
	 * as COMP_TX_ERR. The inno PHY has no free-running clock, and
	 * the vendor node also clears LPM and USB2 suspend. */
	reg = readl(&dwc3_reg->g_usb2phycfg[0]);
	reg &= ~DWC3_GUSB2PHYCFG_USBTRDTIM_MASK;
	reg |= DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT;
	reg |= DWC3_GUSB2PHYCFG_PHYIF;
	reg &= ~DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS;
	reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
	reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	writel(reg, &dwc3_reg->g_usb2phycfg[0]);
	log_info("xhci: gusb2phycfg %08x", readl(&dwc3_reg->g_usb2phycfg[0]));

	/* snps,dis-del-phy-power-chg-quirk on the same node. */
	reg = readl(&dwc3_reg->g_usb3pipectl[0]);
	reg &= ~DWC3_GUSB3PIPECTL_DEPOCHANGE;
	writel(reg, &dwc3_reg->g_usb3pipectl[0]);
	log_info("xhci: gusb3pipectl %08x", readl(&dwc3_reg->g_usb3pipectl[0]));

	/* snps,dis-tx-ipgap-linecheck-quirk. */
	reg = readl(&dwc3_reg->g_uctl1);
	reg |= DWC3_GUCTL1_TX_IPGAP_LINECHECK_DIS;
	writel(reg, &dwc3_reg->g_uctl1);
	log_info("xhci: guctl1 %08x", readl(&dwc3_reg->g_uctl1));

	dwc3_set_mode(dwc3_reg, DWC3_GCTL_PRTCAP_HOST);

	/* U-Boot xhci_register(). */
	ret = xhci_reset((struct xhci_hcor *)((uintptr_t)hccr
			+ HC_LENGTH(xhci_readl(&hccr->cr_capbase))));
	if (ret) {
		return ret;
	}
	hcor = (struct xhci_hcor *)((uintptr_t)hccr
			+ HC_LENGTH(xhci_readl(&hccr->cr_capbase)));
	xhci_ctrl->hccr = hccr;
	xhci_ctrl->hcor = hcor;

	ret = xhci_lowlevel_init(xhci_ctrl);
	if (ret) {
		return ret;
	}

	/* _xhci_alloc_device returns without Enable Slot while rootdev
	 * is 0: U-Boot sets it from the root hub's SET_ADDRESS, which
	 * this stack never sends. The next device then enters ctrl_tx
	 * with slot_id 0 and faults on a NULL virt_dev (FAR 0x18). */
	xhci_ctrl->rootdev = 1;

	/* Root hub. */
	if (!usb_new_device(NULL, hcd, 0)) {
		log_error("xhci: usb_new_device(root) failed");
		return -1;
	}
	return 0;
}

static int xhci_hcd_stop(struct usb_hcd *hcd) {
	xhci_lowlevel_stop(xhci_ctrl);
	return 0;
}

static int xhci_root_hub_control(struct usb_request *req) {
	struct usb_control_header *ctrl = &req->ctrl_header;
	struct usb_device *shim = shim_of(req->endp->dev);
	struct devrequest setup;
	int ret;

	shim_sync(shim, req->endp->dev);
	shim->devnum = 1; /* root hub, whatever Embox numbered it */
	setup.requesttype = ctrl->bm_request_type;
	setup.request = ctrl->b_request;
	setup.value = ctrl->w_value;
	setup.index = ctrl->w_index;
	setup.length = ctrl->w_length;

	/* Caller data is req->buf. req->buffer[] is unused scratch, and
	 * submit_root memcpy's on the CPU, so do not invalidate the line. */
	ret = xhci_submit_root(shim, PIPE_CONTROL | PIPE_DIR_IN,
			req->len ? req->buf : NULL, &setup);
	req->req_stat = ret ? USB_REQ_INTERR : USB_REQ_NOERR;
	usb_request_complete(req);
	return 0;
}

/* Enable Slot posts a command TRB and waits for an event. A timeout
 * with a still-zero event TRB means the HC never wrote a completion
 * where we are polling. These registers say whether it fetched. */
static void xhci_dump_rings(void) {
	struct xhci_ctrl *ctrl = xhci_ctrl;
	uint64_t crcr;
	uint64_t erst;
	uint64_t erdp;
	union xhci_trb *evt;
	union xhci_trb *cmd;
	uint32_t ef[4];
	uint32_t cf[4];
	int i;

	if (!ctrl || !ctrl->hcor || !ctrl->hccr) {
		return;
	}
	crcr = xhci_readq(&ctrl->hcor->or_crcr);
	erst = ctrl->ir_set ? xhci_readq(&ctrl->ir_set->erst_base) : 0;
	erdp = ctrl->ir_set ? xhci_readq(&ctrl->ir_set->erst_dequeue) : 0;
	log_error("xhci: usbcmd=%08x usbsts=%08x crcr=%08x%08x",
			xhci_readl(&ctrl->hcor->or_usbcmd),
			xhci_readl(&ctrl->hcor->or_usbsts),
			(uint32_t)(crcr >> 32), (uint32_t)crcr);
	log_error("xhci: dboff=%08x rtsoff=%08x dba=%p erst=%08x%08x erdp=%08x%08x",
			xhci_readl(&ctrl->hccr->cr_dboff),
			xhci_readl(&ctrl->hccr->cr_rtsoff),
			ctrl->dba,
			(uint32_t)(erst >> 32), (uint32_t)erst,
			(uint32_t)(erdp >> 32), (uint32_t)erdp);
	evt = ctrl->event_ring ? ctrl->event_ring->dequeue : NULL;
	cmd = (ctrl->cmd_ring && ctrl->cmd_ring->first_seg)
			? ctrl->cmd_ring->first_seg->trbs : NULL;
	if (evt) {
		xhci_inval_cache((uintptr_t)evt, sizeof(*evt));
		for (i = 0; i < 4; i++) {
			__le32 raw = evt->generic.field[i];
			ef[i] = le32_to_cpu(raw);
		}
		log_error("xhci: evt %08x %08x %08x %08x",
				ef[0], ef[1], ef[2], ef[3]);
	}
	if (cmd && ctrl->cmd_ring->first_seg) {
		xhci_inval_cache((uintptr_t)cmd, sizeof(*cmd));
		for (i = 0; i < 4; i++) {
			__le32 raw = cmd->generic.field[i];
			cf[i] = le32_to_cpu(raw);
		}
		log_error("xhci: cmd %08x %08x %08x %08x dma=%08x",
				cf[0], cf[1], cf[2], cf[3],
				(uint32_t)ctrl->cmd_ring->first_seg->dma);
	}
}

static int xhci_request(struct usb_request *req) {
	struct usb_endp *endp = req->endp;
	struct usb_dev *dev = endp->dev;
	struct usb_device *shim = shim_of(dev);
	struct devrequest setup;
	/* Same split as the root hub: the caller pointer is req->buf.
	 * The ported core flushes and invalidates that pointer itself. */
	void *buf = (req->len && req->buf) ? req->buf : NULL;
	unsigned long pipe;
	int ret = 0;

	shim_sync(shim, dev);
	shim->portnr = dev->port + 1; /* Embox ports are 0-based here */

	switch (endp->type) {
	case USB_COMM_CONTROL:;
		struct usb_control_header *ctrl = &req->ctrl_header;

		setup.requesttype = ctrl->bm_request_type;
		setup.request = ctrl->b_request;
		setup.value = ctrl->w_value;
		setup.index = ctrl->w_index;
		setup.length = ctrl->w_length;
		pipe = create_pipe(shim, 0)
				| PIPE_CONTROL
				| (req->token & USB_TOKEN_IN ? PIPE_DIR_IN : 0);

		/* First control transfer ever: the slot does not exist yet.
		 * U-Boot's core allocates the device before enumeration;
		 * Embox's does not, so it happens on the first request. */
		if (shim->slot_id == 0 || !xhci_ctrl->devs[shim->slot_id]) {
			ret = _xhci_alloc_device(shim);
			if (ret || shim->slot_id == 0
					|| !xhci_ctrl->devs[shim->slot_id]) {
				log_error("xhci: enable slot failed ret=%d slot=%d",
						ret, shim->slot_id);
				xhci_dump_rings();
				ret = ret ? ret : -ENODEV;
				break;
			}
		}

		/* xHCI sends SET_ADDRESS inside Address Device. A control
		 * transfer before that faults or stalls, and a second
		 * Address Device kills the slot. Embox reads the
		 * descriptor first, so address on the first transfer
		 * and treat its later SET_ADDRESS as already done. */
		if (!shim->addressed) {
			xhci_apply_port_speed(shim);
			log_info("xhci: address port %d speed %d",
					shim->portnr, shim->speed);
			ret = xhci_address_device(shim, shim->portnr);
			if (ret) {
				log_error("xhci: address device failed ret=%d", ret);
				break;
			}
			shim->addressed = 1;
		}
		if (ctrl->b_request == USB_REQ_SET_ADDRESS
				&& (ctrl->bm_request_type & USB_TYPE_MASK)
						== USB_TYPE_STANDARD) {
			break;
		}
		if (ctrl->b_request == USB_REQ_SET_CONFIGURATION
				&& (ctrl->bm_request_type & USB_TYPE_MASK)
						== USB_TYPE_STANDARD) {
			shim_sync_config(shim, dev);
			ret = xhci_set_configuration(shim);
			if (ret) {
				break;
			}
		}
		ret = xhci_ctrl_tx(shim, pipe, &setup, req->len, buf);
		if (!ret && buf && req->len >= 8
				&& ctrl->b_request == USB_REQ_GET_DESCRIPTOR
				&& (ctrl->w_value >> 8) == USB_DT_DEVICE
				&& (ctrl->bm_request_type & USB_TYPE_MASK)
						== USB_TYPE_STANDARD) {
			/* Device descriptor byte 7 is bMaxPacketSize0. */
			shim->ep[0].wMaxPacketSize = ((uint8_t *)buf)[7];
			shim->epmaxpacketin[0] = ((uint8_t *)buf)[7];
		}
		break;
	case USB_COMM_BULK:
		ret = xhci_bulk_tx(shim, pipe_of(endp, PIPE_BULK), req->len, buf);
		break;
	case USB_COMM_INTERRUPT:
		ret = xhci_bulk_tx(shim, pipe_of(endp, PIPE_INTERRUPT), req->len, buf);
		break;
	default:
		return -ENOSYS;
	}

	req->req_stat = ret ? USB_REQ_INTERR : USB_REQ_NOERR;
	usb_request_complete(req);
	return 0;
}

static struct usb_hcd_ops xhci_hcd_ops = {
	.hcd_hci_alloc = xhci_hcd_alloc,
	.hcd_hci_free = xhci_hcd_free,
	.endp_hci_alloc = xhci_endp_alloc,
	.endp_hci_free = xhci_endp_free,
	.hcd_start = xhci_hcd_start,
	.hcd_stop = xhci_hcd_stop,
	.root_hub_control = xhci_root_hub_control,
	.request = xhci_request,
};

/* ------------------------------------------------------------------ */
/* IRQ: ack + count. Polling is the S3 path; H3c moves completions here. */

static irq_return_t xhci_irq(unsigned int irq_nr, void *data) {
	uint32_t status = xhci_readl(&xhci_ctrl->hcor->or_usbsts);

	if (status == ~(uint32_t)0) {
		return IRQ_HANDLED;
	}
	xhci_writel(&xhci_ctrl->hcor->or_usbsts, status & STS_EINT);
	return IRQ_HANDLED;
}

int xhci_hcd_register(uintptr_t base, unsigned int irq) {
	struct usb_hcd *hcd;
	uintptr_t start, len;
	int ret;

	if (xhci_dma.base == NULL) {
		uintptr_t arena = (uintptr_t)dma_arena;
		uintptr_t arena_end = arena + DMA_ARENA_SIZE;

		/* Round inward to whole pages. Rounding the start DOWN
		 * remaps the previous BSS page NOCACHE without a clean,
		 * so a later uncached read of sys_timecounter sees cs == 0
		 * and itimer_read_timespec asserts. */
		start = binalign_bound(arena, MMU_PAGE_SIZE);
		if (start >= arena_end) {
			log_error("xhci: dma arena is not a whole page");
			return -ENOMEM;
		}
		len = (arena_end & ~(uintptr_t)MMU_PAGE_MASK) - start;
		if (len < MMU_PAGE_SIZE) {
			log_error("xhci: dma arena is not a whole page");
			return -ENOMEM;
		}
		xhci_dma.base = (void *)start;
		xhci_dma.size = len;
		xhci_dma.used = 0;
		/* BSS clear is still dirty in the cache. Clean+invalidate
		 * before the pages stop being cacheable. */
		dcache_flush((void *)start, len);
		/* NOCACHE, like every DMA pool in this tree (the 5C lesson). */
		(void)mmap_place(task_resource_mmap(task_kernel_task()),
				start, len, PROT_READ | PROT_WRITE | PROT_NOCACHE);
		if (vmem_map_region(vmem_current_context(), start, start, len,
				PROT_READ | PROT_WRITE | PROT_NOCACHE)) {
			log_error("xhci: dma arena mapping failed");
			return -ENOMEM;
		}
	}

	hcd = usb_hcd_alloc(&xhci_hcd_ops, (void *)base);
	if (!hcd) {
		return -ENOMEM;
	}

	ret = usb_hcd_register(hcd);
	if (ret) {
		return ret;
	}

	/* After hcd_start. The handler reads hcor, which is NULL until
	 * xhci_reset returns. */
	if (irq) {
		ret = irq_attach(irq, xhci_irq, IF_SHARESUP, hcd, "xhci irq");
		if (ret) {
			return ret;
		}
	}

	return 0;
}
