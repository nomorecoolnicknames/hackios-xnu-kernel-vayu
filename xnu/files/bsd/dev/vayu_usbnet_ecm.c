/*
 * CDC-ECM on vayu's USB controller: the upper half of bsd/dev/vayu_usbnet.c (levels 2 and 3).
 *
 * vayu_usbnet=2: the descriptors and the control endpoint, so that the host enumerates the phone
 * as an Ethernet device (Linux's cdc_ether says "new high-speed USB device"); nothing is carried.
 * vayu_usbnet=3: also the two bulk endpoints, the notification endpoint and en2 -- the host's usb0
 * at the other end of the cable, 10.11.0.2/24 on this side.
 *
 * The shape is Linux's drivers/usb/gadget/function/{f_ecm.c,u_ether.c}: a control interface with
 * the CDC functional descriptors (header, union, Ethernet networking) and an interrupt IN endpoint
 * for the notifications, then a data interface whose alternate setting 1 has the two bulk
 * endpoints. High speed only; string 4 carries the host's MAC address, ours is 02:a0:b0:c0:d0:02.
 *
 * The control endpoint is one TRB rewritten for each stage, ep0.c's three-stage flow polled from
 * the event thread: SETUP -> DATA (IN only: nothing here expects the host to send us data) ->
 * STATUS -> SETUP. The bulk rings are Linux's (gadget.c: 31 TRBs and a link TRB back to the first,
 * no LST, so the transfer never ends and UPDATETRANSFER tells the controller about new TRBs). The
 * memory is uncached (vayu_usbnet.c dma_area()); vayu_usbnet_clean/inval are barriers.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/route.h>
#include <netinet/in.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/vayu_usbnet.h>

extern void loader_log_putc(char c);
extern void HakiosEthernetStart(const char *interface, const uint8_t *address);   /* iokit/Kernel/hakios.wlan.cpp */
extern int rtrequest(int req, struct sockaddr *dst, struct sockaddr *gateway, struct sockaddr *netmask, int flags,
    struct rtentry **ret_nrt);                                                      /* bsd/net/route.c */

static int tee_printf(const char *format, ...) __printflike(1, 2);

static int
tee_printf(const char *format, ...)
{
	char line[320];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		(printf)("%s", line);
	}
	return length;
}
#define printf(...) tee_printf(__VA_ARGS__)

/* ---- the descriptors: high speed, CDC-ECM, as Linux's f_ecm.c builds them --------------------- */

#define USB_DT_DEVICE           1
#define USB_DT_CONFIG           2
#define USB_DT_STRING           3
#define USB_DT_INTERFACE        4
#define USB_DT_ENDPOINT         5
#define USB_DT_CS_INTERFACE     0x24

#define USB_CLASS_CDC           2
#define USB_CLASS_CDC_DATA      0x0a
#define USB_CDC_SUBCLASS_ECM    6

#define USB_ENDPOINT_XFER_BULK  2
#define USB_ENDPOINT_XFER_INT   3

/*
 * bLength 18, DEVICE; bcdUSB 2.00; class 2 (CDC) subclass 0 protocol 0; 64-byte packets; the
 * loader's vendor 0x18d1 with a product of our own (0xd00e, not its 0xd00d); 1.00; three strings,
 * one configuration.
 */
static const uint8_t usbnet_device[] = {
	18, USB_DT_DEVICE,
	0x00, 0x02,
	USB_CLASS_CDC, 0, 0,
	64,
	0xd1, 0x18,
	0x0e, 0xd0,
	0x00, 0x01,
	1, 2, 3,
	1,
};

/*
 * One configuration, two interfaces: 0 is the CDC control interface (header, union, Ethernet
 * networking, one interrupt IN endpoint), 1 has alternate 0 with no endpoints and alternate 1 with
 * the two bulk ones. wTotalLength (bytes 2-3) is filled in from sizeof at the copy below.
 */
static const uint8_t usbnet_config[] = {
	/* configuration: 80 bytes, 2 interfaces, value 1, 0xc0 (self-powered), 250 = 500 mA */
	9, USB_DT_CONFIG, 0, 0, 2, 1, 0, 0xc0, 250,
	/* interface 0: CDC ECM, one endpoint */
	9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_CDC, USB_CDC_SUBCLASS_ECM, 0, 0,
	/* CDC header: bcdCDC 1.10 */
	5, USB_DT_CS_INTERFACE, 0x00, 0x10, 0x01,
	/* CDC union: interface 0 controls interface 1 */
	5, USB_DT_CS_INTERFACE, 0x06, 0, 1,
	/* Ethernet networking: MAC at string 4, no statistics, 1514-byte frames, no filters */
	13, USB_DT_CS_INTERFACE, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0,
	/* EP 1 IN: the notifications, interrupt, 16 bytes, every 32 ms in high speed */
	7, USB_DT_ENDPOINT, 0x81, USB_ENDPOINT_XFER_INT, 16, 0, 9,
	/* interface 1 alternate 0: no endpoints */
	9, USB_DT_INTERFACE, 1, 0, 0, USB_CLASS_CDC_DATA, 0, 0, 0,
	/* interface 1 alternate 1: the frames, 512-byte packets both ways */
	9, USB_DT_INTERFACE, 1, 1, 2, USB_CLASS_CDC_DATA, 0, 0, 0,
	7, USB_DT_ENDPOINT, 0x82, USB_ENDPOINT_XFER_BULK, 0, 2, 0,
	7, USB_DT_ENDPOINT, 0x02, USB_ENDPOINT_XFER_BULK, 0, 2, 0,
};

/* Strings 1..4, and 0 the language: UTF-16LE in the wire format, built into the caller's buffer. */
static uint32_t
usbnet_string(uint32_t index, uint8_t *out)
{
	static const char *const strings[] = { "XNU", "vayu usbnet", "vayu0", "02A0B0C0D001" };
	const char *text;
	uint32_t length;

	if (index == 0) {
		out[0] = 4;
		out[1] = USB_DT_STRING;
		out[2] = 0x09;                  /* LANGID 0x0409: English, United States */
		out[3] = 0x04;
		return 4;
	}
	text = strings[index - 1];
	length = (uint32_t)strlen(text);
	out[0] = (uint8_t)(2 + 2 * length);
	out[1] = USB_DT_STRING;
	for (uint32_t i = 0; i < length; i++) {
		out[2 + 2 * i] = (uint8_t)text[i];
		out[3 + 2 * i] = 0;
	}
	return 2 + 2 * length;
}

/* ---- the control endpoint -------------------------------------------------------------------- */

