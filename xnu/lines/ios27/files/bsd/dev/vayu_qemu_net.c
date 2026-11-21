/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * A TEST DEVICE, NOT A DRIVER OF THE PHONE: QEMU's virtio network device as en0, so that the NBD disk
 * (bsd/dev/vayu_nbd.c) and the rest of the kernel's network path can be tried in QEMU (tools/vayu/tests/k6)
 * the way the phone takes its root over en2 (bsd/dev/vayu_usbnet_ecm.c). What it shows is the socket, TCP, the
 * NBD client and the block device; nothing of DWC3, the USB handover or the phone's cache maintenance.
 *
 * Only with vayu27=1 qemu_net=1, and only on QEMU's virt machine: the GIC distributor the device tree names must
 * be QEMU's (0x08000000; vayu's is 0x17a00000), else nothing is mapped or read -- 0x0a000000, where QEMU puts its
 * virtio transports, is the phone's USB subsystem's neighbourhood. "-device virtio-net-device" on the
 * virtio-mmio bus, the current transport (-global virtio-mmio.force-legacy=false), any -netdev behind it.
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/hakios.virtio_net.c (the 8796 line's QEMU link of hakios.net.c, which
 * this tree does not have): the same queues and feature choice -- nothing of what the device offers but its
 * address, a 12-byte header nobody reads, one buffer per frame, no interrupt (a thread polls) -- with an ifnet of
 * its own as vayu_usbnet_ecm.c makes en2.
 */
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/section_keywords.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/kpi_mbuf.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vayu_dma_compat.h>
#include <sys/vayu_storage.h>

extern void IOSleep(unsigned int milliseconds);

#define QEMU_VIRT_GICD          0x08000000ULL   /* hw/arm/virt.c VIRT_GIC_DIST */
#define VIRTIO_BUS              0x0a000000UL    /* hw/arm/virt.c VIRT_MMIO: 32 transports of 0x200 bytes */
#define VIRTIO_SLOTS            32
#define VIRTIO_SLOT_SIZE        0x200

#define VIRTIO_MAGIC            0x000           /* "virt" */
#define VIRTIO_VERSION          0x004
#define VIRTIO_DEVICE_ID        0x008
#define VIRTIO_DEVICE_FEATURES  0x010
#define VIRTIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_DRIVER_FEATURES  0x020
#define VIRTIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_QUEUE_SEL        0x030
#define VIRTIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_QUEUE_NUM        0x038
#define VIRTIO_QUEUE_READY      0x044
#define VIRTIO_QUEUE_NOTIFY     0x050
#define VIRTIO_STATUS           0x070
#define VIRTIO_QUEUE_DESC       0x080
#define VIRTIO_QUEUE_DRIVER     0x090
#define VIRTIO_QUEUE_DEVICE     0x0a0
#define VIRTIO_CONFIG           0x100           /* the device's own: for a network device, its address first */

#define VIRTIO_ID_NET           1
#define VIRTIO_NET_F_MAC        (1u << 5)
#define STATUS_ACKNOWLEDGE      1
#define STATUS_DRIVER           2
#define STATUS_DRIVER_OK        4
#define STATUS_FEATURES_OK      8

#define QUEUE_SIZE              64
#define QUEUE_RECEIVE           0
#define QUEUE_TRANSMIT          1
#define BUFFER_SIZE             2048
#define DESC_F_WRITE            2
#define AVAIL_F_NO_INTERRUPT    1

struct vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
struct vq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[QUEUE_SIZE];
};
struct vq_used {
	uint16_t flags;
	uint16_t idx;
	struct {
		uint32_t id;
		uint32_t len;
	} ring[QUEUE_SIZE];
};
struct virtio_net_header {
	uint8_t flags, gso_type;
	uint16_t header_length, gso_size, checksum_start, checksum_offset, buffers;
};

/* a queue: its rings in the first 4 KiB of its piece of memory, its buffers after them */
#define OFFSET_DESC             0x0000
#define OFFSET_AVAIL            0x0400
#define OFFSET_USED             0x0800
#define OFFSET_BUFFERS          0x1000
#define QUEUE_BYTES             (OFFSET_BUFFERS + QUEUE_SIZE * BUFFER_SIZE)

struct queue {
	volatile struct vq_desc *desc;
	volatile struct vq_avail *avail;
	volatile struct vq_used *used;
	uint8_t *buffers;
	uint16_t seen;
	uint16_t free[QUEUE_SIZE];
	unsigned free_count;
};

