/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * vayu's USB controller (POCO X3 Pro, SM8150), bsd/dev/vayu_usbnet.c and bsd/dev/vayu_usbnet_ecm.c.
 * Origin: the iOS 16.5 line's NBD branch (d0e149d) bsd/sys/vayu_usbnet.h (the 8796 line); the DMA layout is new on 12377 (below).
 *
 * Only with vayu27=1 (sys/vayu_12377_abi.h), then:
 * vayu_usbnet=1: a look at the controller as the loader's UEFI left it, reading only.
 * vayu_usbnet=2: take it from the loader (usb_handover=1) and enumerate as CDC-ECM; no interface.
 * vayu_usbnet=3: and carry frames, between the host's usb0 and this kernel's en2. usbnet_static=1 sets
 *   10.11.0.2/24 and the default route through 10.11.0.1 from the kernel; without it en2 is left to whoever
 *   publishes it to configd (vayu_usbnet_set_publisher(); nothing on 12377 does yet, so en2 stays unconfigured).
 * Unlike the 8796 line, usb_handover=1 alone is not level 3: both arguments are needed (SPEC-K6).
 *
 * vayu_usbnet.c owns the controller: the registers, the DMA area, the endpoint commands, the event
 * buffer and the thread that polls it. Everything above DWC3 -- descriptors, the control endpoint's
 * state machine, the two bulk rings, the notification endpoint, the ifnet -- is vayu_usbnet_ecm.c's.
 * What the two share is in this file: the register offsets, the TRB and endpoint-command bits, the
 * DMA layout, and the calls between them.
 */
#ifndef _SYS_VAYU_USBNET_H_
#define _SYS_VAYU_USBNET_H_

#include <mach/mach_types.h>
#include <stdbool.h>
#include <stdint.h>

void vayu_usbnet_init(void);
/* on the way back to the loader (registered with loader_quiesce_register): the controller, if this kernel took it */
void vayu_usbnet_quiesce(void);

/*
 * en2's publication to configd (an IOKit object for the interface): the 8796 line's HakiosEthernetStart()
 * (iokit/Kernel/hakios.wlan.cpp) is not in this tree. A port that brings one registers it here; until then,
 * usbnet_static=1 is the way to an address.
 */
typedef void (*vayu_usbnet_publish_fn)(const char *ifname, const uint8_t *address);
void vayu_usbnet_set_publisher(vayu_usbnet_publish_fn fn);

/* ---- the controller's side, for the ECM side ------------------------------------------------- */

uint32_t vayu_usbnet_level(void);       /* 1, 2 or 3: the boot-arg's level */
uint32_t vayu_usbnet_ip(void);          /* usbnet_ip: 10.11.0.2, one uint32 */
uint64_t vayu_usbnet_events(void);      /* events processed, for the half-minute line */

uint32_t vayu_usbnet_rd(vm_offset_t offset);
void vayu_usbnet_wr(vm_offset_t offset, uint32_t value);

/* One endpoint command, polled to completion (gadget.c:397 dwc3_send_gadget_ep_cmd).
 * Returns 0, or the command's status (bits 12-15) / 0x10 for "no completion in 10 ms". */
int vayu_usbnet_ep_cmd(unsigned int phys, uint32_t cmd, uint32_t param0, uint32_t param1, uint32_t param2);
/* ... and with the transfer resource index a STARTTRANSFER is answered with (core.h:477). */
int vayu_usbnet_ep_cmd_rsc(unsigned int phys, uint32_t cmd, uint32_t param0, uint32_t param1, uint32_t param2,
    uint32_t *rsc);
/* SETEPCONFIG (gadget.c:670). action: 0 = INIT, 2 = MODIFY (core.h's DEPCFG_ACTION, bits 30-31). */
int vayu_usbnet_ep_config(unsigned int phys, uint32_t type, uint32_t maxpacket, uint32_t fifo, uint32_t binterval,
    bool xfer_in_progress, uint32_t action);