struct usbnet_setup {
	uint8_t  bmRequestType;
	uint8_t  bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
} __attribute__((packed));

/* EP0_ARMED first: a zeroed state is "the SETUP TRB is waiting", which is where a reset leaves it. */
enum {
	EP0_ARMED,
	EP0_DATA_IN,            /* the answer is in the buffer, waiting for XferNotReady(DATA) */
	EP0_DATA_SENT,          /* the data stage is the controller's, waiting for its completion */
	EP0_STATUS_IN,          /* a request with no data: waiting for XferNotReady(STATUS) on ep0 IN */
	EP0_STATUS_IN_SENT,
	EP0_STATUS_OUT,         /* after a data stage: waiting for XferNotReady(STATUS) on ep0 OUT */
	EP0_STATUS_OUT_SENT,
};

static uint32_t ep0_state;
static uint32_t ep0_length;             /* bytes the data stage carries */
static uint32_t ep0_wlength;            /* the request's wLength */
static uint32_t ep0_alt;                /* interface 1's alternate setting */
static bool ep0_configured;
static int ep0_active = -1;             /* the ep0 direction with a transfer under way (phys), or -1 */
static uint32_t ep0_rsc;

/* STARTTRANSFER of the one ep0 TRB on phys, noted so that a reset can end it (gadget.c:3360). */
static int
ep0_start(unsigned int phys)
{
	uint64_t trb = vayu_usbnet_dma_pa(USB_DMA_EP0_TRB);
	int error;

	error = vayu_usbnet_ep_cmd_rsc(phys, USB_DEPCMD_STARTTRANSFER, (uint32_t)(trb >> 32), (uint32_t)trb, 0, &ep0_rsc);
	ep0_active = error == 0 ? (int)phys : -1;
	return error;
}

/* A control transfer cut short by a reset or a disconnect: end it before ep0 is used again. */
static void
ep0_end(void)
{
	if (ep0_active < 0) {
		return;
	}
	vayu_usbnet_ep_cmd((unsigned int)ep0_active, USB_DEPCMD_ENDTRANSFER | USB_DEPCMD_FORCERM | USB_DEPCMD_CMDIOC |
	    USB_DEPCMD_PARAM(ep0_rsc), 0, 0, 0);
	delay(100);                     /* (gadget.c:3247: the core needs a moment after CMDACT clears) */
	ep0_active = -1;
}

static void
ep0_restart(void)
{
	ep0_state = EP0_ARMED;
	ep0_active = -1;                /* (the SETUP TRB: a reset leaves it armed, ep0.c) */
	vayu_usbnet_setup_arm();
}

static void ep0_data_stage(void);

/* No data stage: the status stage is an IN on ep0 (ep0.c: dwc3_ep0_start_control_status). */
static void
ep0_status_only(void)
{
	ep0_state = EP0_STATUS_IN;
}

/* The IN data stage: length bytes from src (or, with src == NULL, what is already in the buffer). */
static void
ep0_data(const void *src, uint32_t length)
{
	uint32_t len = length < ep0_wlength ? length : ep0_wlength;

	if (len == 0) {
		ep0_status_only();      /* (wLength 0: the host wants no data, only the status) */
		return;
	}
	if (src != NULL) {
		memcpy((void *)vayu_usbnet_dma_va(USB_DMA_EP0_DATA), src, len);
	}
	vayu_usbnet_clean(vayu_usbnet_dma_va(USB_DMA_EP0_DATA), len);
	ep0_length = len;
	ep0_state = EP0_DATA_IN;
	/* started at once: the core starts on demand only the status stage (ep0.c __dwc3_gadget_ep0_queue) */
	ep0_data_stage();
}

static void
ep0_data_stage(void)
{
	vayu_usbnet_trb(USB_DMA_EP0_TRB, vayu_usbnet_dma_pa(USB_DMA_EP0_DATA), ep0_length,
	    USB_TRB_HWO | USB_TRB_LST | USB_TRB_ISP_IMI | USB_TRB_IOC | USB_TRB_TYPE(USB_TRBCTL_CONTROL_DATA));
	if (ep0_start(USB_EP_EP0IN) != 0) {
		printf("vayu-usbnet: ep0: the data stage cannot be started\n");
		ep0_restart();
		return;
	}
	ep0_state = EP0_DATA_SENT;
}

static void
ep0_status_in(void)                     /* two-stage: the status is an IN on ep0 */
{
	vayu_usbnet_trb(USB_DMA_EP0_TRB, vayu_usbnet_dma_pa(USB_DMA_EP0_SETUP), 0,
	    USB_TRB_HWO | USB_TRB_LST | USB_TRB_ISP_IMI | USB_TRB_IOC | USB_TRB_TYPE(USB_TRBCTL_CONTROL_STATUS2));
	if (ep0_start(USB_EP_EP0IN) != 0) {
		printf("vayu-usbnet: ep0: the status stage cannot be started\n");
		ep0_restart();
		return;
	}
	ep0_state = EP0_STATUS_IN_SENT;
}

static void
ep0_status_out(void)                    /* three-stage after the data: an OUT on ep0 */
{
	vayu_usbnet_trb(USB_DMA_EP0_TRB, vayu_usbnet_dma_pa(USB_DMA_EP0_SETUP), 0,
	    USB_TRB_HWO | USB_TRB_LST | USB_TRB_ISP_IMI | USB_TRB_IOC | USB_TRB_TYPE(USB_TRBCTL_CONTROL_STATUS3));
	if (ep0_start(USB_EP_EP0OUT) != 0) {
		printf("vayu-usbnet: ep0: the status stage cannot be started\n");
		ep0_restart();
		return;
	}
	ep0_state = EP0_STATUS_OUT_SENT;
}

static void
ep0_stall(void)
{
	/* (ep0.c: dwc3_ep0_stall_and_restart: halt ep0, then a SETUP TRB again) */
	printf("vayu-usbnet: stall\n");
	vayu_usbnet_ep_cmd(USB_EP_EP0OUT, USB_DEPCMD_SETSTALL, 0, 0, 0);
	ep0_restart();
}

static void
usbnet_data_stop(void);
static void usbnet_alt(uint32_t alt);
static void usbnet_configured(void);
static void usbnet_link(bool up);