static vm_offset_t virtio;
static struct queue receive_queue, transmit_queue;
static ifnet_t qnet_ifp;
static uint8_t qnet_address[ETHER_ADDR_LEN] = { 0x02, 'v', 'a', 'y', 'u', 'q' };
static LCK_GRP_DECLARE(qnet_lck_grp, "vayu-qemu-net");
static LCK_MTX_DECLARE(qnet_tx_lock, &qnet_lck_grp);

static inline uint32_t
reg_read(uint32_t reg)
{
	return *(volatile uint32_t *)(virtio + reg);
}

static inline void
reg_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(virtio + reg) = value;
}

static inline void
reg_write64(uint32_t reg, uint64_t value)
{
	reg_write(reg, (uint32_t)value);
	reg_write(reg + 4, (uint32_t)(value >> 32));
}

/*
 * QEMU's virt machine, by its GIC: the distributor's address in the device tree, as pexpert/arm/pe_fiq.c reads it
 * ("reg" of /arm-io/gic), made absolute with /arm-io's "ranges" (the loader writes the registers relative to the
 * platform's I/O base: { child 0, parent io_base, size }).
 */
static bool
qnet_on_qemu_virt(void)
{
	DTEntry entry;
	void const *reg, *ranges;
	unsigned int size, rsize;
	uint64_t gicd;

	if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "reg", &reg, &size) != kSuccess || size < sizeof(uint64_t)) {
		return false;
	}
	gicd = ((const uint64_t *)reg)[0];
	if (SecureDTLookupEntry(NULL, "/arm-io", &entry) == kSuccess &&
	    SecureDTGetProperty(entry, "ranges", &ranges, &rsize) == kSuccess && rsize >= 3 * sizeof(uint64_t)) {
		const uint64_t *r = ranges;

		if (gicd < r[0] || gicd - r[0] >= r[2]) {
			return false;
		}
		gicd = gicd - r[0] + r[1];
	}
	if (gicd != QEMU_VIRT_GICD) {
		printf("vayu-qemu-net: the GIC distributor is at %#llx, not QEMU's virt %#llx\n", gicd, QEMU_VIRT_GICD);
		return false;
	}
	return true;
}

static bool
queue_make(struct queue *q, uint32_t index, bool device_writes)
{
	vm_offset_t va = 0;
	uint64_t pa = 0;

	reg_write(VIRTIO_QUEUE_SEL, index);
	if (reg_read(VIRTIO_QUEUE_NUM_MAX) < QUEUE_SIZE) {
		printf("vayu-qemu-net: virtio queue %u holds only %u\n", index, reg_read(VIRTIO_QUEUE_NUM_MAX));
		return false;
	}
	if (vayu_dma_alloc(QUEUE_BYTES, 0, 0, &va, &pa) != KERN_SUCCESS) {
		printf("vayu-qemu-net: no memory for queue %u\n", index);
		return false;
	}
	q->desc = (volatile struct vq_desc *)(va + OFFSET_DESC);
	q->avail = (volatile struct vq_avail *)(va + OFFSET_AVAIL);
	q->used = (volatile struct vq_used *)(va + OFFSET_USED);
	q->buffers = (uint8_t *)(va + OFFSET_BUFFERS);
	for (int i = 0; i < QUEUE_SIZE; i++) {
		q->desc[i].addr = pa + OFFSET_BUFFERS + (uint64_t)i * BUFFER_SIZE;
		q->desc[i].len = BUFFER_SIZE;
		q->desc[i].flags = device_writes ? DESC_F_WRITE : 0;
		if (device_writes) {
			q->avail->ring[i] = (uint16_t)i;         /* the device gets them all, to fill */
		} else {
			q->free[q->free_count++] = (uint16_t)i;
		}
	}
	q->avail->flags = AVAIL_F_NO_INTERRUPT;
	q->avail->idx = device_writes ? QUEUE_SIZE : 0;
	reg_write(VIRTIO_QUEUE_NUM, QUEUE_SIZE);
	reg_write64(VIRTIO_QUEUE_DESC, pa + OFFSET_DESC);
	reg_write64(VIRTIO_QUEUE_DRIVER, pa + OFFSET_AVAIL);
	reg_write64(VIRTIO_QUEUE_DEVICE, pa + OFFSET_USED);
	__builtin_arm_dmb(0xb);
	reg_write(VIRTIO_QUEUE_READY, 1);
	return true;
}

