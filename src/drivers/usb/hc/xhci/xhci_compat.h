// SPDX-License-Identifier: GPL-2.0+
/*
 * U-Boot 2026.07 compatibility layer for the xHCI host port.
 *
 * The three ported files (xhci.c, xhci-ring.c, xhci-mem.c) and xhci_uboot.h
 * are kept close to their U-Boot shape so they can be diffed against
 * drivers/usb/host/ of v2026.07; everything U-Boot about them funnels into
 * this header. Embox-specific behavior (the DMA arena, the single-controller
 * assumption, the usb_device shim) is here, in one place.
 *
 * Literal port of U-Boot code: this file is GPL-2.0+ like the rest of the
 * module. The Embox USB core headers it sits beside stay BSD.
 */

#ifndef XHCI_UBOOT_COMPAT_H_
#define XHCI_UBOOT_COMPAT_H_

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hal/cache.h>
#include <kernel/printk.h>
#include <util/log.h>

/* U-Boot integer shorthand; the port targets a plain LP64 host. */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* Little-endian target: byteorder swaps are identities. */
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint64_t dma_addr_t;
typedef uintptr_t phys_addr_t;

/* Macros, not functions: the root-hub descriptor template initializes
 * compile-time constants through them, as in U-Boot. */
#define cpu_to_le16(x) ((uint16_t)(x))
#define cpu_to_le32(x) ((uint32_t)(x))
#define cpu_to_le64(x) ((uint64_t)(x))
#define le16_to_cpu(x) ((uint16_t)(x))
#define le32_to_cpu(x) ((uint32_t)(x))
#define le64_to_cpu(x) ((uint64_t)(x))

static inline uint32_t lower_32_bits(uint64_t v) { return (uint32_t)v; }
static inline uint32_t upper_32_bits(uint64_t v) { return (uint32_t)(v >> 32); }

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((uintptr_t)(a) - 1))

/* Register access: aarch64 LE, volatile dwords. */
static inline uint32_t readl(const volatile void *addr) {
	return *(const volatile uint32_t *)addr;
}

static inline void writel(uint32_t val, volatile void *addr) {
	*(volatile uint32_t *)addr = val;
}

/* U-Boot asm/io.h bit helpers over MMIO. */
static inline void setbits_le32(volatile void *addr, uint32_t mask) {
	writel(readl(addr) | mask, addr);
}

static inline void clrbits_le32(volatile void *addr, uint32_t mask) {
	writel(readl(addr) & ~mask, addr);
}

static inline void clrsetbits_le32(volatile void *addr, uint32_t clear, uint32_t set) {
	writel((readl(addr) & ~clear) | set, addr);
}

/* U-Boot console: routed to the Embox kernel console. */
#define printf(...)  printk(__VA_ARGS__)
#define puts(s)      printk("%s\n", (s))

#define BIT(nr)            (1UL << (nr))
#ifndef U32_MAX
#define U32_MAX            UINT32_MAX
#endif

/* Hub/port request classes and features (usb_defs.h). */
#define HUB_CLASS_REQ(dir, type, request) ((((dir) | (type)) << 8) | (request))
#ifndef USB_RT_HUB
#define USB_RT_HUB  (USB_TYPE_CLASS | USB_RECIP_DEVICE)
#endif
#ifndef USB_RT_PORT
#define USB_RT_PORT (USB_TYPE_CLASS | USB_RECIP_OTHER)
#endif

#define DeviceOutRequest \
	((USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE) << 8)

#define ClearHubFeature \
	HUB_CLASS_REQ(USB_DIR_OUT, USB_RT_HUB, USB_REQ_CLEAR_FEATURE)
#define ClearPortFeature \
	HUB_CLASS_REQ(USB_DIR_OUT, USB_RT_PORT, USB_REQ_CLEAR_FEATURE)
#define GetHubDescriptor \
	HUB_CLASS_REQ(USB_DIR_IN, USB_RT_HUB, USB_REQ_GET_DESCRIPTOR)