/* One SETUP has arrived and its eight bytes are in the buffer: answer it (ep0.c: dwc3_ep0_inspect_setup). */
static void
ep0_setup(void)
{
	struct usbnet_setup *setup = (struct usbnet_setup *)vayu_usbnet_dma_va(USB_DMA_EP0_SETUP);
	uint8_t type, request, answer[8];
	uint16_t value, index;
	uint32_t kind, number;

	vayu_usbnet_inval((vm_offset_t)setup, sizeof(*setup));
	type = setup->bmRequestType;
	request = setup->bRequest;
	value = setup->wValue;
	index = setup->wIndex;
	ep0_wlength = setup->wLength;
	printf("vayu-usbnet: setup %02x %02x %04x %04x %04x\n", type, request, value, index, ep0_wlength);

	if (type == 0x80 && request == 6) {                     /* GET_DESCRIPTOR */
		kind = (uint32_t)value >> 8;
		number = value & 0xff;

		if (kind == USB_DT_DEVICE) {
			ep0_data(usbnet_device, sizeof(usbnet_device));
			return;
		}
		if (kind == USB_DT_CONFIG) {
			/* (the one field the table cannot carry: its own length) */
			memcpy((void *)vayu_usbnet_dma_va(USB_DMA_EP0_DATA), usbnet_config, sizeof(usbnet_config));
			((uint8_t *)vayu_usbnet_dma_va(USB_DMA_EP0_DATA))[2] = (uint8_t)sizeof(usbnet_config);
			((uint8_t *)vayu_usbnet_dma_va(USB_DMA_EP0_DATA))[3] = (uint8_t)(sizeof(usbnet_config) >> 8);
			ep0_data(NULL, sizeof(usbnet_config));
			return;
		}
		if (kind == USB_DT_STRING && number <= 4) {
			ep0_data(NULL, usbnet_string(number, (uint8_t *)vayu_usbnet_dma_va(USB_DMA_EP0_DATA)));
			return;
		}
		/* (the device qualifier and the other-speed configuration are for a two-speed device) */
		printf("vayu-usbnet: descriptor %u index %u: not for a high-speed-only device\n", kind, number);
		ep0_stall();
		return;
	}

	if (type == 0x00 && request == 5) {                     /* SET_ADDRESS */
		if (value > 127 || ep0_configured) {
			ep0_stall();
			return;
		}
		/* (at setup time, before the status stage: ep0.c:649 dwc3_ep0_set_address) */
		vayu_usbnet_wr(USB_DCFG, (vayu_usbnet_rd(USB_DCFG) & ~USB_DCFG_DEVADDR) | ((uint32_t)value << 3));
		printf("vayu-usbnet: address %u\n", value);
		ep0_status_only();
		return;
	}

	if (type == 0x00 && request == 9) {                     /* SET_CONFIGURATION */
		if (value == 1) {
			usbnet_data_stop();     /* (a second SET_CONFIGURATION: nothing may still run) */
			ep0_alt = 0;
			usbnet_configured();
			ep0_configured = true;
			printf("vayu-usbnet: configured: the endpoints are on\n");
		} else if (value == 0) {
			usbnet_data_stop();
			ep0_configured = false;
			printf("vayu-usbnet: unconfigured\n");
		} else {
			printf("vayu-usbnet: configuration %u: not here\n", value);
			ep0_stall();
			return;
		}
		ep0_status_only();
		return;
	}

	if (type == 0x80 && request == 8) {                     /* GET_CONFIGURATION */
		answer[0] = ep0_configured ? 1 : 0;
		ep0_data(answer, 1);
		return;
	}

	if ((type == 0x80 || type == 0x81 || type == 0x82) && request == 0) {   /* GET_STATUS */
		answer[0] = 0;
		answer[1] = 0;
		ep0_data(answer, 2);
		return;
	}

	if (type == 0x01 && request == 11) {                    /* SET_INTERFACE */
		if (index == 0 && value == 0) {
			/* (the control interface has one setting) */
		} else if (index == 1 && value <= 1) {
			usbnet_alt(value);
		} else {
			printf("vayu-usbnet: interface %u setting %u: not here\n", index, value);
			ep0_stall();
			return;
		}
		ep0_status_only();
		return;
	}

	if (type == 0x81 && request == 10) {                    /* GET_INTERFACE */
		answer[0] = index == 1 ? (uint8_t)ep0_alt : 0;
		ep0_data(answer, 1);
		return;
	}

	if (type == 0x21 && request == 0x43) {                  /* CDC SET_ETHERNET_PACKET_FILTER */
		printf("vayu-usbnet: packet filter %04x\n", value);
		ep0_status_only();
		return;
	}

	if (type == 0x02 && (request == 1 || request == 3)) {   /* CLEAR_FEATURE / SET_FEATURE */
		uint32_t epnum = index & 0x0f;

		/* (ENDPOINT_HALT; anything else is accepted and does nothing) */
		if (value == 0 && epnum != 0) {
			unsigned int phys = 2 * epnum + ((index & 0x80) != 0 ? 1 : 0);

			vayu_usbnet_ep_cmd(phys, request == 1 ? USB_DEPCMD_CLEARSTALL : USB_DEPCMD_SETSTALL, 0, 0, 0);
		}
		ep0_status_only();
		return;
	}

	/* (a data stage out of the host is not expected: ECM only sends on the bulk endpoints) */
	printf("vayu-usbnet: request %02x %02x %04x %04x %04x: not here\n", type, request, value, index, ep0_wlength);
	ep0_stall();
}

/* XferComplete on ep0: the previous stage is done (ep0.c: dwc3_ep0_xfer_complete). */
static void
ep0_complete(unsigned int phys)
{
	if (ep0_active == (int)phys) {
		ep0_active = -1;
	}
	if (phys == USB_EP_EP0OUT) {
		if (ep0_state == EP0_STATUS_OUT_SENT) {
			ep0_restart();
			return;
		}
		if (ep0_state != EP0_ARMED) {
			/* (the host gave up on the request and sent a new SETUP: start over) */
			printf("vayu-usbnet: ep0: a SETUP while state %u was pending\n", ep0_state);
		}
		ep0_setup();
		return;
	}
	if (ep0_state == EP0_DATA_SENT) {
		ep0_state = EP0_STATUS_OUT;     /* (the status of a control-IN is an OUT) */
		return;
	}
	if (ep0_state == EP0_STATUS_IN_SENT) {
		ep0_restart();
		return;
	}
	printf("vayu-usbnet: ep0: a completion in state %u\n", ep0_state);
}

/* XferNotReady on ep0: the stage the controller is waiting for can be started now. */
static void
ep0_notready(unsigned int phys, uint32_t status)
{
	if ((status & 3) == 1) {
		return;                 /* DATA: already started with the request (ep0_data) */
	}
	if (phys == USB_EP_EP0IN && (status & 3) == 2 && ep0_state == EP0_STATUS_IN) {   /* STATUS */
		ep0_status_in();
		return;
	}
	if (phys == USB_EP_EP0OUT && (status & 3) == 2 && ep0_state == EP0_STATUS_OUT) {
		ep0_status_out();
		return;
	}
	printf("vayu-usbnet: ep0: not ready %u in state %u\n", status, ep0_state);
}