static bool
qnet_device_start(void)
{
	vm_offset_t bus = ml_io_map(VIRTIO_BUS, VIRTIO_SLOTS * VIRTIO_SLOT_SIZE);
	uint32_t features;
	int slot;

	if (bus == 0) {
		return false;
	}
	for (slot = 0; slot < VIRTIO_SLOTS; slot++) {
		virtio = bus + (vm_offset_t)slot * VIRTIO_SLOT_SIZE;
		if (reg_read(VIRTIO_MAGIC) == 0x74726976 && reg_read(VIRTIO_DEVICE_ID) == VIRTIO_ID_NET) {
			break;
		}
	}
	if (slot == VIRTIO_SLOTS) {
		printf("vayu-qemu-net: no virtio network device (QEMU: -device virtio-net-device)\n");
		return false;
	}
	if (reg_read(VIRTIO_VERSION) != 2) {
		printf("vayu-qemu-net: slot %d is a legacy transport (QEMU: -global virtio-mmio.force-legacy=false)\n", slot);
		return false;
	}
	reg_write(VIRTIO_STATUS, 0);
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE);
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
	reg_write(VIRTIO_DEVICE_FEATURES_SEL, 0);
	features = reg_read(VIRTIO_DEVICE_FEATURES);
	reg_write(VIRTIO_DRIVER_FEATURES_SEL, 0);
	reg_write(VIRTIO_DRIVER_FEATURES, features & VIRTIO_NET_F_MAC);
	reg_write(VIRTIO_DRIVER_FEATURES_SEL, 1);
	reg_write(VIRTIO_DRIVER_FEATURES, 1);           /* bit 32: VIRTIO_F_VERSION_1 */
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
	if (!(reg_read(VIRTIO_STATUS) & STATUS_FEATURES_OK)) {
		printf("vayu-qemu-net: slot %d: features refused (offered %#x)\n", slot, features);
		return false;
	}
	if (!queue_make(&receive_queue, QUEUE_RECEIVE, true) || !queue_make(&transmit_queue, QUEUE_TRANSMIT, false)) {
		return false;
	}
	if (features & VIRTIO_NET_F_MAC) {
		for (int i = 0; i < ETHER_ADDR_LEN; i++) {
			qnet_address[i] = *(volatile uint8_t *)(virtio + VIRTIO_CONFIG + i);
		}
	}
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
	reg_write(VIRTIO_QUEUE_NOTIFY, QUEUE_RECEIVE);
	printf("vayu-qemu-net: virtio network device in slot %d (features %#x)\n", slot, features);
	return true;
}

static errno_t
qnet_output(ifnet_t ifp, mbuf_t packet)
{
	struct queue *q = &transmit_queue;
	size_t length = mbuf_pkthdr_len(packet);
	bool sent = false;

	lck_mtx_lock(&qnet_tx_lock);
	while (q->seen != q->used->idx) {
		q->free[q->free_count++] = (uint16_t)(q->used->ring[q->seen++ % QUEUE_SIZE].id % QUEUE_SIZE);
	}
	if (q->free_count != 0 && length != 0 && length <= BUFFER_SIZE - sizeof(struct virtio_net_header)) {
		uint16_t id = q->free[--q->free_count];
		uint8_t *buffer = q->buffers + (size_t)id * BUFFER_SIZE;

		bzero(buffer, sizeof(struct virtio_net_header));
		if (mbuf_copydata(packet, 0, length, buffer + sizeof(struct virtio_net_header)) == 0) {
			q->desc[id].len = (uint32_t)(sizeof(struct virtio_net_header) + length);
			q->avail->ring[q->avail->idx % QUEUE_SIZE] = id;
			__builtin_arm_dmb(0xb);
			q->avail->idx++;
			__builtin_arm_dmb(0xb);
			reg_write(VIRTIO_QUEUE_NOTIFY, QUEUE_TRANSMIT);
			sent = true;
		} else {
			q->free[q->free_count++] = id;
		}
	}
	lck_mtx_unlock(&qnet_tx_lock);
	if (sent) {
		ifnet_stat_increment_out(ifp, 1, (uint32_t)length, 0);
	} else {
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(packet);
	return 0;
}

static void
qnet_input(const uint8_t *frame, size_t length)
{
	struct ifnet_stat_increment_param stats = { .packets_in = 1, .bytes_in = (uint32_t)length };
	unsigned int chunks = 1;
	mbuf_t packet = NULL;
	char *header;

	if (length < ETHER_HDR_LEN || mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet) != 0) {
		return;
	}
	if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
		mbuf_freem(packet);
		return;
	}
	header = mbuf_data(packet);
	mbuf_pkthdr_setrcvif(packet, qnet_ifp);
	mbuf_pkthdr_setheader(packet, header);
	mbuf_pkthdr_adjustlen(packet, -ETHER_HDR_LEN);
	mbuf_setdata(packet, header + ETHER_HDR_LEN, length - ETHER_HDR_LEN);
	bpf_tap_in(qnet_ifp, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
	ifnet_input(qnet_ifp, packet, &stats);
}

