/*
 * The USB controller as UEFI left it (POCO X3 Pro, vayu, SM8150), bsd/dev/vayu_usbnet.c: a look, reading
 * only, at the DWC3 core and its QSCRATCH glue, the GCC USB clocks, the Synopsys femto HS PHY and the apps
 * SMMU.
 *
 * Why nothing is written: during this run the loader's UEFI USB stack drives this controller from core 0
 * (dwc3-msm, the same event buffers, endpoints and clocks), so a write here -- GEVNTCOUNT, DEPCMD, a clock
 * or a reset -- would take the controller away from it. Only 32-bit volatile reads are done. The SMMU scan
 * writes nothing either (no SMR/S2CR is claimed); it only reports what the boot chain set up.
 *
 * Boot-arg vayu_usbnet=1: one look 15 s after the start, another 45 s later, then the thread ends.
 *
 * vayu_usbnet=2 and =3 (which also need usb_handover=1) take the controller over instead: the
 * loader stops touching USB, and the kernel enumerates as a CDC-ECM device (2) and carries frames
 * between the host's usb0 and en2 (3). Above DWC3 that is bsd/dev/vayu_usbnet_ecm.c; the sequence
 * below is Linux's drivers/usb/dwc3, polled from one thread -- no interrupt is attached in this
 * tree (see hakios.ufshc.c, hakios.net.c).
 *
 * Where everything is:
 *   DWC3 core + glue  0x0a600000, 0x100000   vendor arch/arm64/boot/dts/qcom/sm8150-usb.dtsi:21-22;
 *                                           DWC3 regs at +0xc100.., QSCRATCH at +0xf8800,
 *                                           drivers/usb/dwc3/dwc3-msm.c:84
 *   GCC               0x00100000, 0x100000   vayu_qup_spi.c's GCC_PHYS; the USB registers from vendor
 *                                           drivers/clk/qcom/gcc-sm8150.c and sm8150-gdsc.dtsi:51-56
 *   HS PHY            0x088e2000, 0x1000     sm8150-usb.dtsi:141-144 (Synopsys femto PHY)
 *   apps SMMU         0x15000000, 0x100000   vayu_qup_spi.c's APPS_SMMU_PHYS/SIZE; the USB SID 0x140
 *                                           from sm8150-usb.dtsi:24 iommus = <&apps_smmu 0x140 0x0>
 *
 * Every line also goes into the loader's log (tee_printf).
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_usbnet.h>
#include <vm/vm_kern.h>

#ifndef VM_WIMG_WCOMB
#define VM_WIMG_WCOMB           0x6     /* osfmk/arm/memory_types.h: normal memory, not cached */
#endif

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void IOSleep(unsigned int milliseconds);
extern void loader_log_putc(char c);
extern uint64_t kvtophys(vm_offset_t va);
extern void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);
/* loader_return.c: the handover's two flags, and what the loader publishes. */
extern uint64_t loader_live_usb_take(void);
extern uint64_t loader_live_usb_given(void);
extern void loader_live_usb_take_set(uint64_t value);

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

#define DWC3_PHYS               0x0a600000UL
#define DWC3_SIZE               0x100000UL
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x100000UL
#define HSPHY_PHYS              0x088e2000UL
#define HSPHY_SIZE              0x1000UL
#define SMMU_PHYS               0x15000000UL
#define SMMU_SIZE               0x100000UL

#define GSBUSCFG0               0xc100
#define GSBUSCFG1               0xc104
#define GCTL                    0xc110
#define GSTS                    0xc118
#define GUCTL1                  0xc11c
#define GSNPSID                 0xc120
#define GUID                    0xc128
#define GUCTL                   0xc12c
#define GDBGLTSSM               0xc164
#define GUCTL2                  0xc19c
#define GUSB2PHYCFG0            0xc200
#define GUSB3PIPECTL0           0xc2c0
#define GHWPARAMS0              0xc140
#define GHWPARAMS8              0xc600
#define GFLADJ                  0xc630
#define GTXFIFOSIZ(n)           (0xc300 + 4 * (n))
#define GRXFIFOSIZ(n)           (0xc380 + 4 * (n))
#define GEVNTADRLO(n)           (0xc400 + 16 * (n))
#define GEVNTADRHI(n)           (0xc404 + 16 * (n))
#define GEVNTSIZ(n)             (0xc408 + 16 * (n))
#define GEVNTCOUNT(n)           (0xc40c + 16 * (n))
#define DCFG                    0xc700
#define DCTL                    0xc704
#define DEVTEN                  0xc708
#define DSTS                    0xc70c
#define DGCMDPAR                0xc710
#define DGCMD                   0xc714
#define DALEPENA                0xc720
#define DEP(n, o)               (0xc800 + 16 * (n) + (o))

#define QSCRATCH(o)             (0xf8800 + (o))
#define GENERAL_CFG             0x08
#define HS_PHY_CTRL             0x10
#define CGCTL                   0x28
#define SS_PHY_CTRL             0x30
#define PWR_EVNT_IRQ_STAT       0x58
#define PWR_EVNT_IRQ_MASK       0x5c

#define USB30_PRIM_BCR          0xf000
#define USB30_PRIM_GDSCR        0xf004
#define USB30_PRIM_MASTER_CBCR  0xf010
#define USB30_PRIM_SLEEP_CBCR   0xf014
#define USB30_PRIM_MOCK_UTMI_CBCR 0xf018
#define USB3_PRIM_PHY_AUX_CBCR  0xf050
#define USB3_PRIM_PHY_COM_AUX_CBCR 0xf054
#define USB3_PRIM_PHY_PIPE_CBCR 0xf058
#define CFG_NOC_USB3_PRIM_AXI_CBCR 0xf078
#define AGGRE_USB3_PRIM_AXI_CBCR 0xf07c
#define USB3_PRIM_CLKREF        0x8c008
#define USB3_SEC_CLKREF         0x8c028
#define QUSB2PHY_PRIM_BCR       0x12000

#define SMMU_SCR0               0x0
#define SMMU_ID0                0x20
#define SMMU_ID1                0x24
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CB_SCTLR           0x0
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TCR             0x30

static vm_offset_t dwc3, gcc, hsphy, smmu;