/* ---- the data endpoints ---------------------------------------------------------------------- */

#define USB_EP_NUMBER_IN        2       /* EP2 IN: ours, phys 5 (EP2 OUT is 4, USB_EP_OUT) */
#define USB_MAXPACKET_NOTIFY    16

static LCK_GRP_DECLARE(usbnet_lck_grp, "vayu-usbnet");
static LCK_MTX_DECLARE(usbnet_lock, &usbnet_lck_grp);

/*
 * The OUT ring: 31 data TRBs and a link TRB at 31 pointing back at 0 with HWO held forever
 * (gadget.c:818) -- Linux's scheme, so the controller circles on its own and a frame is put back
 * where it was taken from. out_next is the oldest TRB the controller has not finished with, which
 * is also the next one to refill and the one to start at again after the ring ran dry.
 */
static uint32_t out_next;
static uint32_t out_rsc;
static bool out_running;            /* a transfer runs on the OUT ring */

/*
 * The IN ring: the same shape as the OUT ring, one TRB per frame (two for a frame whose length is a
 * multiple of 512: a zero-length packet must follow it, chained). in_head is the oldest TRB still the
 * controller's; a TRB is ours again once its HWO is clear (in_reclaim). One transfer, started with the
 * first frame and kept going with UPDATETRANSFER (gadget.c:1415).
 */
static uint32_t in_head;                /* the oldest TRB not yet given back */
static uint32_t in_tail;                /* where the next frame goes */
static uint32_t in_used;                /* data TRBs the controller has */
static uint32_t in_rsc;
static bool in_running;

/* The notification endpoint: NETWORK_CONNECTION first, then CONNECTION_SPEED_CHANGE (f_ecm.c). */
static uint32_t notify_phase;           /* 0 idle, 1 the first, 2 the second (a transfer runs) */
static uint32_t notify_rsc;

static bool link_up;
static bool data_up;

static ifnet_t usbnet_ifp;
static const uint8_t usbnet_address[ETHER_ADDR_LEN] = { 0x02, 0xa0, 0xb0, 0xc0, 0xd0, 0x02 };

static struct {
	uint64_t rx_frames, rx_bytes, tx_frames, tx_bytes, drops;
} usbnet_count;

static void
out_refill(uint32_t slot)
{
	vayu_usbnet_trb(USB_DMA_OUT_TRB + USB_TRB_STRIDE * slot,
	    vayu_usbnet_dma_pa(USB_DMA_OUT_DATA + USB_FRAME_MAX * slot), USB_FRAME_MAX,
	    USB_TRB_HWO | USB_TRB_IOC | USB_TRB_CSP | USB_TRB_TYPE(USB_TRBCTL_NORMAL));
}

static void
usbnet_input(const uint8_t *frame, size_t length);

/* The frames the controller has finished with, oldest first: hand them up and refill their TRBs. */
static void
out_drain(void)
{
	uint32_t refilled = 0;

	if (!out_running) {
		return;                         /* (an event queued before the ring was stopped) */
	}
	for (uint32_t n = 0; n < USB_TRB_DATA; n++) {
		struct vayu_usbnet_trb *trb =
		    (struct vayu_usbnet_trb *)vayu_usbnet_dma_va(USB_DMA_OUT_TRB + USB_TRB_STRIDE * out_next);
		uint32_t size, length;

		vayu_usbnet_inval((vm_offset_t)trb, sizeof(*trb));
		if ((trb->ctrl & USB_TRB_HWO) != 0) {
			break;                          /* (and everything after it: in order) */
		}
		__asm__ volatile ("dmb oshld" ::: "memory");    /* (the size the controller wrote with HWO) */
		size = trb->size & 0xffffff;
		length = size > USB_FRAME_MAX ? 0 : USB_FRAME_MAX - size;
		if (length != 0) {
			vayu_usbnet_inval(vayu_usbnet_dma_va(USB_DMA_OUT_DATA + USB_FRAME_MAX * out_next), length);
			usbnet_input((const uint8_t *)vayu_usbnet_dma_va(USB_DMA_OUT_DATA + USB_FRAME_MAX * out_next), length);
		}
		out_refill(out_next);
		refilled++;
		if (++out_next == USB_TRB_DATA) {
			out_next = 0;
		}
	}
	/* (a controller that found no HWO TRB waits for this before it looks again, gadget.c:1415) */
	if (refilled != 0 && out_running) {
		vayu_usbnet_ep_cmd(USB_EP_OUT, USB_DEPCMD_UPDATETRANSFER | USB_DEPCMD_PARAM(out_rsc), 0, 0, 0);
	}
}

static void
usbnet_out_kick(void)
{
	uint64_t trb = vayu_usbnet_dma_pa(USB_DMA_OUT_TRB + USB_TRB_STRIDE * out_next);

	out_rsc = 0;
	out_running = vayu_usbnet_ep_cmd_rsc(USB_EP_OUT, USB_DEPCMD_STARTTRANSFER, (uint32_t)(trb >> 32), (uint32_t)trb,
	    0, &out_rsc) == 0;
	if (!out_running) {
		printf("vayu-usbnet: ep2 OUT: the ring cannot be started\n");
	}
}

static void
usbnet_out_fill(void)
{
	for (uint32_t slot = 0; slot < USB_TRB_DATA; slot++) {
		out_refill(slot);
	}
	/* the link TRB, whose HWO is never cleared (gadget.c:818) */
	vayu_usbnet_trb(USB_DMA_OUT_TRB + USB_TRB_STRIDE * USB_TRB_DATA, vayu_usbnet_dma_pa(USB_DMA_OUT_TRB), 0,
	    USB_TRB_HWO | USB_TRB_TYPE(USB_TRBCTL_LINK));
	out_next = 0;
	usbnet_out_kick();
}

/* Start at the oldest armed TRB: the controller walks on from there by itself (gadget.c:1415). */
static void
usbnet_in_kick(void)
{
	uint64_t trb = vayu_usbnet_dma_pa(USB_DMA_IN_TRB + USB_TRB_STRIDE * in_head);

	in_rsc = 0;
	if (vayu_usbnet_ep_cmd_rsc(USB_EP_IN, USB_DEPCMD_STARTTRANSFER, (uint32_t)(trb >> 32), (uint32_t)trb, 0,
	    &in_rsc) != 0) {
		printf("vayu-usbnet: ep2 IN: the transfer cannot be started\n");
		in_running = false;
		return;
	}
	in_running = true;
}