/* what has arrived, handed up; the buffers back to the device */
static __attribute__((noreturn)) void
qnet_thread(__unused void *arg, __unused wait_result_t wr)
{
	struct queue *q = &receive_queue;

	for (;;) {
		unsigned frames = 0;

		while (q->seen != q->used->idx) {
			uint32_t id = q->used->ring[q->seen % QUEUE_SIZE].id % QUEUE_SIZE;
			uint32_t length = q->used->ring[q->seen % QUEUE_SIZE].len;

			q->seen++;
			if (length > sizeof(struct virtio_net_header) && length <= BUFFER_SIZE) {
				qnet_input(q->buffers + (size_t)id * BUFFER_SIZE + sizeof(struct virtio_net_header),
				    length - sizeof(struct virtio_net_header));
			}
			q->avail->ring[q->avail->idx % QUEUE_SIZE] = (uint16_t)id;
			__builtin_arm_dmb(0xb);
			q->avail->idx++;
			frames++;
		}
		if (frames != 0) {
			__builtin_arm_dmb(0xb);
			reg_write(VIRTIO_QUEUE_NOTIFY, QUEUE_RECEIVE);
		} else {
			IOSleep(1);
		}
	}
}

static errno_t
qnet_ioctl(ifnet_t ifp, u_long command, void *data)
{
	struct ifreq *request = data;

	switch (command) {
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;
	case SIOCSIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		return 0;
	case SIOCSIFFLAGS:
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		return 0;
	case SIOCSIFMTU:
		return request->ifr_mtu > ETHERMTU || request->ifr_mtu < IF_MINMTU ? EINVAL : ifnet_set_mtu(ifp, request->ifr_mtu);
	default:
		return EOPNOTSUPP;
	}
}

void
vayu_qemu_net_init(void)
{
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = "vayu-qemu-net",
		.uniqueid_len = 13,
		.name = "en",
		.unit = 0,
		.family = IFNET_FAMILY_ETHERNET,
		.subfamily = IFNET_SUBFAMILY_ANY,
		.type = IFT_ETHER,
		.output = qnet_output,
		.demux = ether_demux,
		.add_proto = ether_add_proto,
		.del_proto = ether_del_proto,
		.check_multi = ether_check_multi,
		.framer_extended = ether_frameout_extended,
		.ioctl = qnet_ioctl,
		.broadcast_addr = etherbroadcastaddr,
		.broadcast_len = ETHER_ADDR_LEN,
	};
	thread_t thread;

	if (!vayu_arg("qemu_net")) {
		return;
	}
	if (!qnet_on_qemu_virt()) {
		printf("vayu-qemu-net: qemu_net=1, but this is not QEMU's virt machine: nothing touched\n");
		return;
	}
	if (!qnet_device_start()) {
		return;
	}
	if (ifnet_allocate_extended(&init, &qnet_ifp) != 0) {
		printf("vayu-qemu-net: en0 cannot be made\n");
		return;
	}
	ifnet_set_capabilities_enabled(qnet_ifp, 0, -1);
	ifnet_set_addrlen(qnet_ifp, ETHER_ADDR_LEN);
	ifnet_set_baudrate(qnet_ifp, 1000 * 1000 * 1000);
	ifnet_set_mtu(qnet_ifp, ETHERMTU);
	ifnet_set_flags(qnet_ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
	ifnet_set_hdrlen(qnet_ifp, ETHER_HDR_LEN);
	ifnet_set_offload(qnet_ifp, 0);
	if (ifnet_attach(qnet_ifp, NULL) != 0) {
		printf("vayu-qemu-net: en0 cannot be attached\n");
		ifnet_release(qnet_ifp);
		qnet_ifp = NULL;
		return;
	}
	ifnet_set_lladdr(qnet_ifp, qnet_address, ETHER_ADDR_LEN);
	bpfattach(qnet_ifp, DLT_EN10MB, ETHER_HDR_LEN);
	if (kernel_thread_start(qnet_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	printf("vayu-qemu-net: en0 attached, %02x:%02x:%02x:%02x:%02x:%02x (a test device of QEMU's)\n", qnet_address[0],
	    qnet_address[1], qnet_address[2], qnet_address[3], qnet_address[4], qnet_address[5]);
}