static inline uint32_t
rd(vm_offset_t base, vm_offset_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void
dump(const char *name, vm_offset_t base, uint32_t from, uint32_t to)
{
	for (uint32_t o = from; o < to; o += 0x20) {
		printf("vayu-usbnet: %s +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", name, o, rd(base, o), rd(base, o + 4),
		    rd(base, o + 8), rd(base, o + 12), rd(base, o + 16), rd(base, o + 20), rd(base, o + 24), rd(base, o + 28));
	}
}

static void
gcc_cbcr(const char *name, vm_offset_t off)
{
	uint32_t value = rd(gcc, off);

	printf("vayu-usbnet: %s %08x %s\n", name, value, (value >> 31) ? "OFF" : "on");
}

/* The read-only SMR/S2CR scan of vayu_qup_spi.c: USFCFG/CLIENTPD when nothing matches. */
static void
smmu_sid(uint32_t sid)
{
	uint32_t scr0 = rd(smmu, SMMU_SCR0), id0 = rd(smmu, SMMU_ID0), id1 = rd(smmu, SMMU_ID1);
	uint32_t numsmrg = id0 & 0xff, pgshift = (id1 & (1u << 31)) ? 16 : 12;
	uint32_t numpage = 1u << (((id1 >> 28) & 7) + 1);
	bool exids = (scr0 & (1u << 3)) != 0, found = false;

	printf("vayu-usbnet: SMMU sCR0 %08x ID0 %08x (NUMSMRG %u, EXIDS %u) ID1 %08x (NUMCB %u)\n", scr0, id0,
	    numsmrg, (id0 >> 8) & 1, id1, id1 & 0xff);
	for (uint32_t i = 0; i < numsmrg; i++) {
		uint32_t smr = rd(smmu, SMMU_SMR(i)), s2cr = rd(smmu, SMMU_S2CR(i));
		bool valid = exids ? (s2cr & (1u << 10)) != 0 : (smr & (1u << 31)) != 0;
		uint32_t id = smr & 0xffff, mask = (smr >> 16) & (exids ? 0xffff : 0x7fff);

		if (!valid || ((sid ^ id) & ~mask & 0xffff) != 0) {
			continue;
		}
		found = true;
		uint32_t type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		printf("vayu-usbnet: SMMU SID %#x matches SMR%u %08x, S2CR %08x: %s, cb %u\n", sid, i, smr, s2cr,
		    type == 0 ? "TRANSLATE" : type == 1 ? "BYPASS" : type == 2 ? "FAULT" : "?", cb);
		if (type == 0 && ((numpage + cb + 1) << pgshift) <= SMMU_SIZE) {
			vm_offset_t cbase = smmu + ((numpage + cb) << pgshift);

			printf("vayu-usbnet: SMMU cb %u: sctlr %08x ttbr0 %08x_%08x tcr %08x\n", cb, rd(cbase, SMMU_CB_SCTLR),
			    rd(cbase, SMMU_CB_TTBR0 + 4), rd(cbase, SMMU_CB_TTBR0), rd(cbase, SMMU_CB_TCR));
		}
	}
	if (!found) {
		printf("vayu-usbnet: SMMU SID %#x: no valid SMR matches; sCR0.USFCFG %u (1 = fault, 0 = bypass), "
		    "CLIENTPD %u\n", sid, (scr0 >> 10) & 1, scr0 & 1);
	}
}

static void
look(unsigned when)
{
	uint32_t hwparams0 = 0, hwparams3 = 0, gsnpsid, gctl;

	printf("vayu-usbnet: look %u: DWC3 at %#lx, read only\n", when, DWC3_PHYS);
	printf("vayu-usbnet: GSBUSCFG0 %08x\n", rd(dwc3, GSBUSCFG0));
	printf("vayu-usbnet: GSBUSCFG1 %08x\n", rd(dwc3, GSBUSCFG1));
	gctl = rd(dwc3, GCTL);
	printf("vayu-usbnet: GCTL %08x\n", gctl);
	printf("vayu-usbnet: GUCTL1 %08x\n", rd(dwc3, GUCTL1));
	printf("vayu-usbnet: GSTS %08x\n", rd(dwc3, GSTS));
	gsnpsid = rd(dwc3, GSNPSID);
	printf("vayu-usbnet: GSNPSID %08x\n", gsnpsid);
	printf("vayu-usbnet: GUID %08x\n", rd(dwc3, GUID));
	printf("vayu-usbnet: GUCTL %08x\n", rd(dwc3, GUCTL));
	printf("vayu-usbnet: GDBGLTSSM %08x\n", rd(dwc3, GDBGLTSSM));
	printf("vayu-usbnet: GUCTL2 %08x\n", rd(dwc3, GUCTL2));
	printf("vayu-usbnet: GUSB2PHYCFG0 %08x\n", rd(dwc3, GUSB2PHYCFG0));
	printf("vayu-usbnet: GUSB3PIPECTL0 %08x\n", rd(dwc3, GUSB3PIPECTL0));
	for (unsigned i = 0; i < 8; i++) {
		uint32_t value = rd(dwc3, GHWPARAMS0 + 4 * i);

		if (i == 0) {
			hwparams0 = value;
		} else if (i == 3) {
			hwparams3 = value;
		}
		printf("vayu-usbnet: GHWPARAMS%u %08x\n", i, value);
	}
	printf("vayu-usbnet: GHWPARAMS8 %08x\n", rd(dwc3, GHWPARAMS8));
	printf("vayu-usbnet: GFLADJ %08x\n", rd(dwc3, GFLADJ));
	printf("vayu-usbnet: GSNPSID core id %04x (%s), revision %04x\n", gsnpsid >> 16,
	    (gsnpsid >> 16) == 0x5533 ? "DWC3" : (gsnpsid >> 16) == 0x3331 ? "DWC3.1" : "?", gsnpsid & 0xffff);
	printf("vayu-usbnet: GCTL.PRTCAPDIR %u (%s)\n", (gctl >> 12) & 3,
	    ((gctl >> 12) & 3) == 1 ? "host" : ((gctl >> 12) & 3) == 2 ? "device" : ((gctl >> 12) & 3) == 3 ? "otg" : "?");
	printf("vayu-usbnet: GHWPARAMS3 DWC3_NUM_EPS %u\n", (hwparams3 >> 18) & 0x1f);
	printf("vayu-usbnet: GHWPARAMS0 mode %u (%s)\n", hwparams0 & 7,
	    (hwparams0 & 7) == 0 ? "device" : (hwparams0 & 7) == 1 ? "host" : (hwparams0 & 7) == 2 ? "drd" : "?");

	printf("vayu-usbnet: GTXFIFOSIZ 0-7: %08x %08x %08x %08x %08x %08x %08x %08x\n", rd(dwc3, GTXFIFOSIZ(0)),
	    rd(dwc3, GTXFIFOSIZ(1)), rd(dwc3, GTXFIFOSIZ(2)), rd(dwc3, GTXFIFOSIZ(3)), rd(dwc3, GTXFIFOSIZ(4)),
	    rd(dwc3, GTXFIFOSIZ(5)), rd(dwc3, GTXFIFOSIZ(6)), rd(dwc3, GTXFIFOSIZ(7)));
	printf("vayu-usbnet: GTXFIFOSIZ 8-15: %08x %08x %08x %08x %08x %08x %08x %08x\n", rd(dwc3, GTXFIFOSIZ(8)),
	    rd(dwc3, GTXFIFOSIZ(9)), rd(dwc3, GTXFIFOSIZ(10)), rd(dwc3, GTXFIFOSIZ(11)), rd(dwc3, GTXFIFOSIZ(12)),
	    rd(dwc3, GTXFIFOSIZ(13)), rd(dwc3, GTXFIFOSIZ(14)), rd(dwc3, GTXFIFOSIZ(15)));
	printf("vayu-usbnet: GRXFIFOSIZ 0-2: %08x %08x %08x\n", rd(dwc3, GRXFIFOSIZ(0)), rd(dwc3, GRXFIFOSIZ(1)),
	    rd(dwc3, GRXFIFOSIZ(2)));

	for (unsigned i = 0; i < 4; i++) {
		printf("vayu-usbnet: GEVNT%u ADRLO %08x ADRHI %08x SIZ %08x COUNT %08x\n", i, rd(dwc3, GEVNTADRLO(i)),
		    rd(dwc3, GEVNTADRHI(i)), rd(dwc3, GEVNTSIZ(i)), rd(dwc3, GEVNTCOUNT(i)));
	}

	{
		uint32_t dcfg = rd(dwc3, DCFG), dctl = rd(dwc3, DCTL), dsts = rd(dwc3, DSTS);

		printf("vayu-usbnet: DCFG %08x (DEVADDR %u, SPEED %u)\n", dcfg, (dcfg >> 3) & 0x7f, dcfg & 7);
		printf("vayu-usbnet: DCTL %08x (RUN_STOP %u)\n", dctl, dctl >> 31);
		printf("vayu-usbnet: DEVTEN %08x\n", rd(dwc3, DEVTEN));
		printf("vayu-usbnet: DSTS %08x (CONNECTSPD %u %s, USBLNKST %u, DEVCTRLHLT %u)\n", dsts, dsts & 7,
		    (dsts & 7) == 0 ? "HS" : (dsts & 7) == 1 ? "FS" : (dsts & 7) == 4 ? "SS" : "?", (dsts >> 18) & 0xf,
		    (dsts >> 22) & 1);
		printf("vayu-usbnet: DGCMDPAR %08x\n", rd(dwc3, DGCMDPAR));
		printf("vayu-usbnet: DGCMD %08x\n", rd(dwc3, DGCMD));
		printf("vayu-usbnet: DALEPENA %08x\n", rd(dwc3, DALEPENA));
	}

	for (unsigned n = 0; n < 8; n++) {
		printf("vayu-usbnet: DEP%u PAR2 %08x PAR1 %08x PAR0 %08x CMD %08x\n", n, rd(dwc3, DEP(n, 0)),
		    rd(dwc3, DEP(n, 4)), rd(dwc3, DEP(n, 8)), rd(dwc3, DEP(n, 0xc)));
	}

	printf("vayu-usbnet: QSCRATCH GENERAL_CFG %08x\n", rd(dwc3, QSCRATCH(GENERAL_CFG)));
	printf("vayu-usbnet: QSCRATCH HS_PHY_CTRL %08x\n", rd(dwc3, QSCRATCH(HS_PHY_CTRL)));
	printf("vayu-usbnet: QSCRATCH CGCTL %08x\n", rd(dwc3, QSCRATCH(CGCTL)));
	printf("vayu-usbnet: QSCRATCH SS_PHY_CTRL %08x\n", rd(dwc3, QSCRATCH(SS_PHY_CTRL)));
	printf("vayu-usbnet: QSCRATCH PWR_EVNT_IRQ_STAT %08x\n", rd(dwc3, QSCRATCH(PWR_EVNT_IRQ_STAT)));
	printf("vayu-usbnet: QSCRATCH PWR_EVNT_IRQ_MASK %08x\n", rd(dwc3, QSCRATCH(PWR_EVNT_IRQ_MASK)));

	gcc_cbcr("USB30_PRIM_MASTER_CBCR", USB30_PRIM_MASTER_CBCR);
	gcc_cbcr("USB30_PRIM_SLEEP_CBCR", USB30_PRIM_SLEEP_CBCR);
	gcc_cbcr("USB30_PRIM_MOCK_UTMI_CBCR", USB30_PRIM_MOCK_UTMI_CBCR);
	gcc_cbcr("USB3_PRIM_PHY_AUX_CBCR", USB3_PRIM_PHY_AUX_CBCR);
	gcc_cbcr("USB3_PRIM_PHY_COM_AUX_CBCR", USB3_PRIM_PHY_COM_AUX_CBCR);
	gcc_cbcr("USB3_PRIM_PHY_PIPE_CBCR", USB3_PRIM_PHY_PIPE_CBCR);
	gcc_cbcr("CFG_NOC_USB3_PRIM_AXI_CBCR", CFG_NOC_USB3_PRIM_AXI_CBCR);
	gcc_cbcr("AGGRE_USB3_PRIM_AXI_CBCR", AGGRE_USB3_PRIM_AXI_CBCR);
	gcc_cbcr("USB3_PRIM_CLKREF", USB3_PRIM_CLKREF);
	gcc_cbcr("USB3_SEC_CLKREF", USB3_SEC_CLKREF);
	{
		uint32_t gdscr = rd(gcc, USB30_PRIM_GDSCR);

		printf("vayu-usbnet: USB30_PRIM_GDSCR %08x PWR_ON %u\n", gdscr, gdscr >> 31);
	}
	printf("vayu-usbnet: USB30_PRIM_BCR %08x\n", rd(gcc, USB30_PRIM_BCR));
	printf("vayu-usbnet: QUSB2PHY_PRIM_BCR %08x\n", rd(gcc, QUSB2PHY_PRIM_BCR));

	/* (the SMMU first: if a PHY read stalls the bus, its answer is in the log already) */
	if (when == 1) {
		smmu_sid(0x140);
	}
	dump("hsphy", hsphy, 0x00, 0x110);

	{
		uint32_t lo = rd(dwc3, GEVNTADRLO(0)), hi = rd(dwc3, GEVNTADRHI(0)), siz = rd(dwc3, GEVNTSIZ(0)) & 0xffff;

		if ((lo != 0 || hi != 0) && siz >= 4 && siz <= 65536) {
			printf("vayu-usbnet: UEFI event buffer 0 at %08x_%08x, %u bytes, not mapped\n", hi, lo, siz);
		}
	}
	printf("vayu-usbnet: look %u done, nothing written\n", when);
}

/* ---- levels 2 and 3: the controller taken from the loader ------------------------------------ */

#define USB_TAKE_WAIT_MS        3000    /* our patience with the loader's usb_given */
#define USB_TAKE_STEP_MS        10

/* The bits of the registers the sequence below reads and writes (drivers/usb/dwc3/core.h). */
#define DCTL_RUN_STOP           (1u << 31)
#define DCTL_CSFTRST            (1u << 30)
#define DSTS_DEVCTRLHLT         (1u << 22)
#define GCTL_PRTCAP_MASK        (3u << 12)
#define GCTL_PRTCAP_DEVICE      (2u << 12)
#define GUSB2PHYCFG_SUSPHY      (1u << 6)
#define GUSB2PHYCFG_ENBLSLPM    (1u << 8)
#define DCFG_SPEED_MASK         0x7u
#define DCFG_DEVADDR_MASK       USB_DCFG_DEVADDR
#define DEVTEN_DISCONNECT       (1u << 0)
#define DEVTEN_USBRST           (1u << 1)
#define DEVTEN_CONNECTDONE      (1u << 2)
#define DEVTEN_ULSTCNG          (1u << 3)
#define GEVNTSIZ_INTMASK        (1u << 31)

/* A device event's type and an endpoint event's fields (core.h:1270-1334). */
#define DEVICE_EVENT_TYPE(raw)  (((raw) >> 8) & 0xf)
#define DEPEVT_PHYS(raw)        (((raw) >> 1) & 0x1f)
#define DEPEVT_EVENT(raw)       (((raw) >> 6) & 0xf)
#define DEPEVT_STATUS(raw)      (((raw) >> 12) & 0xf)
#define DEPEVT_PARAM(raw)       ((raw) >> 16)

static uint32_t usb_level;              /* vayu_usbnet=: what the thread was started for */
static uint32_t usb_delay = 30;         /* usbnet_delay=: seconds before the loader is asked */
static uint32_t usb_ip = 0x0a0b0002;    /* usbnet_ip=: 10.11.0.2, one uint32 */

static vm_offset_t usb_dma;             /* the rings: physically contiguous, uncached */
static uint64_t usb_dma_pa;
static uint32_t usb_event_at;           /* the running offset in event buffer 0 */
static uint64_t usb_events;             /* how many events have been processed */
static uint32_t usb_link_state = 0xffffffffu;  /* the last USBLNKST said */
static volatile bool usb_taken;         /* this kernel has the controller: quiesce has work */
static volatile bool usb_quiesced;      /* ... and must not be touched any more */

uint32_t
vayu_usbnet_level(void)
{
	return usb_level;
}

uint32_t
vayu_usbnet_ip(void)
{
	return usb_ip;
}

uint64_t
vayu_usbnet_events(void)
{
	return usb_events;
}

uint32_t
vayu_usbnet_rd(vm_offset_t offset)
{
	return rd(dwc3, offset);
}

void
vayu_usbnet_wr(vm_offset_t offset, uint32_t value)
{
	*(volatile uint32_t *)(dwc3 + offset) = value;
}

/* Every write of the bring-up, in the log: what the loader's UEFI had set can be read from it. */
static void
reg_write_logged(const char *name, vm_offset_t offset, uint32_t value)
{
	uint32_t old = rd(dwc3, offset);

	*(volatile uint32_t *)(dwc3 + offset) = value;
	printf("vayu-usbnet: w %s %08x -> %08x\n", name, old, value);
}

/*
 * The rings. The task's sketch said 64 KB for the whole area, but 32 x 2048-byte buffers in each
 * direction is 128 KB by itself, so the block is what the layout in vayu_usbnet.h adds up to
 * (USB_DMA_SIZE): one physically contiguous kmem_alloc_contig() piece, because the
 * controller is given physical addresses.
 */
static bool
dma_area(void)
{
	vm_size_t size = round_page(USB_DMA_SIZE);
	uint64_t pa;

	if (kmem_alloc_contig(kernel_map, &usb_dma, size, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("vayu-usbnet: no %lu contiguous bytes for the rings\n", (unsigned long)size);
		return false;
	}
	pa = kvtophys(usb_dma);
	if (pa == 0) {
		printf("vayu-usbnet: the rings at %#lx have no physical address\n", (unsigned long)usb_dma);
		return false;
	}
	for (vm_offset_t offset = 0; offset < size; offset += PAGE_SIZE) {
		if (kvtophys(usb_dma + offset) != pa + offset) {
			printf("vayu-usbnet: the rings are not contiguous: page %lu at %#llx, expected %#llx\n",
			    (unsigned long)(offset >> PAGE_SHIFT), kvtophys(usb_dma + offset), pa + offset);
			return false;
		}
	}
	usb_dma_pa = pa;
	/*
	 * Uncacheable for the CPU from here on (VM_WIMG_WCOMB: normal memory, not cached), every mapping
	 * of these pages, the physical aperture's included (pmap.c pmap_update_cache_attributes_locked; the
	 * aperture is page-granular for exactly this, arm_vm_init.c arm_vm_physmap_init): TRBs 16 bytes
	 * apart share cache lines with each other, and a write-back of one would undo what the controller
	 * wrote into its neighbour. The zeroing's dirty lines go to memory first.
	 */
	for (vm_offset_t line = usb_dma; line < usb_dma + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (vm_offset_t offset = 0; offset < size; offset += PAGE_SIZE) {
		pmap_set_cache_attributes((ppnum_t)atop(pa + offset), VM_WIMG_WCOMB);
	}
	printf("vayu-usbnet: the rings: %lu bytes at %#lx, physical %#llx, uncached\n", (unsigned long)size,
	    (unsigned long)usb_dma, pa);
	return true;
}

vm_offset_t
vayu_usbnet_dma_va(vm_offset_t offset)
{
	return usb_dma + offset;
}

uint64_t
vayu_usbnet_dma_pa(vm_offset_t offset)
{
	return usb_dma_pa + offset;
}

/*
 * The area is uncached (dma_area()), so what is left of "clean" and "invalidate" is the ordering:
 * our stores out before the controller is told (a doorbell register write), the controller's
 * writes seen before we read on.
 */
void
vayu_usbnet_clean(__unused vm_offset_t va, __unused vm_size_t len)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

void
vayu_usbnet_inval(__unused vm_offset_t va, __unused vm_size_t len)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

void
vayu_usbnet_trb(vm_offset_t trb, uint64_t buffer_pa, uint32_t size, uint32_t ctrl)
{
	struct vayu_usbnet_trb *entry = (struct vayu_usbnet_trb *)vayu_usbnet_dma_va(trb);

	entry->bpl = (uint32_t)buffer_pa;
	entry->bph = (uint32_t)(buffer_pa >> 32);
	entry->size = size;
	/* HWO last: the core must not fetch half a TRB (gadget.c:1173); uncached normal memory may
	 * reorder stores, hence the barrier */
	__asm__ volatile ("dmb oshst" ::: "memory");
	entry->ctrl = ctrl;
	vayu_usbnet_clean((vm_offset_t)entry, sizeof(*entry));
}

/*
 * One endpoint command, polled to completion: gadget.c:397 dwc3_send_gadget_ep_cmd. In high speed
 * SUSPHY and ENBLSLPM hold endpoint commands off, so they are lifted for the command and put back
 * after it (gadget.c:411-436). The resource index a STARTTRANSFER is answered with is what
 * UPDATETRANSFER and ENDTRANSFER want back (core.h:475-505).
 */
int
vayu_usbnet_ep_cmd(unsigned int phys, uint32_t cmd, uint32_t param0, uint32_t param1, uint32_t param2)
{
	return vayu_usbnet_ep_cmd_rsc(phys, cmd, param0, param1, param2, NULL);
}

int
vayu_usbnet_ep_cmd_rsc(unsigned int phys, uint32_t cmd, uint32_t param0, uint32_t param1, uint32_t param2,
    uint32_t *rsc)
{
	uint32_t cfg = rd(dwc3, GUSB2PHYCFG0), saved = 0, reg = 0;
	bool done = false;

	if ((cfg & GUSB2PHYCFG_SUSPHY) != 0) {
		saved |= GUSB2PHYCFG_SUSPHY;
		cfg &= ~GUSB2PHYCFG_SUSPHY;
	}
	if ((cfg & GUSB2PHYCFG_ENBLSLPM) != 0) {
		saved |= GUSB2PHYCFG_ENBLSLPM;
		cfg &= ~GUSB2PHYCFG_ENBLSLPM;
	}
	if (saved != 0) {
		vayu_usbnet_wr(GUSB2PHYCFG0, cfg);
	}
	vayu_usbnet_wr(DEP(phys, 8), param0);
	vayu_usbnet_wr(DEP(phys, 4), param1);
	vayu_usbnet_wr(DEP(phys, 0), param2);
	if ((cmd & 0xf) == USB_DEPCMD_UPDATETRANSFER) {
		/* the no-response form (gadget.c:452): the TRB cache is updated, nothing is waited for */
		vayu_usbnet_wr(DEP(phys, 0xc), cmd & ~USB_DEPCMD_CMDIOC);
		if (saved != 0) {
			vayu_usbnet_wr(GUSB2PHYCFG0, cfg | saved);
		}
		if (rsc != NULL) {
			*rsc = 0;
		}
		return 0;
	}
	vayu_usbnet_wr(DEP(phys, 0xc), cmd | USB_DEPCMD_CMDACT);
	for (unsigned int i = 0; i < 100; i++) {        /* 100 x 100 us: 10 ms */
		reg = rd(dwc3, DEP(phys, 0xc));
		if ((reg & USB_DEPCMD_CMDACT) == 0) {
			done = true;
			break;
		}
		delay(100);
	}
	if (saved != 0) {
		vayu_usbnet_wr(GUSB2PHYCFG0, cfg | saved);
	}
	if (!done) {
		printf("vayu-usbnet: ep%u: command %#x does not complete in 10 ms\n", phys, cmd & 0xf);
		return 0x10;
	}
	if (rsc != NULL) {
		*rsc = USB_DEPCMD_GET_RSC(reg);
	}
	if (((reg >> 12) & 0xf) != 0) {
		printf("vayu-usbnet: ep%u: command %#x: status %u\n", phys, cmd & 0xf, (reg >> 12) & 0xf);
		return (int)((reg >> 12) & 0xf);
	}
	return 0;
}

int
vayu_usbnet_ep_config(unsigned int phys, uint32_t type, uint32_t maxpacket, uint32_t fifo, uint32_t binterval,
    bool xfer_in_progress, uint32_t action)
{
	uint32_t param0, param1;

	param0 = ((type & 3) << 1) | ((maxpacket & 0x7ff) << 3) | ((fifo & 0x1f) << 17) | ((action & 3) << 30);
	param1 = USB_DEPCFG_XFER_COMPLETE_EN | USB_DEPCFG_XFER_NOT_READY_EN | ((phys & 0x1f) << 25);
	if (xfer_in_progress) {
		param1 |= USB_DEPCFG_XFER_IN_PROGRESS_EN;
	}
	if (binterval != 0) {
		param1 |= ((binterval - 1) & 0xff) << 16;
	}
	return vayu_usbnet_ep_cmd(phys, USB_DEPCMD_SETEPCONFIG, param0, param1, 0);
}

void
vayu_usbnet_setup_arm(void)
{
	uint64_t trb = vayu_usbnet_dma_pa(USB_DMA_EP0_TRB);

	vayu_usbnet_trb(USB_DMA_EP0_TRB, vayu_usbnet_dma_pa(USB_DMA_EP0_SETUP), 8,
	    USB_TRB_HWO | USB_TRB_LST | USB_TRB_ISP_IMI | USB_TRB_IOC | USB_TRB_TYPE(USB_TRBCTL_CONTROL_SETUP));
	if (vayu_usbnet_ep_cmd(USB_EP_EP0OUT, USB_DEPCMD_STARTTRANSFER, (uint32_t)(trb >> 32), (uint32_t)trb, 0) != 0) {
		printf("vayu-usbnet: ep0: the SETUP transfer cannot be started\n");
	}
}

static bool
wait_bits(vm_offset_t offset, uint32_t mask, bool set, unsigned int ms)
{
	for (unsigned int i = 0; i < ms; i++) {
		if (((rd(dwc3, offset) & mask) != 0) == set) {
			return true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return false;
}

/* The loader's half of the handover (xnuloader/loader.c: kernel_live's usb_take/usb_given). */
static bool
usb_handover(void)
{
	unsigned int waited;

	loader_live_usb_take_set(1);
	__builtin_arm_dsb(0xf);
	for (waited = 0; waited < USB_TAKE_WAIT_MS; waited += USB_TAKE_STEP_MS) {
		if (loader_live_usb_given() != 0) {
			printf("vayu-usbnet: the loader gave USB up after %u ms\n", waited);
			return true;
		}
		delay_for_interval(USB_TAKE_STEP_MS, NSEC_PER_MSEC);
	}
	printf("vayu-usbnet: the loader did not give USB up in 3 s (an older loader?): left alone\n");
	loader_live_usb_take_set(0);
	return false;
}

static bool
dwc3_endpoints(void)
{
	/* (gadget.c:639 dwc3_gadget_start_config: XferRscIdx 0, then a resource for every ep) */
	if (vayu_usbnet_ep_cmd(USB_EP_EP0OUT, USB_DEPCMD_DEPSTARTCFG, 0, 0, 0) != 0) {
		printf("vayu-usbnet: DEPSTARTCFG failed\n");
		return false;
	}
	/* EP0 OUT and IN: control, 64-byte packets (dwc3_gadget_set_ep_config, gadget.c:670) */
	if (vayu_usbnet_ep_config(USB_EP_EP0OUT, USB_DEPCFG_TYPE_CONTROL, 64, 0, 0, false, 0) != 0 ||
	    vayu_usbnet_ep_config(USB_EP_EP0IN, USB_DEPCFG_TYPE_CONTROL, 64, 0, 0, false, 0) != 0) {
		printf("vayu-usbnet: ep0 cannot be configured\n");
		return false;
	}
	/* one transfer resource per endpoint, once until the next core reset (gadget.c:742-752): ep0 and
	 * the ECM function's 3, 4, 5 (and 2, unused, so that the indices stay in order) */
	for (unsigned int phys = USB_EP_EP0OUT; phys <= USB_EP_IN; phys++) {
		if (vayu_usbnet_ep_cmd(phys, USB_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) != 0) {
			printf("vayu-usbnet: ep%u has no transfer resource\n", phys);
			return false;
		}
	}
	reg_write_logged("DALEPENA", USB_DALEPENA,
	    vayu_usbnet_rd(USB_DALEPENA) | (1u << USB_EP_EP0OUT) | (1u << USB_EP_EP0IN));
	return true;
}

/*
 * The sequence: stop, reset, device mode, event buffer, high speed, endpoints, SETUP, events, run.
 * Linux's drivers/usb/dwc3 in the same order (the line numbers are in each step).
 */
static bool
dwc3_take(void)
{
	uint32_t gctl, dcfg;

	/*
	 * (1) gadget.c:2150 dwc3_gadget_run_stop(is_on = 0): UEFI's transfers ended first (the data book
	 * wants no active transfer when RUN_STOP drops, gadget.c:2200), then RUN_STOP, then UEFI's events
	 * acknowledged until the core halts: "Controller is not halted until the events are acknowledged"
	 * (gadget.c:2216). Run 051220 (usb2): without that, DEVCTRLHLT never came.
	 */
	reg_write_logged("DEVTEN", DEVTEN, 0);
	for (unsigned int phys = 0; phys < 16; phys++) {
		uint32_t rsc = (rd(dwc3, DEP(phys, 0xc)) >> 16) & 0x7f;     /* the last STARTTRANSFER's index */

		if ((rd(dwc3, DALEPENA) & (1u << phys)) == 0) {
			continue;
		}
		printf("vayu-usbnet: ep%u (UEFI's): ENDTRANSFER rsc %u: %d\n", phys, rsc, vayu_usbnet_ep_cmd(phys,
		    USB_DEPCMD_ENDTRANSFER | USB_DEPCMD_FORCERM | USB_DEPCMD_CMDIOC | USB_DEPCMD_PARAM(rsc), 0, 0, 0));
		delay(100);
	}
	reg_write_logged("DCTL", DCTL, rd(dwc3, DCTL) & ~DCTL_RUN_STOP);
	for (unsigned int ms = 0; ms < 500 && (rd(dwc3, DSTS) & DSTS_DEVCTRLHLT) == 0; ms++) {
		uint32_t pending = rd(dwc3, GEVNTCOUNT(0)) & 0xfffc;

		if (pending != 0) {
			vayu_usbnet_wr(GEVNTCOUNT(0), pending);      /* (UEFI's events, unread: acknowledged) */
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	printf("vayu-usbnet: DSTS %08x, GEVNTCOUNT0 %08x after the stop\n", rd(dwc3, DSTS), rd(dwc3, GEVNTCOUNT(0)));
	if ((rd(dwc3, DSTS) & DSTS_DEVCTRLHLT) == 0) {
		/* (UEFI's transfers may still be active; the core soft reset below stops the device anyway) */
		printf("vayu-usbnet: DSTS.DEVCTRLHLT does not come up in 500 ms; soft reset anyway\n");
	}

	/* (2) core.c:206 dwc3_core_soft_reset */
	reg_write_logged("DCTL", DCTL, rd(dwc3, DCTL) | DCTL_CSFTRST);
	if (!wait_bits(DCTL, DCTL_CSFTRST, false, 100)) {
		printf("vayu-usbnet: DCTL.CSFTRST does not clear\n");
		return false;
	}
	/* the data book's PHY sync delay after a soft reset (core.c:206, then msleep(50)) */
	delay_for_interval(50, NSEC_PER_MSEC);

	/* (3) device mode; GCTL, GUSB2PHYCFG0 and GUSB3PIPECTL0 otherwise as UEFI left them */
	gctl = rd(dwc3, GCTL);
	if ((gctl & GCTL_PRTCAP_MASK) != GCTL_PRTCAP_DEVICE) {
		reg_write_logged("GCTL", GCTL, (gctl & ~GCTL_PRTCAP_MASK) | GCTL_PRTCAP_DEVICE);
	}
	printf("vayu-usbnet: GCTL %08x, GUSB2PHYCFG0 %08x\n", rd(dwc3, GCTL), rd(dwc3, GUSB2PHYCFG0));

	/* (4) event buffer 0 (core.c: dwc3_event_buffers_setup); 1..3 only masked away */
	reg_write_logged("GEVNTADRLO0", GEVNTADRLO(0), (uint32_t)vayu_usbnet_dma_pa(USB_DMA_EVENTS));
	reg_write_logged("GEVNTADRHI0", GEVNTADRHI(0), (uint32_t)(vayu_usbnet_dma_pa(USB_DMA_EVENTS) >> 32));
	/* (INTMASK: no interrupt is routed; the controller writes events all the same) */
	reg_write_logged("GEVNTSIZ0", GEVNTSIZ(0), USB_EVENT_SIZE | GEVNTSIZ_INTMASK);
	reg_write_logged("GEVNTCOUNT0", GEVNTCOUNT(0), 0);
	for (unsigned int n = 1; n < 4; n++) {
		char name[16];

		if (rd(dwc3, GEVNTSIZ(n)) == 0) {
			continue;
		}
		snprintf(name, sizeof(name), "GEVNTSIZ%u", n);
		reg_write_logged(name, GEVNTSIZ(n), rd(dwc3, GEVNTSIZ(n)) | GEVNTSIZ_INTMASK);
	}

	/* (5) high speed, address 0 (gadget.c:2165, the run_stop's DCFG) */
	dcfg = rd(dwc3, DCFG) & ~(DCFG_SPEED_MASK | DCFG_DEVADDR_MASK);
	reg_write_logged("DCFG", DCFG, dcfg);

	/* (6) the endpoints */
	if (!dwc3_endpoints()) {
		return false;
	}
	/* the loader's FIFOs (GTXFIFOSIZ(0) is ep0's): used as they are, not resized */
	printf("vayu-usbnet: GTXFIFOSIZ 1 %08x, 2 %08x%s\n", rd(dwc3, GTXFIFOSIZ(1)), rd(dwc3, GTXFIFOSIZ(2)),
	    rd(dwc3, GTXFIFOSIZ(1)) != 0 && rd(dwc3, GTXFIFOSIZ(2)) != 0 ? "" :
	    ": NOT ALLOCATED, the notify and bulk IN endpoints will not move");

	/* (7) one SETUP TRB waiting (ep0.c: dwc3_ep0_out_start) */
	vayu_usbnet_setup_arm();

	/* (8) the events the loop wants (core.h:405-416) */
	reg_write_logged("DEVTEN", DEVTEN, DEVTEN_DISCONNECT | DEVTEN_USBRST | DEVTEN_CONNECTDONE | DEVTEN_ULSTCNG);

	/* (9) and run (gadget.c:2185) */
	reg_write_logged("DCTL", DCTL, rd(dwc3, DCTL) | DCTL_RUN_STOP);
	if (!wait_bits(DSTS, DSTS_DEVCTRLHLT, false, 100)) {
		printf("vayu-usbnet: DSTS.DEVCTRLHLT does not clear: the controller does not run\n");
		return false;
	}
	printf("vayu-usbnet: running as a USB device (ECM), waiting for the host\n");
	return true;
}

/* ---- the event buffer, one event at a time --------------------------------------------------- */

static void
event_one(uint32_t raw)
{
	if ((raw & 1) != 0) {                                   /* a device event (core.h:1327) */
		switch (DEVICE_EVENT_TYPE(raw)) {
		case 0:
			printf("vayu-usbnet: the host is gone (disconnect)\n");
			vayu_usbnet_ecm_disconnect();
			break;
		case 1:
			printf("vayu-usbnet: USB reset\n");
			vayu_usbnet_wr(USB_DCFG, vayu_usbnet_rd(USB_DCFG) & ~DCFG_DEVADDR_MASK);
			vayu_usbnet_ecm_reset();
			break;
		case 2: {
			static const char *const speed[] = { "high", "full", "low", "?", "super" };
			uint32_t dsts = rd(dwc3, DSTS);

			printf("vayu-usbnet: connect done: %s speed (DSTS %08x)\n",
			    speed[(dsts & 7) < 5 ? (dsts & 7) : 3], dsts);
			vayu_usbnet_ecm_connect();
			break;
		}
		case 3: {
			uint32_t state = (rd(dwc3, DSTS) >> 18) & 0xf;

			/* (once per change, as gadget.c:3660 keeps dwc->link_state) */
			if (state != usb_link_state) {
				usb_link_state = state;
				printf("vayu-usbnet: link state %u\n", state);
			}
			break;
		}
		default:
			printf("vayu-usbnet: device event %u\n", DEVICE_EVENT_TYPE(raw));
			break;
		}
		return;
	}
	switch (DEPEVT_EVENT(raw)) {
	case USB_DEPEVT_XFERCOMPLETE:
	case USB_DEPEVT_XFERINPROGRESS:
	case USB_DEPEVT_XFERNOTREADY:
		vayu_usbnet_ecm_ep_event(DEPEVT_PHYS(raw), DEPEVT_EVENT(raw), DEPEVT_STATUS(raw), DEPEVT_PARAM(raw));
		break;
	case USB_DEPEVT_EPCMDCMPLT:
		break;
	default:
		printf("vayu-usbnet: endpoint %u event %u\n", DEPEVT_PHYS(raw), DEPEVT_EVENT(raw));
		break;
	}
}

/*
 * The loop: every millisecond while anything has happened in the last second, every five when it
 * has been quiet (a quiet controller costs one read of GEVNTCOUNT). No interrupt is attached, as in
 * hakios.ufshc.c and hakios.net.c.
 */
static void
event_loop(void)
{
	uint32_t quiet = 0, said = 0;

	for (;;) {
		uint32_t count = rd(dwc3, GEVNTCOUNT(0)) & 0xfffc;
		uint32_t interval;

		if (usb_quiesced) {
			thread_terminate(current_thread());
		}
		if (count != 0) {
			uint32_t done = 0;

			while (done < count) {
				uint32_t *event = (uint32_t *)(usb_dma + USB_DMA_EVENTS + usb_event_at);

				vayu_usbnet_inval((vm_offset_t)event, sizeof(*event));
				event_one(*event);
				usb_event_at = (usb_event_at + 4) % USB_EVENT_SIZE;
				done += 4;
				usb_events++;
			}
			vayu_usbnet_wr(GEVNTCOUNT(0), done);
			quiet = 0;
		} else if (quiet < 1000) {
			quiet++;
		}
		interval = (count != 0 || quiet < 1000) ? 1 : 5;
		said += interval;
		if (said >= 30000) {
			said = 0;
			vayu_usbnet_ecm_counters();
		}
		delay_for_interval(interval, NSEC_PER_MSEC);
	}
}

/* Into the loader's log only: quiesce runs with interrupts off, where printf panics ("Console I/O from
 * interrupt-disabled context", run 051220). */
static void
quiet_log(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/*
 * On the way back to the loader (loader_return.c), interrupts off: no sleeping, no printf. Stopped as
 * in dwc3_take() (events acknowledged until the halt); the loader restarts its own USB stack after the
 * run anyway (stop + start_ex).
 */
void
vayu_usbnet_quiesce(void)
{
	unsigned int ms;

	if (!usb_taken || dwc3 == 0) {
		return;
	}
	usb_quiesced = true;
	vayu_usbnet_wr(DEVTEN, 0);
	vayu_usbnet_wr(DCTL, rd(dwc3, DCTL) & ~DCTL_RUN_STOP);
	for (ms = 0; ms < 100 && (rd(dwc3, DSTS) & DSTS_DEVCTRLHLT) == 0; ms++) {
		uint32_t pending = rd(dwc3, GEVNTCOUNT(0)) & 0xfffc;

		if (pending != 0) {
			vayu_usbnet_wr(GEVNTCOUNT(0), pending);
		}
		delay(1000);
	}
	vayu_usbnet_wr(GEVNTSIZ(0), rd(dwc3, GEVNTSIZ(0)) | GEVNTSIZ_INTMASK);
	quiet_log("vayu-usbnet: quiesce: RUN_STOP 0, %s after %u ms (usb_take stays 1)\n",
	    (rd(dwc3, DSTS) & DSTS_DEVCTRLHLT) != 0 ? "halted" : "STILL RUNNING", ms);
}

static void
vayu_usbnet_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t level = (uint32_t)(uintptr_t)parameter;

	dwc3 = ml_io_map(DWC3_PHYS, DWC3_SIZE);
	if (dwc3 == 0) {
		printf("vayu-usbnet: dwc3 cannot be mapped\n");
		thread_terminate(current_thread());
	}
	if (level == 1) {
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		if (gcc == 0) {
			printf("vayu-usbnet: gcc cannot be mapped\n");
			thread_terminate(current_thread());
		}
		hsphy = ml_io_map(HSPHY_PHYS, HSPHY_SIZE);
		if (hsphy == 0) {
			printf("vayu-usbnet: hsphy cannot be mapped\n");
			thread_terminate(current_thread());
		}
		smmu = ml_io_map(SMMU_PHYS, SMMU_SIZE);
		if (smmu == 0) {
			printf("vayu-usbnet: smmu cannot be mapped\n");
			thread_terminate(current_thread());
		}
		IOSleep(15000);
		look(1);
		IOSleep(45000);
		look(2);
		printf("vayu-usbnet: step 1 done, nothing written\n");
		thread_terminate(current_thread());
	}

	/*
	 * en2 and its IOKit object at once, long before configd starts, so that configd meets it in its
	 * first look at the interfaces (run 055212: published after the delay, configd made no service of
	 * it and net_test's DHCP setservice had to). The link stays down until the host selects alternate 1.
	 */
	vayu_usbnet_ecm_init();
	/* (the log streams through the loader's USB until it is asked to let go) */
	IOSleep(usb_delay * 1000);
	if (!usb_handover()) {
		thread_terminate(current_thread());
	}
	usb_taken = true;
	if (!dma_area()) {
		thread_terminate(current_thread());
	}
	if (!dwc3_take()) {
		printf("vayu-usbnet: the controller is not ours; nothing more is done\n");
		thread_terminate(current_thread());
	}
	event_loop();
}

void
vayu_usbnet_init(void)
{
	thread_t thread;
	uint32_t level = 0, handover = 0;

	/* usb_handover=1 alone means the network: level 3 (the loader hands USB over for it) */
	if (!PE_parse_boot_argn("vayu_usbnet", &level, sizeof(level))) {
		level = PE_parse_boot_argn("usb_handover", &handover, sizeof(handover)) && handover == 1 ? 3 : 0;
	}
	if (level == 0) {
		return;
	}
	if (level > 3) {
		printf("vayu-usbnet: level %u: there are 1, 2 and 3; 3 is used\n", level);
		level = 3;
	}
	if (level >= 2) {
		PE_parse_boot_argn("usbnet_delay", &usb_delay, sizeof(usb_delay));
		if (usb_delay > 3600) {
			usb_delay = 3600;
		}
		PE_parse_boot_argn("usbnet_ip", &usb_ip, sizeof(usb_ip));
		if (!PE_parse_boot_argn("usb_handover", &handover, sizeof(handover)) || handover != 1) {
			printf("vayu-usbnet: level %u needs usb_handover=1 (the loader owns USB)\n", level);
			return;
		}
	}
	usb_level = level;
	if (kernel_thread_start(vayu_usbnet_thread, (void *)(uintptr_t)level, &thread) != KERN_SUCCESS) {
		printf("vayu-usbnet: the thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}