static void
notify_send(uint32_t phase)
{
	uint8_t *data = (uint8_t *)vayu_usbnet_dma_va(USB_DMA_NOTIFY_DATA);
	uint64_t trb = vayu_usbnet_dma_pa(USB_DMA_NOTIFY_TRB);
	uint32_t length;

	if (notify_phase != 0) {
		return;                         /* (one at a time: the first one is still the controller's) */
	}
	if (phase == 1) {
		/* NETWORK_CONNECTION (CDC 1.2 6.3.1): connected */
		data[0] = 0xa1;
		data[1] = 0x00;
		data[2] = 0x01;                 /* wValue: 1 = connected */
		data[3] = 0x00;
		data[4] = 0x00;
		data[5] = 0x00;
		data[6] = 0x00;                 /* wLength */
		data[7] = 0x00;
		length = 8;
	} else {
		/* CONNECTION_SPEED_CHANGE (6.3.2): 480 Mbit/s each way */
		data[0] = 0xa1;
		data[1] = 0x2a;
		data[2] = 0x00;
		data[3] = 0x00;
		data[4] = 0x08;                 /* wLength: two 32-bit rates */
		data[5] = 0x00;
		data[6] = 0x00;                 /* 480000000, little endian */
		data[7] = 0x38;
		data[8] = 0x9c;
		data[9] = 0x1c;
		data[10] = 0x00;
		data[11] = 0x38;
		data[12] = 0x9c;
		data[13] = 0x1c;
		length = 16;
	}
	vayu_usbnet_clean((vm_offset_t)data, length);
	vayu_usbnet_trb(USB_DMA_NOTIFY_TRB, vayu_usbnet_dma_pa(USB_DMA_NOTIFY_DATA), length,
	    USB_TRB_HWO | USB_TRB_LST | USB_TRB_IOC | USB_TRB_TYPE(USB_TRBCTL_NORMAL));
	notify_phase = phase;
	if (vayu_usbnet_ep_cmd_rsc(USB_EP_NOTIFY, USB_DEPCMD_STARTTRANSFER, (uint32_t)(trb >> 32), (uint32_t)trb, 0,
	    &notify_rsc) != 0) {
		printf("vayu-usbnet: ep1 IN: the notification cannot be sent\n");
		notify_phase = 0;
	}
}

/* SET_CONFIGURATION(1): the three endpoints of the ECM function (gadget.c:670, f_ecm.c). */
static void
usbnet_configured(void)
{
	uint32_t bits = (1u << USB_EP_NOTIFY) | (1u << USB_EP_OUT) | (1u << USB_EP_IN);

	/*
	 * INIT every time (gadget.c:701 on each enable): the data toggles start again as the host's do.
	 * The endpoints are off while that happens; their transfer resources were assigned once, at the
	 * start (vayu_usbnet.c dwc3_endpoints()). FIFO 1 for the interrupt endpoint, 2 for bulk IN.
	 */
	vayu_usbnet_wr(USB_DALEPENA, vayu_usbnet_rd(USB_DALEPENA) & ~bits);
	if (vayu_usbnet_ep_config(USB_EP_NOTIFY, USB_DEPCFG_TYPE_INTR, USB_MAXPACKET_NOTIFY, 1, 9, false, 0) != 0 ||
	    vayu_usbnet_ep_config(USB_EP_OUT, USB_DEPCFG_TYPE_BULK, USB_MAXPACKET, 0, 0, true, 0) != 0 ||
	    vayu_usbnet_ep_config(USB_EP_IN, USB_DEPCFG_TYPE_BULK, USB_MAXPACKET, USB_EP_NUMBER_IN, 0, true, 0) != 0) {
		printf("vayu-usbnet: the endpoints cannot be configured\n");
		return;
	}
	vayu_usbnet_wr(USB_DALEPENA, vayu_usbnet_rd(USB_DALEPENA) | bits);
	printf("vayu-usbnet: endpoints: ep1 IN notify %u bytes, ep2 OUT and IN %u\n", USB_MAXPACKET_NOTIFY, USB_MAXPACKET);
}

/* The IN ring empty: no data TRB armed, the link TRB back to the first. */
static void
in_fill(void)
{
	for (uint32_t slot = 0; slot < USB_TRB_DATA; slot++) {
		vayu_usbnet_trb(USB_DMA_IN_TRB + USB_TRB_STRIDE * slot, 0, 0, 0);
	}
	vayu_usbnet_trb(USB_DMA_IN_TRB + USB_TRB_STRIDE * USB_TRB_DATA, vayu_usbnet_dma_pa(USB_DMA_IN_TRB), 0,
	    USB_TRB_HWO | USB_TRB_TYPE(USB_TRBCTL_LINK));
	in_head = in_tail = in_used = 0;
}

/* The TRBs the controller has given back, oldest first (called with usbnet_lock held). */
static void
in_reclaim(void)
{
	while (in_used != 0) {
		struct vayu_usbnet_trb *trb =
		    (struct vayu_usbnet_trb *)vayu_usbnet_dma_va(USB_DMA_IN_TRB + USB_TRB_STRIDE * in_head);

		vayu_usbnet_inval((vm_offset_t)trb, sizeof(*trb));
		if ((trb->ctrl & USB_TRB_HWO) != 0) {
			break;
		}
		in_used--;
		in_head = (in_head + 1) % USB_TRB_DATA;
	}
}

static void
usbnet_data_stop(void)
{
	bool was_up;

	/* (data_up first, under the lock: no frame is queued behind our back from here on) */
	lck_mtx_lock(&usbnet_lock);
	was_up = data_up;
	data_up = false;
	if (in_running) {
		/* (gadget.c:3240: ENDTRANSFER with FORCERM, then the core's moment, gadget.c:3247) */
		vayu_usbnet_ep_cmd(USB_EP_IN, USB_DEPCMD_ENDTRANSFER | USB_DEPCMD_FORCERM | USB_DEPCMD_CMDIOC |
		    USB_DEPCMD_PARAM(in_rsc), 0, 0, 0);
		delay(100);
	}
	in_rsc = 0;
	in_fill();
	in_running = false;
	lck_mtx_unlock(&usbnet_lock);
	if (out_running) {
		vayu_usbnet_ep_cmd(USB_EP_OUT, USB_DEPCMD_ENDTRANSFER | USB_DEPCMD_FORCERM | USB_DEPCMD_CMDIOC |
		    USB_DEPCMD_PARAM(out_rsc), 0, 0, 0);
		delay(100);
	}
	out_rsc = 0;
	out_running = false;
	out_next = 0;
	if (notify_phase != 0) {
		/* (a notification the host never read keeps its transfer: end it, or the next cannot start) */
		vayu_usbnet_ep_cmd(USB_EP_NOTIFY, USB_DEPCMD_ENDTRANSFER | USB_DEPCMD_FORCERM | USB_DEPCMD_CMDIOC |
		    USB_DEPCMD_PARAM(notify_rsc), 0, 0, 0);
		delay(100);
	}
	notify_phase = 0;
	if (was_up) {
		usbnet_link(false);
	}
}