vm_offset_t vayu_usbnet_dma_va(vm_offset_t offset);
uint64_t vayu_usbnet_dma_pa(vm_offset_t offset);
/*
 * The DMA area is write-back RAM (K3's contract, sys/vayu_dma_compat.h): the CPU's bytes go out with
 * vayu_usbnet_for_device() before the controller is told about them, and the controller's are read only after
 * vayu_usbnet_for_cpu(). Both take an offset in the area and work on whole cache lines around it: every piece of
 * the layout below starts on a line of its own and is written by one side at a time.
 */
void vayu_usbnet_for_device(vm_offset_t offset, vm_size_t len);
void vayu_usbnet_for_cpu(vm_offset_t offset, vm_size_t len);

/*
 * One TRB at an offset of the area: address, size and control with HWO clear first, out to memory, then HWO set and
 * out again -- the controller never meets a TRB it owns with half of it old (gadget.c:1173's "HWO last").
 */
void vayu_usbnet_trb(vm_offset_t trb, uint64_t buffer_pa, uint32_t size, uint32_t ctrl);
/* The TRB at an offset as the controller left it (for_cpu first): its ctrl and size words. */
void vayu_usbnet_trb_read(vm_offset_t trb, uint32_t *ctrl, uint32_t *size);
/* The SETUP TRB on ep0 OUT, armed again (ep0.c: dwc3_ep0_out_start) -- no state of the ECM side's. */
void vayu_usbnet_setup_arm(void);

/* ---- the ECM side, for the event loop -------------------------------------------------------- */

void vayu_usbnet_ecm_init(void);        /* level 3: en2 and its address, once the controller runs */
void vayu_usbnet_ecm_ep_event(unsigned int phys, uint32_t event, uint32_t status, uint32_t param);
void vayu_usbnet_ecm_disconnect(void);
void vayu_usbnet_ecm_reset(void);
void vayu_usbnet_ecm_connect(void);
void vayu_usbnet_ecm_counters(void);

/* ---- registers the ECM side touches (the rest is vayu_usbnet.c's) ----------------------------- */

#define USB_DCFG                0xc700  /* DCFG: SET_ADDRESS writes DEVADDR here (ep0.c:649) */
#define USB_DALEPENA            0xc720  /* DALEPENA: one bit per physical endpoint */
#define USB_DCFG_DEVADDR        (0x7fu << 3)

/* ---- TRB control, core.h:730-747 ------------------------------------------------------------- */

#define USB_TRB_HWO             (1u << 0)
#define USB_TRB_LST             (1u << 1)
#define USB_TRB_CHN             (1u << 2)
#define USB_TRB_CSP             (1u << 3)
#define USB_TRB_TYPE(n)         (((n) & 0x3f) << 4)
#define USB_TRB_ISP_IMI         (1u << 10)
#define USB_TRB_IOC             (1u << 11)

#define USB_TRBCTL_NORMAL       1
#define USB_TRBCTL_CONTROL_SETUP 2
#define USB_TRBCTL_CONTROL_STATUS2 3
#define USB_TRBCTL_CONTROL_STATUS3 4
#define USB_TRBCTL_CONTROL_DATA 5
#define USB_TRBCTL_LINK         8

struct vayu_usbnet_trb {
	uint32_t bpl;
	uint32_t bph;
	uint32_t size;
	uint32_t ctrl;
};

/* ---- endpoint commands and their parameters, core.h:475-505, gadget.h:31-54 ------------------- */

#define USB_DEPCMD_DEPSTARTCFG      0x09
#define USB_DEPCMD_ENDTRANSFER      0x08
#define USB_DEPCMD_UPDATETRANSFER   0x07
#define USB_DEPCMD_STARTTRANSFER    0x06
#define USB_DEPCMD_CLEARSTALL       0x05
#define USB_DEPCMD_SETSTALL         0x04
#define USB_DEPCMD_SETTRANSFRESOURCE 0x02
#define USB_DEPCMD_SETEPCONFIG      0x01