#define GetHubStatus \
	HUB_CLASS_REQ(USB_DIR_IN, USB_RT_HUB, USB_REQ_GET_STATUS)
#define GetPortStatus \
	HUB_CLASS_REQ(USB_DIR_IN, USB_RT_PORT, USB_REQ_GET_STATUS)
#define SetHubFeature \
	HUB_CLASS_REQ(USB_DIR_OUT, USB_RT_HUB, USB_REQ_SET_FEATURE)
#define SetPortFeature \
	HUB_CLASS_REQ(USB_DIR_OUT, USB_RT_PORT, USB_REQ_SET_FEATURE)

#define USB_PORT_FEAT_CONNECTION     0
#define USB_PORT_FEAT_ENABLE         1
#define USB_PORT_FEAT_SUSPEND        2
#define USB_PORT_FEAT_OVER_CURRENT   3
#define USB_PORT_FEAT_RESET          4
#define USB_PORT_FEAT_POWER          8
#define USB_PORT_FEAT_LOWSPEED       9
#define USB_PORT_FEAT_HIGHSPEED      10
#define USB_PORT_FEAT_C_CONNECTION   16
#define USB_PORT_FEAT_C_ENABLE       17
#define USB_PORT_FEAT_C_SUSPEND      18
#define USB_PORT_FEAT_C_OVER_CURRENT 19
#define USB_PORT_FEAT_C_RESET        20
#define USB_PORT_FEAT_TEST           21

#define USB_PORT_STAT_CONNECTION 0x0001
#define USB_PORT_STAT_ENABLE     0x0002
#define USB_PORT_STAT_SUSPEND    0x0004
#define USB_PORT_STAT_OVERCURRENT 0x0008
#define USB_PORT_STAT_RESET      0x0010
#define USB_PORT_STAT_POWER      0x0100
#define USB_PORT_STAT_LOW_SPEED  0x0200
#define USB_PORT_STAT_HIGH_SPEED 0x0400
#define USB_PORT_STAT_TEST       0x0800
#define USB_PORT_STAT_INDICATOR  0x1000
#define USB_PORT_STAT_SUPER_SPEED 0x0600 /* faking support to XHCI */

#define USB_PORT_STAT_C_CONNECTION  0x0001
#define USB_PORT_STAT_C_ENABLE      0x0002
#define USB_PORT_STAT_C_SUSPEND     0x0004
#define USB_PORT_STAT_C_OVERCURRENT 0x0008
#define USB_PORT_STAT_C_RESET       0x0010

#define USB_DT_SS_HUB (USB_TYPE_CLASS | 0x0a)

/* Endpoint direction bit (usb_defs.h). */
#define USB_DIR_OUT         0x00
#define USB_DIR_IN          0x80

static inline int fls(unsigned int x) {
	return x ? 32 - __builtin_clz(x) : 0;
}

#define clamp_val(val, lo, hi) \
	((val) < (lo) ? (lo) : ((val) > (hi) ? (hi) : (val)))

/* U-Boot include/linux/iopoll.h argument order: sleep_us, timeout_us.
 * Do not usleep. get_timer is clock_gettime(CLOCK_MONOTONIC); it reads
 * sys_timecounter, which used to share the arena's NOCACHE page and
 * assert in itimer_read_timespec. The spin cap is the backstop if the
 * clock reads but does not advance. */
#define readx_poll_sleep_timeout(op, addr, val, cond, sleep_us, timeout_us) \
({	uint64_t __end = get_timer(0) + (uint64_t)(timeout_us) / 1000 + 1; \
	unsigned __spins = 0; \
	int __ret = -ETIMEDOUT; \
	(void)(sleep_us); \
	for (;;) { \
		(val) = op(addr); \
		if (cond) { __ret = 0; break; } \
		if (get_timer(0) >= __end) break; \
		if (++__spins > 20000000u) break; \
	} \
	__ret; })
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
/* Control stays at ep*2 (the EP0 ring). Anything else matches
 * xhci_get_ep_index: IN is ep*2, OUT is ep*2-1. The old OR-1 formula
 * addressed an OUT endpoint one slot off, so its doorbell missed the
 * ring Configure Endpoint had built. */