static void
usbnet_data_start(void)
{
	if (usbnet_ifp == NULL || !ep0_configured) {
		printf("vayu-usbnet: interface 1 alternate 1: no data path\n");
		return;
	}
	/* the bulk endpoints anew (data toggles from DATA0, as after f_ecm's set_alt) */
	usbnet_configured();
	usbnet_out_fill();
	lck_mtx_lock(&usbnet_lock);
	in_fill();
	in_running = false;
	in_rsc = 0;
	data_up = true;
	lck_mtx_unlock(&usbnet_lock);
	usbnet_link(true);
	notify_send(1);
}

static void
usbnet_alt(uint32_t alt)
{
	/* (f_ecm's set_alt: whatever ran stops first, then alternate 1 starts afresh) */
	usbnet_data_stop();
	ep0_alt = alt;
	if (alt == 1) {
		usbnet_data_start();
	}
}

/*
 * One frame into the IN ring (called with usbnet_lock held): the zero-length TRB first, then the
 * frame's with HWO last, so the controller never meets half of a chain; then STARTTRANSFER if no
 * transfer runs yet, else the no-response UPDATETRANSFER (gadget.c:1415).
 */
static bool
usbnet_tx(mbuf_t packet, uint32_t length)
{
	uint32_t slot = in_tail, next = (in_tail + 1) % USB_TRB_DATA;
	bool zlp = (length % USB_MAXPACKET) == 0;
	uint8_t *buffer;

	if (!data_up) {
		return false;                   /* (stopped between the caller's look and the lock) */
	}
	in_reclaim();
	if (in_used + (zlp ? 2 : 1) > USB_TRB_DATA - 1) {
		return false;                   /* (the ring is full: the frame is dropped) */
	}
	buffer = (uint8_t *)vayu_usbnet_dma_va(USB_DMA_IN_DATA + USB_FRAME_MAX * slot);
	if (mbuf_copydata(packet, 0, length, buffer) != 0) {
		return false;
	}
	if (zlp) {
		vayu_usbnet_trb(USB_DMA_IN_TRB + USB_TRB_STRIDE * next,
		    vayu_usbnet_dma_pa(USB_DMA_IN_DATA + USB_FRAME_MAX * next), 0,
		    USB_TRB_HWO | USB_TRB_IOC | USB_TRB_TYPE(USB_TRBCTL_NORMAL));
		vayu_usbnet_trb(USB_DMA_IN_TRB + USB_TRB_STRIDE * slot,
		    vayu_usbnet_dma_pa(USB_DMA_IN_DATA + USB_FRAME_MAX * slot), length,
		    USB_TRB_HWO | USB_TRB_CHN | USB_TRB_TYPE(USB_TRBCTL_NORMAL));
		in_tail = (next + 1) % USB_TRB_DATA;
		in_used += 2;
	} else {
		vayu_usbnet_trb(USB_DMA_IN_TRB + USB_TRB_STRIDE * slot,
		    vayu_usbnet_dma_pa(USB_DMA_IN_DATA + USB_FRAME_MAX * slot), length,
		    USB_TRB_HWO | USB_TRB_IOC | USB_TRB_TYPE(USB_TRBCTL_NORMAL));
		in_tail = next;
		in_used += 1;
	}
	if (!in_running) {
		usbnet_in_kick();
	} else {
		vayu_usbnet_ep_cmd(USB_EP_IN, USB_DEPCMD_UPDATETRANSFER | USB_DEPCMD_PARAM(in_rsc), 0, 0, 0);
	}
	return true;
}

/* The frame the stack wants out: into a free IN slot, under the lock the thread takes too. */
static errno_t
usbnet_output(ifnet_t ifp, mbuf_t packet)
{
	size_t length = mbuf_pkthdr_len(packet);
	bool queued = false;

	if (link_up && data_up && length != 0 && length <= USB_FRAME_MAX) {
		lck_mtx_lock(&usbnet_lock);
		queued = usbnet_tx(packet, (uint32_t)length);
		lck_mtx_unlock(&usbnet_lock);
	}
	if (queued) {
		usbnet_count.tx_frames++;
		usbnet_count.tx_bytes += length;
		ifnet_stat_increment_out(ifp, 1, (uint32_t)length, 0);
	} else {
		usbnet_count.drops++;
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(packet);
	return 0;
}

/* ---- the interface --------------------------------------------------------------------------- */

/* A frame has arrived from the host: the same path hakios.net.c's HAKIOS_NET_INPUT takes. */
static void
usbnet_input(const uint8_t *frame, size_t length)
{
	struct ifnet_stat_increment_param stats = { .packets_in = 1, .bytes_in = (uint32_t)length };
	unsigned int chunks = 1;
	mbuf_t packet = NULL;
	char *header;

	usbnet_count.rx_frames++;
	usbnet_count.rx_bytes += length;
	if (usbnet_ifp == NULL || length < ETHER_HDR_LEN || length > USB_FRAME_MAX ||
	    mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet) != 0) {
		usbnet_count.drops++;
		return;
	}
	if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
		mbuf_freem(packet);
		usbnet_count.drops++;
		return;
	}
	header = mbuf_data(packet);
	mbuf_pkthdr_setrcvif(packet, usbnet_ifp);
	mbuf_pkthdr_setheader(packet, header);
	mbuf_pkthdr_adjustlen(packet, -ETHER_HDR_LEN);
	mbuf_setdata(packet, header + ETHER_HDR_LEN, length - ETHER_HDR_LEN);
	bpf_tap_in(usbnet_ifp, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
	ifnet_input(usbnet_ifp, packet, &stats);
}

