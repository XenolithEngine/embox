// SPDX-License-Identifier: GPL-2.0+
/*
 * Internal API between the ported U-Boot xHCI core and the Embox glue
 * (xhci_hcd.c). The functions below are U-Boot statics that the glue needs;
 * they were de-static'd in the port and declared here.
 */

#ifndef XHCI_PORT_INT_H_
#define XHCI_PORT_INT_H_

#include "xhci_compat.h"
#include "xhci_uboot.h"

int xhci_reset(struct xhci_hcor *hcor);
int xhci_start(struct xhci_hcor *hcor);
int xhci_lowlevel_init(struct xhci_ctrl *ctrl);
int xhci_lowlevel_stop(struct xhci_ctrl *ctrl);
int xhci_submit_root(struct usb_device *udev, unsigned long pipe,
		     void *buffer, struct devrequest *setup);
int xhci_address_device(struct usb_device *udev, int root_portnr);
int xhci_set_configuration(struct usb_device *udev);
int _xhci_alloc_device(struct usb_device *udev);

/* xhci-dwc3.c */
struct dwc3;
void dwc3_set_mode(struct dwc3 *dwc3_reg, u32 mode);
int dwc3_core_init(struct dwc3 *dwc3_reg);
void dwc3_set_fladj(struct dwc3 *dwc3_reg, u32 val);

#endif /* XHCI_PORT_INT_H_ */