#define usb_pipe_ep_index(p) \
	(usb_pipecontrol(p) ? (usb_pipeendpoint(p) << 1) : \
		((usb_pipeendpoint(p) << 1) - (usb_pipein(p) ? 0 : 1)))

/* U-Boot get_timer(base) is milliseconds since base. get_timer(0) is
 * the absolute tick, which readx_poll uses as a deadline. The event
 * wait does get_timer(start) < XHCI_TIMEOUT. Returning the absolute
 * tick made that compare false once the box had been up for 5s, so
 * Enable Slot was sampled once and timed out with the completion
 * already in the ring. */
#include <time.h>
static inline uint64_t get_timer(uint64_t base) {
	struct timespec ts;
	uint64_t ms;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
	return ms - base;
}

/* U-Boot yields inside long loops. A macro, not a function: Embox
 * sched.h already declares schedule(void). Must stay empty. The bulk
 * queue loop calls it before the doorbell, and a real yield there
 * stranded the pad thread: the shell got the CPU back and the endpoint
 * was never rung. The event wait is what sleeps (ksleep(1) while the
 * event is not ready), after the doorbell. */
#define schedule() do { } while (0)

/* U-Boot cacheline config: xhci-mem.c derives CACHELINE_SIZE from it. */
#define CONFIG_SYS_CACHELINE_SIZE 64

/* U-Boot transfer-status bits (usb_defs.h) used by record_transfer_result. */
#define USB_ST_ACTIVE      0x1
#define USB_ST_STALLED     0x2
#define USB_ST_BUF_ERR     0x4
#define USB_ST_BABBLE_DET  0x8
#define USB_ST_NAK_REC     0x10
#define USB_ST_CRC_ERR     0x20
#define USB_ST_BIT_ERR     0x40
#ifndef USB_ST_NOT_PROC
#define USB_ST_NOT_PROC    0x80000000L
#endif

/* U-Boot iommu plumbing: single identity-mapped region, nothing to map. */
static inline dma_addr_t xhci_dma_map(void *dev, void *addr, size_t len) {
	(void)dev; (void)len;
	return (dma_addr_t)(uintptr_t)addr;
}

/* U-Boot diagnostics. */
#define BUG_ON(x)   assert(!(x))
#define WARN_ON(x)  do { if (x) { log_warning("xhci: WARN_ON(" #x ")"); } } while (0)
#define BUG()       assert(0)

/* U-Boot logging: routed to the Embox module log. */
#define debug(...)      log_debug(__VA_ARGS__)
#define pr_err(...)     log_error(__VA_ARGS__)
#define pr_warn(...)    log_warning(__VA_ARGS__)
#define pr_info(...)    log_info(__VA_ARGS__)

#define mdelay(msec)   usleep((msec) * 1000)
#define udelay(usec)   usleep(usec)
#define mdelay_1(x)    mdelay(x)

/* No U-Boot watchdog in Embox. */
#define WATCHDOG_RESET() do { } while (0)

/* Strict-align build: unaligned access goes through memcpy. */
#define get_unaligned(p) \
	__extension__({ __typeof__(*(p)) __v; memcpy(&__v, (p), sizeof(__v)); __v; })
#define put_unaligned(v, p) \
	__extension__({ __typeof__(*(p)) __t = (v); memcpy((p), &__t, sizeof(__t)); })

/* Cache ops over request buffers (DMA structures live in the NOCACHE arena
 * and need nothing). */
static inline void flush_dcache_range(uintptr_t start, uintptr_t end) {
	if (end > start) {
		dcache_flush((const void *)start, end - start);
	}
}