/* Tell whoever listens for kernel events (configd does) that the link is up, or down. */
static void
usbnet_link_event(ifnet_t ifp, uint32_t code)
{
	_Alignas(struct kern_event_msg) char message[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = { 0 };
	struct kern_event_msg *header = (struct kern_event_msg *)message;
	struct net_event_data *about = (struct net_event_data *)(header + 1);

	header->total_size = sizeof(message);
	header->vendor_code = KEV_VENDOR_APPLE;
	header->kev_class = KEV_NETWORK_CLASS;
	header->kev_subclass = KEV_DL_SUBCLASS;
	header->event_code = code;
	about->if_family = ifnet_family(ifp);
	about->if_unit = ifnet_unit(ifp);
	strlcpy(about->if_name, ifnet_name(ifp), IFNAMSIZ);
	ifnet_event(ifp, header);
}

static void
usbnet_link(bool up)
{
	if (usbnet_ifp == NULL || link_up == up) {
		return;
	}
	link_up = up;
	printf("vayu-usbnet: en2: the link is %s\n", up ? "up" : "down");
	usbnet_link_event(usbnet_ifp, up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
}

/* As hakios.net.c's net_ioctl, without a radio's cases. */
static errno_t
usbnet_ioctl(ifnet_t ifp, u_long command, void *data)
{
	static const int media[] = { IFM_ETHER | IFM_AUTO };
	struct ifreq *request = data;
	struct ifmediareq *media_request = data;
	user_addr_t list;
	errno_t error = 0;

	switch (command) {
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64:
		/*
		 * What configd asks to learn whether the cable is in (hakios.net.c net_ioctl). Without an
		 * answer it took en2 for active from boot, ran DHCP into nothing and settled for a link-local
		 * address before the host had even selected alternate 1 (run 070520, 14.4 s).
		 */
		list = command == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)media_request)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)media_request)->ifmu_ulist);
		media_request->ifm_active = media_request->ifm_current = media[0];
		media_request->ifm_mask = 0;
		media_request->ifm_status = IFM_AVALID | (link_up ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && media_request->ifm_count > 0) {
			error = copyout(media, list, sizeof(media));
		}
		media_request->ifm_count = 1;
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;                          /* (the host's frames all come in anyway) */
	case SIOCSIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		break;
	case SIOCSIFFLAGS:
		/* (running whenever it is up: whether the cable carries anything is the link's answer) */
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCSIFMTU:
		error = request->ifr_mtu > ETHERMTU || request->ifr_mtu < IF_MINMTU ? EINVAL :
		    ifnet_set_mtu(ifp, request->ifr_mtu);
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}
	return error;
}

static bool
usbnet_attach(void)
{
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = "vayu-usbnet",
		.uniqueid_len = 11,
		.name = "en",
		.unit = 2,
		.family = IFNET_FAMILY_ETHERNET,
		.subfamily = IFNET_SUBFAMILY_ANY,
		.type = IFT_ETHER,
		.output = usbnet_output,
		.demux = ether_demux,
		.add_proto = ether_add_proto,
		.del_proto = ether_del_proto,
		.check_multi = ether_check_multi,
		.framer_extended = ether_frameout_extended,
		.ioctl = usbnet_ioctl,
		.broadcast_addr = etherbroadcastaddr,
		.broadcast_len = ETHER_ADDR_LEN,
	};
	errno_t error;

	error = ifnet_allocate_extended(&init, &usbnet_ifp);
	if (error != 0) {
		printf("vayu-usbnet: en2 cannot be made: error %d\n", error);
		return false;
	}
	ifnet_set_capabilities_enabled(usbnet_ifp, 0, -1);
	ifnet_set_addrlen(usbnet_ifp, ETHER_ADDR_LEN);
	ifnet_set_baudrate(usbnet_ifp, 480 * 1000 * 1000);
	ifnet_set_mtu(usbnet_ifp, ETHERMTU);
	ifnet_set_flags(usbnet_ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
	ifnet_set_hdrlen(usbnet_ifp, ETHER_HDR_LEN);
	ifnet_set_offload(usbnet_ifp, 0);
	error = ifnet_attach(usbnet_ifp, NULL);
	if (error != 0) {
		printf("vayu-usbnet: en2 cannot be attached: error %d\n", error);
		ifnet_release(usbnet_ifp);
		usbnet_ifp = NULL;
		return false;
	}
	ifnet_set_lladdr(usbnet_ifp, usbnet_address, ETHER_ADDR_LEN);
	bpfattach(usbnet_ifp, DLT_EN10MB, ETHER_HDR_LEN);
	printf("vayu-usbnet: en2 attached, %02x:%02x:%02x:%02x:%02x:%02x, link down\n", usbnet_address[0],
	    usbnet_address[1], usbnet_address[2], usbnet_address[3], usbnet_address[4], usbnet_address[5]);
	return true;
}

/* ---- the address: 10.11.0.2/24, once (bsd/netinet/in.c:1272 for the attach) ------------------- */

/*
 * struct in_aliasreq, laid out here rather than including netinet/in_var.h from bsd/dev: the same
 * bytes as net/if.h's ifaliasreq, whose size the SIOCAIFADDR number carries, and which in_control()
 * (bsd/netinet/in.c:656) casts to this. Three sockaddr_in, 16 bytes each, after IFNAMSIZ.
 */
struct in_aliasreq {
	char ifra_name[IFNAMSIZ];
	struct sockaddr_in ifra_addr;
	struct sockaddr_in ifra_broadaddr;
	struct sockaddr_in ifra_mask;
};

