// SPDX-License-Identifier: GPL-2.0+
/*
 * DWC3 host bring-up for the xHCI port, from U-Boot 2026.07
 * drivers/usb/host/xhci-dwc3.c. The DM probe/remove plumbing is replaced
 * by xhci_hcd_init_dwc3() in xhci_hcd.c. The GCTL.CORESOFTRESET pulse
 * is done by the board overlay; this file must not mdelay() from
 * hcd_start.
 *
 * Original copyright:
 * Copyright 2015 Freescale Semiconductor, Inc.
 * Author: Ramneek Mehresh<ramneek.mehresh@freescale.com>
 */

#include "xhci_compat.h"
#include "dwc3_regs.h"
#include "xhci_uboot.h"

void dwc3_set_mode(struct dwc3 *dwc3_reg, u32 mode)
{
	clrsetbits_le32(&dwc3_reg->g_ctl,
			DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_OTG),
			DWC3_GCTL_PRTCAPDIR(mode));
}

void dwc3_core_soft_reset(struct dwc3 *dwc3_reg)
{
	/*
	 * The overlay already pulsed GCTL.CORESOFTRESET and set PRTCAP
	 * host (silicon readback 30c11004). Do not pulse again, and do
	 * not mdelay(): that ksleep is clock_gettime, and it used to die
	 * once the arena remap had uncached the monotonic itimer.
	 */
	(void)dwc3_reg;
	log_info("dwc3: soft-reset already done in overlay");
}

int dwc3_core_init(struct dwc3 *dwc3_reg)
{
	u32 reg;
	u32 revision;
	unsigned int dwc3_hwparams1;

	log_info("dwc3: core_init");
	revision = readl(&dwc3_reg->g_snpsid);
	log_info("dwc3: read GSNPSID=%08x", revision);
	/* This should read as U3 followed by revision number */
	if ((revision & DWC3_GSNPSID_MASK) != 0x55330000 &&
	    (revision & DWC3_GSNPSID_MASK) != 0x33310000) {
		log_error("dwc3: this is not a DesignWare USB3 DRD Core (GSNPSID=%08x)",
				revision);
		return -1;
	}

	dwc3_core_soft_reset(dwc3_reg);

	dwc3_hwparams1 = readl(&dwc3_reg->g_hwparams1);

	reg = readl(&dwc3_reg->g_ctl);
	reg &= ~DWC3_GCTL_SCALEDOWN_MASK;
	reg &= ~DWC3_GCTL_DISSCRAMBLE;
	switch (DWC3_GHWPARAMS1_EN_PWROPT(dwc3_hwparams1)) {
	case DWC3_GHWPARAMS1_EN_PWROPT_CLK:
		reg &= ~DWC3_GCTL_DSBLCLKGTNG;
		break;
	default:
		debug("No power optimization available\n");
	}

	/*
	 * WORKAROUND: DWC3 revisions <1.90a have a bug
	 * where the device can fail to connect at SuperSpeed
	 * and falls back to high-speed mode which causes
	 * the device to enter a Connect/Disconnect loop
	 */
	if ((revision & DWC3_REVISION_MASK) < 0x190a)
		reg |= DWC3_GCTL_U2RSTECN;

	writel(reg, &dwc3_reg->g_ctl);

	return 0;
}

void dwc3_set_fladj(struct dwc3 *dwc3_reg, u32 val)
{
	setbits_le32(&dwc3_reg->g_fladj, GFLADJ_30MHZ_REG_SEL |
			GFLADJ_30MHZ(val));
}