static inline void invalidate_dcache_range(uintptr_t start, uintptr_t end) {
	if (end > start) {
		dcache_inval((void *)start, end - start);
	}
}

#define ARCH_DMA_MINALIGN 64

/* U-Boot iommu plumbing: single identity-mapped region, nothing to unmap. */
static inline void xhci_dma_unmap(void *dev, dma_addr_t addr, size_t len) {
	(void)dev; (void)addr; (void)len;
}

/* CONFIG_IS_ENABLED(DM_USB) is 0 here: the DM plumbing is not ported, the
 * Embox glue in xhci_hcd.c replaces it. */
#define CONFIG_IS_ENABLED(option) 0

/*
 * DMA arena. U-Boot hands out cache-coherent memory from dma_alloc_coherent;
 * Embox has no such allocator, so the module owns one NOCACHE arena and a
 * bump pointer, reset when the controller is (re)initialized. Everything the
 * hardware writes or reads (contexts, rings, DCBAA, scratchpad arrays) comes
 * from here -- the 5C OHCI lesson again: DMA pools must be NOCACHE.
 */
struct xhci_dma_arena {
	uint8_t *base;
	size_t   used;
	size_t   size;
};

extern struct xhci_dma_arena xhci_dma;

void *xhci_dma_alloc(size_t size, size_t align);

/* U-Boot allocation entry points redirected into the arena. Plain malloc()
 * stays the Embox heap: only hardware-visible memory needs the arena. */
void *xhci_dma_memalign(size_t align, size_t size);
#define memalign(align, size) xhci_dma_memalign((align), (size))

/*
 * U-Boot's USB device model, minimal shape: the fields the ported xHCI code
 * actually touches. The Embox glue (xhci_hcd.c) builds one of these around
 * Embox's struct usb_dev/usb_endp for the duration of a request and keeps
 * the slot_id association in udev2slot[].
 */
/* Descriptor layouts from U-Boot include/usb.h and linux/usb/ch9.h,
 * verbatim: xhci.c's root-hub template initializes them by position. */
#define USB_MAXCHILDREN 8 /* U-Boot's arbitrary root-port cap */

struct usb_endpoint_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bEndpointAddress;
	uint8_t  bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t  bInterval;
	uint8_t  bRefresh;
	uint8_t  bSynchAddress;
} __attribute__((packed));

struct usb_ss_ep_comp_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bMaxBurst;
	uint8_t  bmAttributes;
	uint16_t wBytesPerInterval;
} __attribute__((packed));

struct usb_device_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t bcdUSB;
	uint8_t  bDeviceClass;
	uint8_t  bDeviceSubClass;
	uint8_t  bDeviceProtocol;
	uint8_t  bMaxPacketSize0;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t  iManufacturer;
	uint8_t  iProduct;
	uint8_t  iSerialNumber;
	uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t wTotalLength;
	uint8_t  bNumInterfaces;
	uint8_t  bConfigurationValue;
	uint8_t  iConfiguration;
	uint8_t  bmAttributes;
	uint8_t  bMaxPower;
} __attribute__((packed));

struct xhci_ub_interface_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bInterfaceNumber;
	uint8_t  bAlternateSetting;
	uint8_t  bNumEndpoints;
	uint8_t  bInterfaceClass;
	uint8_t  bInterfaceSubClass;
	uint8_t  bInterfaceProtocol;
	uint8_t  iInterface;
} __attribute__((packed));

#define USB_MAXENDPOINTS 32
#define USB_MAXINTERFACES 8
#define USB_MAX_ACTIVE_INTERFACES 2

struct xhci_ub_interface {
	struct xhci_ub_interface_descriptor desc;
	uint8_t no_of_ep;
	uint8_t num_altsetting;
	uint8_t act_altsetting;
	struct usb_endpoint_descriptor ep_desc[USB_MAXENDPOINTS];
	struct usb_ss_ep_comp_descriptor ss_ep_comp_desc[USB_MAXENDPOINTS];
} __attribute__((packed));