static void
usbnet_ipv4(void)
{
	struct in_aliasreq ifra;
	struct ifreq ifr;
	struct sockaddr_in *sin;
	socket_t so = NULL;
	uint32_t ip = vayu_usbnet_ip();
	errno_t error;

	error = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	if (error != 0) {
		printf("vayu-usbnet: no socket to give en2 its address: %d\n", error);
		return;
	}
	bzero(&ifr, sizeof(ifr));
	strlcpy(ifr.ifr_name, "en2", sizeof(ifr.ifr_name));
	error = sock_ioctl(so, SIOCPROTOATTACH, &ifr);
	printf("vayu-usbnet: SIOCPROTOATTACH en2: %d%s\n", error, error == EEXIST ? " (already attached)" : "");

	bzero(&ifra, sizeof(ifra));
	strlcpy(ifra.ifra_name, "en2", sizeof(ifra.ifra_name));
	sin = &ifra.ifra_addr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(ip);
	sin = &ifra.ifra_mask;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(0xffffff00);
	sin = &ifra.ifra_broadaddr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl((ip & 0xffffff00) | 0xff);
	error = sock_ioctl(so, SIOCAIFADDR, &ifra);
	printf("vayu-usbnet: SIOCAIFADDR en2 %u.%u.%u.%u/24: %d\n", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
	    (ip >> 8) & 0xff, ip & 0xff, error);

	bzero(&ifr, sizeof(ifr));
	strlcpy(ifr.ifr_name, "en2", sizeof(ifr.ifr_name));
	error = sock_ioctl(so, SIOCGIFFLAGS, &ifr);
	if (error == 0) {
		ifr.ifr_flags |= IFF_UP;
		error = sock_ioctl(so, SIOCSIFFLAGS, &ifr);
	}
	printf("vayu-usbnet: en2 up: %d\n", error);
	sock_close(so);

	/* the default route through the host (10.11.0.1: the same /24, host part 1) */
	{
		struct sockaddr_in dst = { .sin_len = sizeof(dst), .sin_family = AF_INET };
		struct sockaddr_in mask = { .sin_len = sizeof(mask), .sin_family = AF_INET };
		struct sockaddr_in gateway = { .sin_len = sizeof(gateway), .sin_family = AF_INET };

		gateway.sin_addr.s_addr = htonl((ip & 0xffffff00) | 1);
		error = rtrequest(RTM_ADD, (struct sockaddr *)&dst, (struct sockaddr *)&gateway, (struct sockaddr *)&mask,
		    RTF_UP | RTF_GATEWAY | RTF_STATIC, NULL);
		printf("vayu-usbnet: default route via %u.%u.%u.1: %d\n", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
		    (ip >> 8) & 0xff, error);
	}
}

/* ---- what the controller's side calls -------------------------------------------------------- */

void
vayu_usbnet_ecm_init(void)
{
	if (vayu_usbnet_level() < 3) {
		return;
	}
	uint32_t manual = 0;

	if (!usbnet_attach()) {
		return;
	}
	/*
	 * configd makes a network service only of an interface it finds in IOKit (hakios.wlan.cpp's top
	 * comment); with one, en2 is configured like any Ethernet port: DHCP from the host (its dnsmasq
	 * hands out 10.11.0.2, the router and DNS), and the routing, DNS and reachability that apps see.
	 * usbnet_static=1 instead: the address and default route from here, enough for ssh and ping (no
	 * DNS: that is configd's to give mDNSResponder).
	 */
	PE_parse_boot_argn("usbnet_static", &manual, sizeof(manual));
	if (manual != 0) {
		usbnet_ipv4();
	} else {
		HakiosEthernetStart("en2", usbnet_address);
		printf("vayu-usbnet: en2 left to configd (DHCP); usbnet_static=1 would set it here\n");
	}
}

void
vayu_usbnet_ecm_connect(void)
{
	/* (gadget.c:3392 dwc3_gadget_conndone_interrupt: the speed is known, ep0 is 64 by now) */
	if (vayu_usbnet_ep_config(USB_EP_EP0OUT, USB_DEPCFG_TYPE_CONTROL, 64, 0, 0, false, 2) != 0 ||
	    vayu_usbnet_ep_config(USB_EP_EP0IN, USB_DEPCFG_TYPE_CONTROL, 64, 0, 0, false, 2) != 0) {
		printf("vayu-usbnet: ep0 cannot be told the speed\n");
	}
}

void
vayu_usbnet_ecm_reset(void)
{
	usbnet_data_stop();
	ep0_configured = false;
	ep0_alt = 0;
	/* (a halt the host set does not outlive the reset: gadget.c dwc3_clear_stall_all_ep) */
	vayu_usbnet_ep_cmd(USB_EP_NOTIFY, USB_DEPCMD_CLEARSTALL, 0, 0, 0);
	vayu_usbnet_ep_cmd(USB_EP_OUT, USB_DEPCMD_CLEARSTALL, 0, 0, 0);
	vayu_usbnet_ep_cmd(USB_EP_IN, USB_DEPCMD_CLEARSTALL, 0, 0, 0);
	/* (an armed SETUP TRB lives through a bus reset, which is what ep0.c assumes; a request that was
	 * under way is ended first, gadget.c:3360 dwc3_ep0_end_control_data) */
	if (ep0_state != EP0_ARMED) {
		ep0_end();
		vayu_usbnet_ep_cmd(USB_EP_EP0OUT, USB_DEPCMD_SETSTALL, 0, 0, 0);
		ep0_restart();
	}
}

void
vayu_usbnet_ecm_disconnect(void)
{
	usbnet_data_stop();
	ep0_configured = false;
	ep0_alt = 0;
	if (ep0_state != EP0_ARMED) {
		ep0_end();
		ep0_restart();
	}
}

void
vayu_usbnet_ecm_ep_event(unsigned int phys, uint32_t event, uint32_t status, uint32_t param)
{
	(void)param;

	switch (phys) {
	case USB_EP_EP0OUT:
	case USB_EP_EP0IN:
		if (event == USB_DEPEVT_XFERCOMPLETE || event == USB_DEPEVT_XFERINPROGRESS) {
			ep0_complete(phys);
		} else if (event == USB_DEPEVT_XFERNOTREADY) {
			ep0_notready(phys, status);
		}
		break;
	case USB_EP_NOTIFY:
		if (event == USB_DEPEVT_XFERCOMPLETE) {
			uint32_t phase = notify_phase;

			notify_phase = 0;
			if (phase == 1) {
				notify_send(2);
			}
		}
		break;
	case USB_EP_OUT:
		out_drain();
		if (event == USB_DEPEVT_XFERNOTREADY && !out_running && data_up) {
			usbnet_out_kick();      /* (no transfer runs: start one at the oldest TRB, gadget.c:3098) */
		}
		break;
	case USB_EP_IN:
		lck_mtx_lock(&usbnet_lock);
		in_reclaim();
		if (event == USB_DEPEVT_XFERCOMPLETE) {
			/* (only after an ENDTRANSFER, as no TRB carries LST: the transfer is over) */
			in_running = false;
			in_rsc = 0;
		}
		if (!in_running && in_used != 0 && data_up) {
			usbnet_in_kick();
		}
		lck_mtx_unlock(&usbnet_lock);
		break;
	default:
		printf("vayu-usbnet: endpoint %u: event %u\n", phys, event);
		break;
	}
}

void
vayu_usbnet_ecm_counters(void)
{
	if (!ep0_configured) {
		return;
	}
	printf("vayu-usbnet: %llu frames in (%llu bytes), %llu out (%llu bytes), %llu dropped; %llu events\n",
	    usbnet_count.rx_frames, usbnet_count.rx_bytes, usbnet_count.tx_frames, usbnet_count.tx_bytes,
	    usbnet_count.drops, vayu_usbnet_events());
}