#define USB_DEPCMD_CMDIOC       (1u << 8)
#define USB_DEPCMD_CMDACT       (1u << 10)
#define USB_DEPCMD_FORCERM      (1u << 11)      /* HIPRI_FORCERM, for ENDTRANSFER */
#define USB_DEPCMD_PARAM(n)     ((uint32_t)(n) << 16)
#define USB_DEPCMD_GET_RSC(n)   (((n) >> 16) & 0x7f)

#define USB_DEPCFG_TYPE_CONTROL 0
#define USB_DEPCFG_TYPE_BULK    2
#define USB_DEPCFG_TYPE_INTR    3

#define USB_DEPCFG_XFER_COMPLETE_EN     (1u << 8)
#define USB_DEPCFG_XFER_IN_PROGRESS_EN  (1u << 9)
#define USB_DEPCFG_XFER_NOT_READY_EN    (1u << 10)

/* Endpoint events, core.h:1257-1264. */
#define USB_DEPEVT_XFERCOMPLETE     1
#define USB_DEPEVT_XFERINPROGRESS   2
#define USB_DEPEVT_XFERNOTREADY     3
#define USB_DEPEVT_EPCMDCMPLT       7

/* ---- the DMA area (vayu_usbnet.c allocates it) ---------------------------------------------------- */

#define USB_EVENT_SIZE          4096
#define USB_TRB_SLOTS           32      /* slots per bulk ring, each a data TRB of its own */
#define USB_FRAME_MAX           2048
#define USB_MAXPACKET           512

/*
 * The cache line the layout is cut in (Kryo 485: CTR_EL0.DminLine and CWG are 64 bytes; vayu_usbnet.c refuses to
 * start on a core whose line or writeback granule is larger).
 *
 * The 8796 line kept the TRBs of a ring 16 bytes apart and made the whole area uncached; on 12377 the physical
 * aperture keeps a write-back alias of every page (sys/vayu_dma_compat.h, rule 2), so the area stays write-back and
 * no cache line may hold bytes that the CPU and the controller write at the same time (rule 3). A bulk ring is
 * therefore one 64-byte slot per data TRB: the data TRB at +0, a link TRB at +16 to the next slot (the last slot's
 * to the first), the rest unused. The controller follows the links as it follows the one link TRB of Linux's rings
 * (gadget.c:818: HWO set once and never cleared); the CPU writes a slot only while the controller does not own its
 * data TRB, and reads it only after vayu_usbnet_for_cpu().
 */
#define USB_DMA_LINE            64
#define USB_RING_SLOT           USB_DMA_LINE
#define USB_RING_LINK           16      /* the link TRB, within a slot */

#define USB_DMA_EVENTS          0x0000      /* 4096: the controller's */
#define USB_DMA_EP0_TRB         0x1000      /* one line */
#define USB_DMA_EP0_SETUP       0x1040      /* one line: the controller's */
#define USB_DMA_EP0_DATA        0x1080      /* 512 bytes */
#define USB_DMA_NOTIFY_TRB      0x1280      /* one line */
#define USB_DMA_NOTIFY_DATA     0x12c0      /* one line */
#define USB_DMA_OUT_TRB         0x1400      /* 32 slots of 64 */
#define USB_DMA_IN_TRB          0x1c00      /* 32 slots of 64 */
#define USB_DMA_OUT_DATA        0x4000      /* 32 x 2048: the controller's */
#define USB_DMA_IN_DATA         0x14000     /* 32 x 2048 */
#define USB_DMA_SIZE            0x24000

#define USB_OUT_SLOT(n)         (USB_DMA_OUT_TRB + USB_RING_SLOT * (n))
#define USB_IN_SLOT(n)          (USB_DMA_IN_TRB + USB_RING_SLOT * (n))

/* ---- the physical endpoints: phys = 2 * epnum + direction (IN = 1), gadget.c:700 --------------- */

#define USB_EP_EP0OUT           0
#define USB_EP_EP0IN            1
#define USB_EP_NOTIFY           3       /* EP1 IN: the CDC notifications, 16 bytes */
#define USB_EP_OUT              4       /* EP2 OUT: the host's frames */
#define USB_EP_IN               5       /* EP2 IN: ours */

#endif /* _SYS_VAYU_USBNET_H_ */