struct xhci_ub_config {
	struct usb_config_descriptor desc;
	uint8_t no_of_if;
	struct xhci_ub_interface if_desc[USB_MAXINTERFACES];
} __attribute__((packed));

struct usb_hub_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bNbrPorts;
	uint16_t wHubCharacteristics;
	uint8_t  bPwrOn2PwrGood;
	uint8_t  bHubContrCurrent;
	union {
		struct {
			uint8_t DeviceRemovable[(USB_MAXCHILDREN + 1 + 7) / 8];
			uint8_t PortPowerCtrlMask[(USB_MAXCHILDREN + 1 + 7) / 8];
		} __attribute__((packed)) hs;
		struct {
			uint8_t  bHubHdrDecLat;
			uint16_t wHubDelay;
			uint16_t DeviceRemovable;
		} __attribute__((packed)) ss;
	} u;
} __attribute__((packed));


/* U-Boot enum usb_speed numeric values (NOT Embox's; the glue translates). */
#define USB_SPEED_UNKNOWN 0xff
#define USB_SPEED_FULL    0
#define USB_SPEED_LOW     1
#define USB_SPEED_HIGH    2
#define USB_SPEED_SUPER   3

/* U-Boot device status bits the ported code tests. */
#define USB_ST_BIT(x)     (1 << (x))

struct usb_device {
	void *controller;   /* struct xhci_ctrl *, single-instance module */
	int  devnum;        /* USB address assigned by the HC */
	int  speed;         /* USB_SPEED_* above */
	int  slot_id;
	int  addressed;     /* Address Device already completed */
	int  portnr;        /* root-hub port, 1-based */
	int  maxchild;
	unsigned long status;
	unsigned long act_len;
	struct usb_endpoint_descriptor ep[31]; /* [0] = ep0 IN/OUT */
	struct xhci_ub_config config;
	int epmaxpacketin[16];
};

static inline struct usb_device *xhci_get_ctrl_dev(struct usb_device *udev) {
	return udev;
}

/* U-Boot pipe encoding, from include/usb.h. The glue builds pipes from
 * Embox endpoints; the ported ring code decodes them. */
#define PIPE_DEV_MASK   (0xff)
#define PIPE_DEV_SHIFT  0
#define PIPE_INDEX_MASK 0xf
#define PIPE_INDEX_SHIFT 8

/* Unsigned. (3 << 30) is a negative int, and a 64-bit pipe sign-extends
 * it, so usb_pipecontrol() never matches. Control IN still landed on
 * index 0 by accident. Control OUT (SET_CONFIG) took ep*2-1, wrapped,
 * and prepare_ring reported the slot context as a disabled endpoint. */
#define PIPE_TYPE_MASK  (0x3u << 30)
#define PIPE_ISOCH      (0u << 30)
#define PIPE_BULK       (1u << 30)
#define PIPE_INTERRUPT  (2u << 30)
#define PIPE_CONTROL    (3u << 30)

/* Bit 31 is PIPE_INTERRUPT (2<<30) and half of PIPE_CONTROL (3<<30).
 * With the direction bit there, an interrupt OUT pipe could not say
 * OUT: the type bit forced usb_pipein, and the transfer hit the IN
 * index. Bit 16 is free (device is 0..7, endpoint is 8..11). */
#define PIPE_DIR_MASK   (1 << 16)
#define PIPE_DIR_IN     (1 << 16)
#define PIPE_DIR_OUT    (0)

