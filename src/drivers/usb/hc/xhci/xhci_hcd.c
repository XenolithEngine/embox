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
#include <sys/mman.h>

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

static uint8_t dma_arena[DMA_ARENA_SIZE]
		__attribute__((aligned(64)));

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

/* Embox enum usb_speed -> U-Boot numeric values (see xhci_compat.h). */
static int shim_speed_of(struct usb_dev *dev) {
	switch (dev->speed) {
	case USB_SPEED_HIGH: return 2;
	case USB_SPEED_LOW:  return 1;
	case USB_SPEED_FULL: return 0;
	default:             return USB_SPEED_UNKNOWN;
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
	shim_owner[i] = dev;
	return shim;
}

/* Mirror Embox's parsed descriptors into the shim before a request that
 * walks them (SET_CONFIGURATION builds endpoint contexts from udev->ep). */
static void shim_sync_config(struct usb_device *shim, struct usb_dev *dev) {
	struct usb_dev_config *conf = dev->current_config;
	int epn = 0;

	shim->devnum = dev->addr;
	if (!conf) {
		return;
	}
	for (int i = 0; i < conf->usb_iface_num && epn < 30; i++) {
		struct usb_interface *iface = conf->usb_iface[i];
		if (!iface) {
			continue;
		}
		shim->maxchild = 0;
		for (int e = 0; e < iface->endp_n && epn < 30; e++, epn++) {
			struct usb_endp *endp = iface->endpoints[e];
			struct usb_endpoint_descriptor *desc;

			if (!endp) {
				continue;
			}
			/* U-Boot indexes ep[] by (endpoint number * 2 + dir). */
			desc = &shim->ep[(endp->address & 0xf) * 2
					+ (endp->direction == USB_DIRECTION_IN ? 1 : 0)];
			desc->bLength = sizeof(*desc);
			desc->bDescriptorType = USB_DT_ENDPOINT;
			desc->bEndpointAddress = endp->address;
			desc->bmAttributes = (uint8_t)endp->type;
			desc->wMaxPacketSize = endp->max_packet_size;
			desc->bInterval = endp->interval;
		}
	}
}

static void shim_sync(struct usb_device *shim, struct usb_dev *dev) {
	shim->devnum = dev->addr;
	shim->speed = shim_speed_of(dev);
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

static void *xhci_endp_alloc(struct usb_endp *endp) {
	/* Endpoint state is the per-slot eps[] array inside the ported
	 * core; nothing to hang on endp->hci_specific. */
	return NULL;
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

	/* Set dwc3 usb2 phy config: HS, 16-bit UTMI, no suspend. */
	reg = readl(&dwc3_reg->g_usb2phycfg[0]);
	reg &= ~DWC3_GUSB2PHYCFG_USBTRDTIM_MASK;
	reg |= DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT;
	reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
	reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	writel(reg, &dwc3_reg->g_usb2phycfg[0]);

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

	if (req->token & USB_TOKEN_IN) {
		dcache_inval(req->buffer, req->len);
	}
	ret = xhci_submit_root(shim, PIPE_CONTROL | PIPE_DIR_IN, req->buffer, &setup);
	req->req_stat = ret ? USB_REQ_INTERR : USB_REQ_NOERR;
	usb_request_complete(req);
	return 0;
}

static int xhci_request(struct usb_request *req) {
	struct usb_endp *endp = req->endp;
	struct usb_dev *dev = endp->dev;
	struct usb_device *shim = shim_of(dev);
	struct devrequest setup;
	void *buf = req->len ? (req->token & USB_TOKEN_IN ? (void *)req->buffer
	                                                  : (void *)req->buf)
	                     : NULL;
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
			if (ret) {
				break;
			}
		}

		if (ctrl->b_request == USB_REQ_SET_ADDRESS
				&& (ctrl->bm_request_type & USB_TYPE_MASK)
						== USB_TYPE_STANDARD) {
			ret = xhci_address_device(shim, shim->portnr);
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
		if (ctrl->b_request == USB_REQ_GET_DESCRIPTOR && req->len >= 8
				&& (ctrl->w_value >> 8) == USB_DT_DEVICE
				&& (ctrl->bm_request_type & USB_TYPE_MASK)
						== USB_TYPE_STANDARD) {
			/* Feed bMaxPacketSize0 back before the address step. */
			shim->ep[0].wMaxPacketSize =
					le16_to_cpu(((uint16_t *)req->buffer)[3]);
		}
		ret = xhci_ctrl_tx(shim, pipe, &setup, req->len, buf);
		break;
	case USB_COMM_BULK:
		if (req->token & USB_TOKEN_OUT) {
			dcache_flush(req->buf, req->len);
		} else {
			dcache_inval(req->buffer, req->len);
		}
		ret = xhci_bulk_tx(shim, pipe_of(endp, PIPE_BULK), req->len, buf);
		break;
	case USB_COMM_INTERRUPT:
		if (req->token & USB_TOKEN_OUT) {
			dcache_flush(req->buf, req->len);
		} else {
			dcache_inval(req->buffer, req->len);
		}
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
		xhci_dma.base = dma_arena;
		xhci_dma.size = DMA_ARENA_SIZE;
		xhci_dma.used = 0;
		start = (uintptr_t)dma_arena & ~(uintptr_t)MMU_PAGE_MASK;
		len = binalign_bound((uintptr_t)dma_arena + DMA_ARENA_SIZE,
				MMU_PAGE_SIZE) - start;
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

	if (irq) {
		ret = irq_attach(irq, xhci_irq, IF_SHARESUP, hcd, "xhci irq");
		if (ret) {
			return ret;
		}
	}

	return usb_hcd_register(hcd);
}