#define usb_pipedev(p)      (((p) >> PIPE_DEV_SHIFT) & PIPE_DEV_MASK)
#define usb_pipedevice(p)   usb_pipedev(p)
#define usb_pipeendpoint(p) (((p) >> PIPE_INDEX_SHIFT) & PIPE_INDEX_MASK)
#define usb_pipetype(p)     ((p) & PIPE_TYPE_MASK)
#define usb_pipein(p)       ((p) & PIPE_DIR_IN)
#define usb_pipeout(p)      (!usb_pipein(p))
#define usb_pipecontrol(p)  (usb_pipetype(p) == PIPE_CONTROL)
#define usb_pipebulk(p)     (usb_pipetype(p) == PIPE_BULK)
#define usb_pipeint(p)      (usb_pipetype(p) == PIPE_INTERRUPT)
#define usb_pipeiso(p)      (usb_pipetype(p) == PIPE_ISOCH)

static inline unsigned int create_pipe(struct usb_device *dev, unsigned int ep) {
	return (dev->devnum << PIPE_DEV_SHIFT) | (ep << PIPE_INDEX_SHIFT);
}

/* U-Boot usb_maxpacket(): max packet of the endpoint a pipe addresses.
 * The shim's ep[] is indexed like U-Boot's: endpoint * 2 + (dir == IN). */
static inline int usb_maxpacket(struct usb_device *dev, unsigned long pipe) {
	int idx = usb_pipe_ep_index(pipe);
	return dev->ep[idx].wMaxPacketSize & 0x7ff;
}

/* Endpoint descriptor field decoding used by the ported code. */
static inline uint16_t usb_endpoint_maxp(const struct usb_endpoint_descriptor *ep) {
	return ep->wMaxPacketSize & 0x7ff;
}

static inline int usb_endpoint_num(const struct usb_endpoint_descriptor *ep) {
	return ep->bEndpointAddress & 0xf;
}

static inline int usb_endpoint_dir_in(const struct usb_endpoint_descriptor *ep) {
	return (ep->bEndpointAddress & 0x80) != 0;
}

static inline int usb_endpoint_xfer_int(const struct usb_endpoint_descriptor *ep) {
	return (ep->bmAttributes & 3) == 3;
}

static inline int usb_endpoint_xfer_bulk(const struct usb_endpoint_descriptor *ep) {
	return (ep->bmAttributes & 3) == 2;
}

static inline int usb_endpoint_xfer_isoc(const struct usb_endpoint_descriptor *ep) {
	return (ep->bmAttributes & 3) == 1;
}

/* Additional transactions per microframe: bits 12..11 of wMaxPacketSize. */
static inline int usb_endpoint_maxp_mult(const struct usb_endpoint_descriptor *ep) {
	return 1 + ((ep->wMaxPacketSize >> 11) & 0x3);
}

#define fallthrough __attribute__((fallthrough))

static inline int usb_endpoint_xfer_control(const struct usb_endpoint_descriptor *ep) {
	return (ep->bmAttributes & 3) == 0;
}

/* U-Boot control request header (Embox's usb_control_header differs). */
struct devrequest {
	uint8_t requesttype;
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;
} __attribute__((packed));

/* Composite request id xhci_submit_root switches on (usb_defs.h). */
#define DeviceRequest \
	((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) << 8)

/* U-Boot request types referenced by the ported root-hub code. */
#define USB_TYPE_STANDARD (0x00 << 5)
#define USB_TYPE_CLASS    (0x01 << 5)
#define USB_TYPE_MASK     (0x03 << 5)
#define USB_RECIP_DEVICE  0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT 0x02
#define USB_RECIP_OTHER   0x03

#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_CONFIGURATION 0x09
#ifndef USB_REQ_SET_INTERFACE
#define USB_REQ_SET_INTERFACE     0x0b
#endif

#define USB_DT_DEVICE        0x01
#define USB_DT_CONFIG        0x02
#define USB_DT_STRING        0x03
#define USB_DT_INTERFACE     0x04
#define USB_DT_ENDPOINT      0x05
#ifndef USB_DT_HUB
#define USB_DT_HUB           0x29
#endif
#define USB_DT_HUB_NONVAR    0x2a
#define USB_DT_SUPER_SPEED_HUB 0x2a

#endif /* XHCI_UBOOT_COMPAT_H_ */
