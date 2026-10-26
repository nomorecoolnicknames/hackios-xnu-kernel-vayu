/*
 * FastRPC for the SLPI's sensors protection domain (vayu, SM8150-AC, XNU).
 *
 * The SLPI opens the GLINK channel "fastrpcglink-apps-dsp" (the DT's dsps edge; qcom,intents =
 * <0x64 0x40>: 64 intents of 100 bytes, recovery-live.dts:7961-7986) and nobody answers it.
 * This file is the answering side: the application processors act as the FastRPC "AP"
 * endpoint, attach the sensors PD, register the reverse listener, and serve the file calls the
 * DSP makes (apps_std) from fw/sensors.pak. Upstream Linux has no such endpoint (there a
 * userspace daemon serves the DSP); the reverse protocol comes from linux-msm/hexagonrpc, and
 * every structure and number below is taken from the wire format those sources document. No
 * code is copied from them or from Linux (both are GPL): this is a clean implementation of the
 * same bytes, with the source of each fact cited.
 *
 * WHERE EVERY FACT COMES FROM
 *
 *   - struct fastrpc_msg, 40 bytes: client_id i32 @0, tid i32 @4, ctx u64 @8, handle u32 @16,
 *     sc u32 @20, addr u64 @24 (dma_addr_t, 64-bit on arm64), size u64 @32.
 *     ref/fastrpc.c:198-206. The answer struct fastrpc_invoke_rsp, 16 bytes: ctx u64 @0,
 *     retval i32 @8. ref/fastrpc.c:208-211.
 *   - sc: attr bits 31:29, method 28:24, inbufs 23:16, outbufs 15:8, in-handles 7:4,
 *     out-handles 3:0. ref/fastrpc.c:86-110; hexagonrpc/include/libhexagonrpc/hexagonrpc.h:31-43
 *     is the same definition on the userspace side.
 *   - ctx: a 256-entry cyclic id in bits 15:8, a 48-bit sequence in bits 63:16
 *     (ref/fastrpc.c:50-53, 744-753), and the protection domain in the low nibble
 *     (msg->ctx = ctxid | fl->pd, ref/fastrpc.c:1270). ROOT 0, USER 1, SENSORS 2
 *     (ref/fastrpc.c:126-128). Answers are matched on bits 15:8 and the sequence, not on the
 *     PD nibble (ref/fastrpc.c:2728-2754). A ctx of 0xABCDABCD is a PD-exit notification, not
 *     an answer, and is dropped (ref/fastrpc.c:56-63, 2720-2726).
 *   - One invoke = one physically contiguous packet holding all metadata and all inline
 *     arguments. For n scalars (in + out buffers, no fds): rpra[n] (16 B each: pv, len),
 *     invoke_buf[n] (8 B each: num, pgidx), phy_page[n] (16 B each: addr, size), fdlist[16]
 *     (u64), crclist[64] (u32), poll (u32) = 40n + 388 bytes; the inline data begins at the
 *     next 128-byte boundary, and every buffer begins on one, in argument order.
 *     ref/fastrpc.c:1009-1021 (size), 1023-1042 (payload), 1110-1118 (pointers), 1121-1192
 *     (each buffer's pv, pages and the copy of input buffers), 1213-1252 (copy back of the
 *     output buffers after the answer). msg.size is the page-rounded packet size
 *     (ref/fastrpc.c:1274); msg.addr is the packet's DSP address (ref/fastrpc.c:1273).
 *   - The DSP address is the physical address: this tree puts an S1-bypass entry for the
 *     sensors SIDs 0x5a1-0x5a3 (SMR 0x5a0, mask 0x3) in the apps SMMU. The context-bank
 *     number goes in bits 32+ of every address, as on downstream: buf->phys += (u64)cb << 32
 *     with cb = SID & 0xf; upstream's default_soc_data has sid_pos = 32, i.e. (u64)sid << 32
 *     (ref/fastrpc.c:361-376, 509-525, 2507-2511). Boot-arg vayu_frpc_cb chooses the number
 *     (default 1, the first of 0x5a1/0x5a2/0x5a3); 0 sends plain physical addresses.
 *     Upstream also masks the SDSP's DMA to 32 bits (ref/fastrpc.c:2391, 2510), so the packet
 *     must live below 4 GiB; that is checked after each allocation. The DT marks these context
 *     banks dma-coherent (iommus = <&apps_smmu 0x5a1 0>, "dma-coherent"), so no cache
 *     maintenance is done around the packet: only the barriers before the send and after the
 *     answer.
 *   - INIT_ATTACH for the sensors PD: handle 1 (FASTRPC_INIT_HANDLE, ref/fastrpc.c:42),
 *     method 0 (FASTRPC_RMID_INIT_ATTACH, ref/fastrpc.c:115), sc = one in-buffer, no
 *     out-buffer (ref/fastrpc.c:1851), the one buffer an int client id (ref/fastrpc.c:1848-1850),
 *     and fl->pd is set to SENSORS_PD before the invoke (ref/fastrpc.c:1852-1855).
 *   - The reverse listener (hexagonrpc): the AP first opens "adsp_default_listener" through
 *     handle 0 / method 0 (libhexagonrpc/interface/remotectl.c:25-38;
 *     libhexagonrpc/handle.c:34 passes strlen + 1), then calls method 0 on the returned handle
 *     with no arguments to register (hexagonrpcd/interface/adsp_default_listener.c:25-31,
 *     hexagonrpcd/rpcd.c:47-68). It then calls handle 3 / method 3 init2 with no arguments and
 *     handle 3 / method 4 next2 in a loop (hexagonrpcd/interface/adsp_listener.c:25-50,
 *     hexagonrpcd/listener.c:563-605). Handle 3 is ADSP_LISTENER
 *     (hexagonrpcd/interface/adsp_listener.h:28).
 *   - next2's arguments (hexagonrpcd/interface/adsp_listener.c:33-50): in is {ret_rctx u32,
 *     ret_res u32, outbufs_len u32} plus one byte sequence of the previous answer's output
 *     buffers, and its scalar block also carries inbufs_size (the OUT_BLOB_SEQ's length, see
 *     hexagonrpc.c:235-243) -- 16 bytes in all; out is {rctx, handle, sc, inbufs_len} (16
 *     bytes) plus 256 bytes of input buffers. REMOTE_SCALARS: in 2, out 2, method 4, i.e.
 *     0x04020200. The first call has rctx 0 and result 0xffffffff
 *     (hexagonrpcd/listener.c:569-572); afterwards each call carries the previous request's
 *     answer (hexagonrpcd/listener.c:419-490).
 *   - The byte sequence carrying buffers (in either direction): for each buffer a u32 length,
 *     then padding to the next 8-byte boundary, then the bytes. Encoding:
 *     hexagonrpcd/iobuffer.c:194-228; decoding: hexagonrpcd/iobuffer.c:130-192. There is no
 *     padding after the last buffer.
 *   - Call dispatch by handle: 0 remotectl, 1 apps_std, 2 apps_mem
 *     (hexagonrpcd/rpcd.c:209-213); the method is sc's method, or the first u32 of the first
 *     input buffer when the method field is 31 (extended id; hexagonrpcd/listener.c:501-513,
 *     hexagonrpc.c:715-718, 803-808).
 *   - apps_std methods and their exact argument order (hexagonrpcd/interface/apps_std.c):
 *     fflush 2 {fd} (39-49), fclose 3 {fd} (51-61), fread 4 {fd, size} -> {written, eof} +
 *     data (63-76), fseek 9 {fd, pos, whence} (78-90), fopen_with_env 19 {env, -, path, mode}
 *     -> {fd} (92-106), opendir 26 {path} -> {dir u64} (108-119), closedir 27 {dir u64}
 *     (121-131), readdir 28 {dir u64} -> {inode, name[256], eof} (133-146), stat 31 (extended
 *     id) {path} -> 96 bytes of stat fields (161-184). The implementations, the NUL checks,
 *     the write-mode refusal and the env bases are hexagonrpcd/apps_std.c:53-352; the method
 *     table (which ids exist) is 408-474.
 *   - Paths the sensors PD is served (hexagonrpcd/rpcd_builder.c:112-209): /persist and
 *     /mnt/vendor/persist both carry sensors/registry/registry (159-169, 185-191);
 *     /vendor/etc/sensors/{config,sns_reg_config} and /system/vendor/... (171-183);
 *     /sys/devices/soc0 is a directory whose files come from the platform's socinfo directory
 *     (192-196). hexagonrpcd synthesizes each virtual file's stat
 *     (hexagonrpcd/hexagonfs_plat_subtype_name.c:76-96) and the values below are HYPOTHESES
 *     for a vayu: they are logged every time one is served so they can be corrected from one
 *     boot.
 *   - fw/sensors.pak: "SNSPAK1\0", u32 count, u32 reserved, then count entries of
 *     {char path[192], u32 offset, u32 size, u32 mode (0 file, 1 dir), u32 zero}, then the
 *     data at those absolute file offsets (ref/mksnspak.py:9-12, 31-38).
 *   - AEE result codes (hexagonrpc/include/libhexagonrpc/error.h:36-85).
 *
 * WHAT THIS FILE DOES NOT KNOW
 *
 *   - Whether the DSP's stat out-buffer is 96 or 104 bytes. Both are handled: one 96-byte
 *     buffer, or the 104-byte variant (96 + the extra u64 as a second buffer when the request
 *     says sc.out == 2); the size used is logged on every stat.
 *   - Whether it asks for absolute paths or relative ones under ADSP_LIBRARY_PATH /
 *     ADSP_AVS_CFG_PATH; both are accepted, as are /persist and /mnt/vendor/persist.
 *   - The platform values: soc_id "339", hw_platform "MTP", platform_subtype "Unknown",
 *     platform_subtype_id "0", platform_version "65536", revision "2.2".
 *
 * Boot-args:
 *   vayu_frpc=0|1|2   0: nothing (the channel stays unanswered); 1: attach, listener and
 *                     apps_std from fw/sensors.pak; 2: also hex-dump the first 16 messages
 *                     each way. Read here and in vayu_slpi.c (which adds the channel).
 *   vayu_frpc_cb=N    the context-bank number in bits 32+ (default 1; 0: plain physical
 *                     addresses).
 *
 * Locking and threads: the GLINK edge's thread calls vayu_fastrpc_input(), which only copies
 * the 16-byte answer into a small ring under a spin lock and issues a wakeup; it never sleeps
 * and never prints. The FastRPC thread polls the ring every millisecond (the same shape as
 * vayu_ssc.c). One next2 is outstanding at a time and its reply is carried by the following
 * next2, so at most two packet buffers are ever needed; both are allocated once, physically
 * contiguous and below 4 GiB. Kernel stacks are 16 KB: the big packet (64 KB), the reply
 * encoder (62 KB) and the read staging buffer (60 KB) are static, never on a stack.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/vayu_fastrpc.h>
#include <sys/vayu_pas.h>		/* struct vayu_glink, struct vayu_glink_channel, vayu_glink_send() */
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern void loader_log_putc(char c);
extern void IOSleep(unsigned int);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);

/* fw/sensors.pak, read by the loader's image helper (as fw/adsp.mdt is read by vayu_pas.c). */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out); /* hakios.image.c:26 */

/* The apps SMMU's bypass (vayu_pas.c, a driver commit's d279ffa): a handle >= 0, or -1. */
extern int vayu_smmu_claim(uint16_t sid, uint16_t mask, bool bypass);
extern void vayu_smmu_release(int handle);

/* --------------------------------------------------------------------------------------- */
/* Numbers from the wire format (all cited in the comment above)                           */
/* --------------------------------------------------------------------------------------- */

#define FRPC_PD			2		/* SENSORS_PD: ref/fastrpc.c:128 */
#define FRPC_CLIENT_ID		0x4001		/* the "pid" we present */
#define FRPC_TID		1
#define FRPC_INIT_HANDLE	1		/* ref/fastrpc.c:42 */
#define FRPC_LISTENER_HANDLE	3		/* hexagonrpc adsp_listener.h:28 */
#define FRPC_STAT_MSG_ID	31		/* interface/apps_std.c:178-184 */

#define FRPC_ALIGN		128		/* ref/fastrpc.c:38 */
#define FRPC_MAX_FDLIST		16		/* ref/fastrpc.c:39 */
#define FRPC_MAX_CRCLIST	64		/* ref/fastrpc.c:40 */
#define FRPC_META_PER		40		/* 16 + 8 + 16 bytes per scalar */
#define FRPC_META_FIXED		388		/* 8*16 + 4*64 + 4 */
#define FRPC_PAGE		4096
#define FRPC_BUF_SIZE		65536		/* one packet, comfortably below the 64 KB asked for */
#define FRPC_SLOTS		2
#define FRPC_MAX_ARGS		8
#define FRPC_MAX_IN		8
#define FRPC_MAX_OUT		4

#define FRPC_RING		16
#define FRPC_MAX_FILES		16
#define FRPC_MAX_DIRS		4
#define FRPC_PATH		256
#define FRPC_PAK_MAX		256
#define FRPC_REPLY_BYTES	62000		/* a 60 KB read plus its 8-byte scalar block and padding */
#define FRPC_MAX_READ		61440		/* one fread; the whole invoke packet stays below 64 KB */

#define FRPC_DIR_BASE		0x100		/* dir handles are 0x100..0x103 (opaque to the DSP) */
#define FRPC_PD_NOTIFY_CTX	0xABCDABCDULL	/* ref/fastrpc.c:63 */

#define FRPC_SID		0x5a0		/* the SMR covering 0x5a1-0x5a3 (mask 0x3) */
#define FRPC_SID_MASK		0x3

/* AEE codes: hexagonrpc/include/libhexagonrpc/error.h:36-85. */
#define AEE_SUCCESS		0
#define AEE_EFAILED		1
#define AEE_ENOMEMORY		2
#define AEE_EBADPARM		14
#define AEE_EUNSUPPORTED	20
#define AEE_EBUFFERTOOSMALL	38

/* sc in the shape ref/fastrpc.c:101-110 builds it, with no handles and attr 0. */
#define FRPC_SC(method, in, out) \
	((((uint32_t)(method) & 0x1f) << 24) | (((uint32_t)(in) & 0xff) << 16) | \
	 (((uint32_t)(out) & 0xff) << 8))
#define FRPC_SC_METHOD(sc)	(((sc) >> 24) & 0x1f)
#define FRPC_SC_IN(sc)		(((sc) >> 16) & 0xff)
#define FRPC_SC_OUT(sc)		(((sc) >> 8) & 0xff)

/*
 * struct fastrpc_msg: the 40 bytes of every FastRPC invoke (ref/fastrpc.c:198-206). Natural
 * alignment on a 64-bit kernel already gives the offsets 0/4/8/16/20/24/32 and size 40.
 */
struct fastrpc_msg {
	int32_t client_id;
	int32_t tid;
	uint64_t ctx;
	uint32_t handle;
	uint32_t sc;
	uint64_t addr;
	uint64_t size;
};

/* --------------------------------------------------------------------------------------- */
/* The loader's log, and small helpers                                                     */
/* --------------------------------------------------------------------------------------- */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[420];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {	/* (not on the way out: loader_return runs with interrupts off) */
		printf("%s", line);
	}
	return length;
}

static uint32_t
get32(const void *p)
{
	const uint8_t *b = p;

	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t
get64(const void *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32((const uint8_t *)p + 4) << 32);
}

static void
put32(void *p, uint32_t v)
{
	uint8_t *b = p;

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static void
put64(void *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32((uint8_t *)p + 4, (uint32_t)(v >> 32));
}

static uint64_t
frpc_now_ns(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

/* A bounded copy that always terminates (path buffers are 192 bytes in the pak). */
static void
frpc_copy(char *to, size_t cap, const char *from)
{
	size_t n;

	if (cap == 0) {
		return;
	}
	n = strlen(from);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(to, from, n);
	to[n] = 0;
}

static uint32_t
frpc_min32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static void
frpc_hexdump(const char *way, const uint8_t *p, uint32_t n)
{
	static const char hex[] = "0123456789abcdef";
	char line[3 * 16 + 1];

	for (uint32_t off = 0; off < n; off += 16) {
		uint32_t k = frpc_min32(16, n - off);
		uint32_t j = 0;

		for (uint32_t i = 0; i < k; i++) {
			line[j++] = hex[p[off + i] >> 4];
			line[j++] = hex[p[off + i] & 0xf];
			if (i != k - 1) {
				line[j++] = ' ';
			}
		}
		line[j] = 0;
		say("vayu-frpc: %s %04x: %s\n", way, off, line);
	}
}

/* --------------------------------------------------------------------------------------- */
/* State                                                                                   */
/* --------------------------------------------------------------------------------------- */

struct frpc_arg {
	void *data;
	uint32_t length;
	bool out;
};

struct frpc_in {
	const uint8_t *data;
	uint32_t size;
};

struct frpc_out {
	const void *data;
	uint32_t size;
};

struct frpc_slot {
	vm_offset_t va;			/* kernel virtual address of the packet */
	uint64_t pa;			/* its physical address (below 4 GiB) */
	uint64_t dsp;			/* pa with vayu_frpc_cb in bits 32+ */
	uint64_t ctx;			/* the ctx last used, sequence included */
	bool busy;
};

struct frpc_ring_entry {
	uint64_t ctx;
	uint32_t length;
	int32_t retval;
	uint8_t data[64];
};

struct frpc_file {
	bool used;
	uint8_t virt;			/* 1: a synthesized sysfs file, 0: a fw/sensors.pak file */
	uint32_t ino;
	const uint8_t *data;
	uint32_t size;
	uint32_t pos;
};

struct frpc_dir {
	bool used;
	uint8_t virt;			/* 1: /sys/devices/soc0, 0: a fw/sensors.pak directory */
	uint32_t next;
	char path[FRPC_PATH];
};

struct frpc_pak_entry {
	const uint8_t *path;
	uint32_t path_len;
	uint32_t offset, size, mode;	/* mode 0 file, 1 dir (ref/mksnspak.py:9-12) */
};

static struct vayu_glink *frpc_glink;
static struct vayu_glink_channel *frpc_channel;
static uint32_t frpc_level;		/* vayu_slpi's level, log only */
static uint32_t frpc_mode;		/* vayu_frpc: 1 = serve, 2 = also hex-dump */
static uint32_t frpc_cb = 1;		/* vayu_frpc_cb; 0: plain physical addresses */
static bool frpc_claimed;
static int frpc_smmu = -1;		/* vayu_smmu_claim()'s handle */
static volatile bool frpc_leaving;	/* vayu_fastrpc_quiesce() */
static volatile bool frpc_running;	/* the run thread is alive */

static lck_grp_t *frpc_lock_group;
static lck_spin_t frpc_lock;
static bool frpc_lock_ready;

static struct frpc_slot frpc_slot[FRPC_SLOTS];
static uint64_t frpc_seq;		/* our 48-bit sequence (ref/fastrpc.c:51-53) */

static struct frpc_ring_entry frpc_ring[FRPC_RING];
static unsigned frpc_ring_head, frpc_ring_tail;
static uint32_t frpc_rx_count, frpc_rx_dropped, frpc_tx_count, frpc_dumped_tx, frpc_dumped_rx;

/* The packet buffers, allocated once by the run thread. */
static bool frpc_buffers_ok;

/* The file server. Only the run thread touches these. */
static struct frpc_file frpc_file[FRPC_MAX_FILES];
static struct frpc_dir frpc_dir[FRPC_MAX_DIRS];
static struct frpc_pak_entry frpc_pak[FRPC_PAK_MAX];
static uint32_t frpc_pak_count;
static const uint8_t *frpc_pak_data;
static uint64_t frpc_pak_size;

static uint8_t frpc_reply[FRPC_REPLY_BYTES];	/* the encoded output buffers of one answer */
static uint8_t frpc_read_buf[FRPC_MAX_READ];	/* one fread's data */

static uint32_t frpc_requests, frpc_bytes_read, frpc_unmatched, frpc_unsupported;
static uint64_t frpc_last_summary_ns;

/*
 * The platform files hexagonrpcd synthesizes for the sensors PD. The DSP reads them out of
 * /sys/devices/soc0 (hexagonrpcd/rpcd_builder.c:192-196, which maps that directory to the
 * platform's socinfo directory); the values are HYPOTHESES for a vayu and every one served is
 * logged. Newline endings are the sysfs convention.
 */
static const struct {
	const char *path;
	const char *value;
} frpc_virt[] = {
	{ "/sys/devices/soc0/soc_id", "339\n" },
	{ "/sys/devices/soc0/hw_platform", "MTP\n" },
	{ "/sys/devices/soc0/platform_subtype", "Unknown\n" },
	{ "/sys/devices/soc0/platform_subtype_id", "0\n" },
	{ "/sys/devices/soc0/platform_version", "65536\n" },
	{ "/sys/devices/soc0/revision", "2.2\n" },
};
#define FRPC_VIRT_COUNT (sizeof(frpc_virt) / sizeof(frpc_virt[0]))

/* The env bases hexagonrpcd serves (hexagonrpcd/apps_std.c:373-380). */
static const struct {
	const char *name;
	const char *base;
} frpc_env[] = {
	{ "ADSP_LIBRARY_PATH", "/usr/lib/qcom/adsp/" },
	{ "ADSP_AVS_CFG_PATH", "/vendor/etc/acdbdata/" },
};
#define FRPC_ENV_COUNT (sizeof(frpc_env) / sizeof(frpc_env[0]))

/* --------------------------------------------------------------------------------------- */
/* The GLINK thread's side: copy the answer, wake the run thread's poll                    */
/* --------------------------------------------------------------------------------------- */

void
vayu_fastrpc_input(void *context __unused, const uint8_t *data, uint32_t length)
{
	struct frpc_ring_entry *e;
	uint64_t ctx;

	if (data == NULL || length < 16) {
		frpc_rx_dropped++;
		return;
	}
	ctx = get64(data);
	if (ctx == FRPC_PD_NOTIFY_CTX) {	/* a PD exit notification, not an answer (ref/fastrpc.c:56-63) */
		frpc_rx_dropped++;
		return;
	}
	if (!frpc_lock_ready) {			/* (the channel can open before the run thread starts) */
		frpc_rx_dropped++;
		return;
	}
	lck_spin_lock(&frpc_lock);
	if (frpc_ring_head - frpc_ring_tail >= FRPC_RING) {
		frpc_rx_dropped++;
		lck_spin_unlock(&frpc_lock);
		return;
	}
	e = &frpc_ring[frpc_ring_head % FRPC_RING];
	e->ctx = ctx;
	e->length = length;
	e->retval = (int32_t)get32(data + 8);
	memcpy(e->data, data, length < sizeof(e->data) ? length : sizeof(e->data));
	frpc_ring_head++;
	frpc_rx_count++;
	lck_spin_unlock(&frpc_lock);
	wakeup((caddr_t)(void *)&frpc_ring);	/* (the run thread polls; the wake is a courtesy) */
}

/* --------------------------------------------------------------------------------------- */
/* The packet: one physically contiguous buffer per invoke, below 4 GiB                    */
/* --------------------------------------------------------------------------------------- */

static void
frpc_buffers_free(void)
{
	for (unsigned i = 0; i < FRPC_SLOTS; i++) {
		if (frpc_slot[i].va != 0) {
			kmem_free(kernel_map, frpc_slot[i].va, FRPC_BUF_SIZE);
			frpc_slot[i].va = 0;
			frpc_slot[i].pa = 0;
			frpc_slot[i].dsp = 0;
			frpc_slot[i].busy = false;
		}
	}
	frpc_buffers_ok = false;
}

static bool
frpc_buffers_alloc(void)
{
	for (unsigned i = 0; i < FRPC_SLOTS; i++) {
		uint64_t pa = 0;
		vm_offset_t va = 0;
		bool low = false;

		/* The SDSP's DMA is masked to 32 bits (ref/fastrpc.c:2391, 2510): try again if the
		 * physical allocator hands out memory above 4 GiB. */
		for (unsigned attempt = 0; attempt < 4 && !low; attempt++) {
			/* max_pnum: the last page frame below 4 GiB */
			if (kmem_alloc_contig(kernel_map, &va, FRPC_BUF_SIZE, FRPC_PAGE - 1, 0xfffff, 0,
			    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				say("vayu-frpc: no physically contiguous memory for packet %u (try %u)\n",
				    i, attempt + 1);
				IOSleep(10);
				continue;
			}
			pa = kvtophys(va);
			if (pa != 0 && pa + FRPC_BUF_SIZE <= 0x100000000ULL) {
				low = true;
			} else {
				say("vayu-frpc: packet %u landed at %#llx, above 4 GiB: freed\n", i,
				    (unsigned long long)pa);
				kmem_free(kernel_map, va, FRPC_BUF_SIZE);
				va = 0;
			}
		}
		if (!low) {
			say("vayu-frpc: no packet memory below 4 GiB for packet %u\n", i);
			frpc_buffers_free();
			return false;
		}
		frpc_slot[i].va = va;
		frpc_slot[i].pa = pa;
		frpc_slot[i].dsp = pa | ((uint64_t)frpc_cb << 32);
		frpc_slot[i].busy = false;
		say("vayu-frpc: packet %u: %u KB at physical %#llx, DSP address %#llx\n", i,
		    FRPC_BUF_SIZE >> 10, (unsigned long long)pa, (unsigned long long)frpc_slot[i].dsp);
	}
	frpc_buffers_ok = true;
	return true;
}

/* --------------------------------------------------------------------------------------- */
/* Invoke                                                                                  */
/* --------------------------------------------------------------------------------------- */

static bool
frpc_slot_get(unsigned *index)
{
	lck_spin_lock(&frpc_lock);
	for (unsigned i = 0; i < FRPC_SLOTS; i++) {
		if (!frpc_slot[i].busy) {
			frpc_slot[i].busy = true;
			*index = i;
			lck_spin_unlock(&frpc_lock);
			return true;
		}
	}
	lck_spin_unlock(&frpc_lock);
	return false;
}

static void
frpc_slot_put(unsigned index)
{
	lck_spin_lock(&frpc_lock);
	frpc_slot[index].busy = false;
	lck_spin_unlock(&frpc_lock);
}

static void
frpc_summary(void)
{
	uint64_t now = frpc_now_ns();
	unsigned files = 0, dirs = 0;

	if (frpc_last_summary_ns == 0) {
		frpc_last_summary_ns = now;
		return;
	}
	if (now - frpc_last_summary_ns < 30000000000ULL) {
		return;
	}
	frpc_last_summary_ns = now;
	for (unsigned i = 0; i < FRPC_MAX_FILES; i++) {
		if (frpc_file[i].used) {
			files++;
		}
	}
	for (unsigned i = 0; i < FRPC_MAX_DIRS; i++) {
		if (frpc_dir[i].used) {
			dirs++;
		}
	}
	say("vayu-frpc: 30 s: %u request(s), %u byte(s) read, %u open file(s), %u open dir(s), "
	    "%u tx / %u rx frame(s), %u dropped, %u answer(s) nobody waited for\n", frpc_requests,
	    frpc_bytes_read, files, dirs, frpc_tx_count, frpc_rx_count, frpc_rx_dropped, frpc_unmatched);
}

/*
 * Wait for the answer with our ctx and sequence. The ring is filled by the GLINK thread; only
 * this thread consumes it. Returns false on timeout (timeout_ms == 0: no timeout) or when
 * vayu_fastrpc_quiesce() was called.
 */
static bool
frpc_wait(struct frpc_slot *slot, uint32_t timeout_ms, int32_t *retval)
{
	uint64_t start = frpc_now_ns();

	for (;;) {
		bool found = false;
		uint8_t rx[40];
		uint32_t rx_len = 0;
		int32_t rv = -1;

		lck_spin_lock(&frpc_lock);
		for (unsigned i = frpc_ring_tail; i != frpc_ring_head; i++) {
			struct frpc_ring_entry *e = &frpc_ring[i % FRPC_RING];

			/* id (15:8) and sequence (63:16), not the PD nibble (ref/fastrpc.c:2728-2754) */
			if ((e->ctx & ~0xffULL) == (slot->ctx & ~0xffULL)) {
				frpc_unmatched += i - frpc_ring_tail;	/* not ours: gone */
				rv = e->retval;
				rx_len = e->length < sizeof(rx) ? e->length : (uint32_t)sizeof(rx);
				memcpy(rx, e->data, rx_len);
				frpc_ring_tail = i + 1;
				found = true;
				break;
			}
		}
		lck_spin_unlock(&frpc_lock);
		if (found) {
			*retval = rv;
			if (frpc_mode >= 2 && frpc_dumped_rx < 16) {	/* (dumped here, not on the GLINK thread) */
				frpc_dumped_rx++;
				frpc_hexdump("rx", rx, rx_len);
			}
			return true;
		}
		if (frpc_leaving) {
			return false;
		}
		if (timeout_ms != 0 && frpc_now_ns() - start >= (uint64_t)timeout_ms * 1000000ULL) {
			return false;
		}
		frpc_summary();
		IOSleep(1);
	}
}

/*
 * One FastRPC invoke: args[0 .. nargs) are the scalars in order, all input buffers first and
 * then the output buffers (ref/fastrpc.c:1092-1192, 1225-1240). The packet layout, the DSP
 * addresses and the copy-back follow ref/fastrpc.c exactly; see the file comment.
 */
static int32_t
frpc_invoke(uint32_t handle, uint32_t sc, const struct frpc_arg *args, uint32_t nargs, uint32_t timeout_ms)
{
	uint64_t metalen, off, pkt_size;
	uint32_t offsets[FRPC_MAX_ARGS];
	struct fastrpc_msg msg;
	struct frpc_slot *slot;
	unsigned index;
	int32_t retval = -1;
	int err;

	if (nargs > FRPC_MAX_ARGS) {
		say("vayu-frpc: invoke with %u arguments refused\n", nargs);
		return -1;
	}
	if (frpc_glink == NULL || frpc_channel == NULL || !frpc_buffers_ok) {
		return -1;
	}
	if (!frpc_slot_get(&index)) {
		say("vayu-frpc: both packets are busy; invoke (handle %u) dropped\n", handle);
		return -1;
	}
	slot = &frpc_slot[index];

	/* 40 bytes of metadata per scalar plus the fixed tail (ref/fastrpc.c:1009-1021). */
	metalen = (uint64_t)FRPC_META_PER * nargs + FRPC_META_FIXED;
	off = (metalen + (FRPC_ALIGN - 1)) & ~(uint64_t)(FRPC_ALIGN - 1);
	for (uint32_t i = 0; i < nargs; i++) {
		offsets[i] = 0;
		if (args[i].length == 0) {
			continue;
		}
		off = (off + (FRPC_ALIGN - 1)) & ~(uint64_t)(FRPC_ALIGN - 1);
		offsets[i] = (uint32_t)off;
		off += args[i].length;
	}
	pkt_size = off;
	if (pkt_size > FRPC_BUF_SIZE) {
		say("vayu-frpc: invoke (handle %u, sc %#x) needs %llu bytes: too large\n", handle, sc,
		    (unsigned long long)pkt_size);
		frpc_slot_put(index);
		return -1;
	}

	{
		uint8_t *base = (uint8_t *)slot->va;
		uint64_t dsp = slot->dsp;

		memset(base, 0, (size_t)pkt_size);
		for (uint32_t i = 0; i < nargs; i++) {
			uint32_t len = args[i].length;
			uint64_t pv = dsp + offsets[i];

			/* rpra[i] = {pv, len}: pv is OUR virtual address of the argument, as upstream
			 * puts it (ref/fastrpc.c:1163 rpra[i].buf.pv = args - offset); the DSP finds the
			 * bytes through phy_page[i] (DSP address, page-aligned) + (pv & page offset). */
			put64(base + 16 * i, len != 0 ? (uint64_t)(slot->va + offsets[i]) : 0);
			put64(base + 16 * i + 8, len);
			/* invoke_buf[i] = {num, pgidx} (ref/fastrpc.c:1127-1130). */
			put32(base + 16 * nargs + 8 * i, len != 0 ? 1 : 0);
			put32(base + 16 * nargs + 8 * i + 4, i);
			if (len != 0) {
				uint64_t first = pv >> 12;
				uint64_t last = (pv + len - 1) >> 12;

				/* phy_page[i] = {page-aligned address, pages spanned}
				 * (ref/fastrpc.c:1166-1173). */
				put64(base + 24 * nargs + 16 * i, pv & ~(uint64_t)(FRPC_PAGE - 1));
				put64(base + 24 * nargs + 16 * i + 8, (last - first + 1) * FRPC_PAGE);
			}
			if (!args[i].out && len != 0) {
				memcpy(base + offsets[i], args[i].data, len);
			}
		}
		/* fdlist[16], crclist[64] and the poll word stay zero: no fds, no CRC, no poll. */
	}

	slot->ctx = ((frpc_seq + 1) << 16) | ((uint64_t)(index + 1) << 8) | FRPC_PD;
	frpc_seq++;

	memset(&msg, 0, sizeof(msg));
	msg.client_id = FRPC_CLIENT_ID;
	msg.tid = FRPC_TID;
	msg.ctx = slot->ctx;
	msg.handle = handle;
	msg.sc = sc;
	msg.addr = slot->dsp;
	msg.size = (pkt_size + FRPC_PAGE - 1) & ~(uint64_t)(FRPC_PAGE - 1);

	__asm__ volatile ("dsb sy" : : : "memory");	/* the DSP must see every byte first */
	if (frpc_mode >= 2 && frpc_dumped_tx < 16) {
		frpc_dumped_tx++;
		frpc_hexdump("tx", (const uint8_t *)&msg, sizeof(msg));
	}
	frpc_tx_count++;
	err = vayu_glink_send(frpc_glink, frpc_channel, &msg, sizeof(msg));
	if (err != 0) {		/* vayu_glink_send: 0 or an errno (EMSGSIZE is 40 = sizeof(msg)!) */
		say("vayu-frpc: GLINK refused a %u-byte message (handle %u, sc %#x): %d\n",
		    (uint32_t)sizeof(msg), handle, sc, err);
		frpc_slot_put(index);
		return -1;
	}

	if (!frpc_wait(slot, timeout_ms, &retval)) {
		if (frpc_leaving) {
			say("vayu-frpc: quiesced while waiting for handle %u sc %#x\n", handle, sc);
		} else {
			say("vayu-frpc: no answer for handle %u sc %#x after %u ms\n", handle, sc, timeout_ms);
		}
		frpc_slot_put(index);
		return -1;
	}
	__asm__ volatile ("dsb sy" : : : "memory");	/* and we must see every byte it wrote */

	{
		uint8_t *base = (uint8_t *)slot->va;

		for (uint32_t i = 0; i < nargs; i++) {
			if (args[i].out && args[i].length != 0) {
				/* fastrpc_put_args: the outputs live in the same packet
				 * (ref/fastrpc.c:1225-1240). */
				memcpy(args[i].data, base + offsets[i], args[i].length);
			}
		}
	}
	frpc_slot_put(index);
	return retval;
}

/* --------------------------------------------------------------------------------------- */
/* The buffer sequence of the reverse protocol (hexagonrpcd/iobuffer.c)                     */
/* --------------------------------------------------------------------------------------- */

/* Decode n buffers out of a request's byte sequence: u32 length, pad to 8, bytes. A zero-size
 * buffer has no padding of its own and the next length follows at once
 * (hexagonrpcd/iobuffer.c:130-192: the alignment is only consumed before data). */
static bool
frpc_decode_inbufs(uint32_t n, const uint8_t *blob, uint32_t blob_len, struct frpc_in *in, uint32_t max)
{
	uint32_t pos = 0;

	if (n > max) {
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint32_t size;

		if (blob_len - pos < 4) {
			return false;
		}
		size = get32(blob + pos);
		pos += 4;
		in[i].data = blob + pos;
		in[i].size = size;
		if (size == 0) {
			continue;
		}
		while ((pos & 7) != 0) {
			if (pos >= blob_len) {
				return false;
			}
			pos++;
		}
		if (size > blob_len - pos) {
			return false;
		}
		in[i].data = blob + pos;
		pos += size;
	}
	return true;
}

/* Size of the encoded sequence, with no padding around a zero-size buffer and none after the
 * last buffer (hexagonrpcd/iobuffer.c:194-228 as its encoder actually writes it). */
static uint32_t
frpc_outbufs_size(uint32_t n, const struct frpc_out *out)
{
	uint32_t pos = 0;

	for (uint32_t i = 0; i < n; i++) {
		pos += 4;
		if (out[i].size != 0) {
			while ((pos & 7) != 0) {
				pos++;
			}
			pos += out[i].size;
		}
	}
	return pos;
}

/* Encode n buffers: u32 length, pad to 8, bytes (hexagonrpcd/iobuffer.c:194-228). */
static bool
frpc_encode_outbufs(uint32_t n, const struct frpc_out *out, uint8_t *to, uint32_t cap, uint32_t *length)
{
	uint32_t pos = 0;

	for (uint32_t i = 0; i < n; i++) {
		if (cap - pos < 4) {
			return false;
		}
		put32(to + pos, out[i].size);
		pos += 4;
		if (out[i].size == 0) {
			continue;
		}
		while ((pos & 7) != 0) {
			if (pos >= cap) {
				return false;
			}
			to[pos++] = 0;
		}
		if (out[i].size > cap - pos) {
			return false;
		}
		memcpy(to + pos, out[i].data, out[i].size);
		pos += out[i].size;
	}
	*length = pos;
	return true;
}

/* --------------------------------------------------------------------------------------- */
/* fw/sensors.pak                                                                          */
/* --------------------------------------------------------------------------------------- */

static bool
frpc_pak_parse(const uint8_t *data, uint64_t size)
{
	uint32_t count;

	if (size < 16 || memcmp(data, "SNSPAK1\0", 8) != 0) {
		return false;
	}
	count = get32(data + 8);
	if (count > FRPC_PAK_MAX) {
		say("vayu-frpc: fw/sensors.pak has %u entries: only %u are kept\n", count, FRPC_PAK_MAX);
		count = FRPC_PAK_MAX;
	}
	if (size < 16 + (uint64_t)count * 208) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = data + 16 + (uint64_t)i * 208;
		uint32_t plen = 0;

		while (plen < 192 && e[plen] != 0) {
			plen++;
		}
		if (plen == 0 || plen == 192) {
			continue;
		}
		frpc_pak[i].path = e;
		frpc_pak[i].path_len = plen;
		frpc_pak[i].offset = get32(e + 192);
		frpc_pak[i].size = get32(e + 196);
		frpc_pak[i].mode = get32(e + 200);
		if ((uint64_t)frpc_pak[i].offset + frpc_pak[i].size > size) {
			say("vayu-frpc: fw/sensors.pak entry %u points outside the file; dropped\n", i);
			frpc_pak[i].path = NULL;
		}
	}
	frpc_pak_count = count;
	return true;
}

static void
frpc_pak_load(void)
{
	uint64_t size = 0;

	frpc_pak_data = hakios_image_file("fw", "sensors.pak", &size, NULL);
	frpc_pak_size = size;
	if (frpc_pak_data == NULL || size == 0) {
		say("vayu-frpc: no fw/sensors.pak: only the synthesized files are served\n");
		frpc_pak_data = NULL;
		return;
	}
	if (!frpc_pak_parse(frpc_pak_data, frpc_pak_size)) {
		say("vayu-frpc: fw/sensors.pak (%llu bytes) is not the SNSPAK1 format of ref/mksnspak.py\n",
		    (unsigned long long)frpc_pak_size);
		frpc_pak_data = NULL;
		frpc_pak_count = 0;
		return;
	}
	say("vayu-frpc: fw/sensors.pak: %llu bytes, %u entries\n", (unsigned long long)frpc_pak_size,
	    frpc_pak_count);
}

static int
frpc_pak_exact(const char *path)
{
	size_t n = strlen(path);

	if (frpc_pak_data == NULL) {
		return -1;
	}
	for (uint32_t i = 0; i < frpc_pak_count; i++) {
		if (frpc_pak[i].path != NULL && frpc_pak[i].path_len == n &&
		    memcmp(frpc_pak[i].path, path, n) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/*
 * The same file under the two registry prefixes: the pak may have been built with either
 * (ref/hexagonrpc/hexagonrpcd/rpcd_builder.c:159-169 maps the same tree at /persist and
 * /mnt/vendor/persist).
 */
static int
frpc_pak_find(const char *path)
{
	char alt[FRPC_PATH];
	int i;

	i = frpc_pak_exact(path);
	if (i >= 0) {
		return i;
	}
	if (strncmp(path, "/mnt/vendor/persist/", 20) == 0) {
		frpc_copy(alt, sizeof(alt), path + 11);		/* "/mnt/vendor" is 11 characters */
		return frpc_pak_exact(alt);
	}
	if (strncmp(path, "/persist/", 9) == 0) {
		snprintf(alt, sizeof(alt), "/mnt/vendor%s", path);
		return frpc_pak_exact(alt);
	}
	return -1;
}

static int
frpc_virt_find(const char *path)
{
	for (unsigned i = 0; i < FRPC_VIRT_COUNT; i++) {
		if (strcmp(path, frpc_virt[i].path) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static const char *
frpc_env_base(const char *env)
{
	for (unsigned i = 0; i < FRPC_ENV_COUNT; i++) {
		if (strcmp(env, frpc_env[i].name) == 0) {
			return frpc_env[i].base;
		}
	}
	return NULL;
}

/*
 * Full path the file server should look at. An absolute path is used as it is; a relative one
 * is joined to the base the DSP named (hexagonrpcd/apps_std.c:175-191). An unknown env is
 * logged and treated as "/" so the request still shows up in the log.
 */
static void
frpc_resolve(const char *env, const char *path, const char *relative_base, char *full, size_t cap)
{
	const char *base;

	if (path[0] == '/') {
		frpc_copy(full, cap, path);
		return;
	}
	base = relative_base != NULL ? relative_base : frpc_env_base(env);
	if (base == NULL) {
		say("vayu-frpc: relative path \"%s\" with unknown env \"%s\": trying it as absolute\n",
		    path, env);
		base = "/";
	}
	if (base[0] != 0 && base[strlen(base) - 1] == '/') {
		snprintf(full, cap, "%s%s", base, path);
	} else {
		snprintf(full, cap, "%s/%s", base, path);
	}
}

/* For opendir, whose paths hexagonrpcd resolves against its root (apps_std.c:223). */
static void
frpc_resolve_abs(const char *path, char *full, size_t cap)
{
	if (path[0] == '/') {
		frpc_copy(full, cap, path);
	} else {
		snprintf(full, cap, "/%s", path);
	}
}

/* Strip one trailing slash (but not the root's). */
static void
frpc_trim_slash(char *path)
{
	size_t n = strlen(path);

	while (n > 1 && path[n - 1] == '/') {
		path[--n] = 0;
	}
}

/* --------------------------------------------------------------------------------------- */
/* The file and directory tables                                                           */
/* --------------------------------------------------------------------------------------- */

static struct frpc_file *
frpc_file_get(uint32_t fd)
{
	if (fd == 0 || fd > FRPC_MAX_FILES) {
		return NULL;
	}
	if (!frpc_file[fd - 1].used) {
		return NULL;
	}
	return &frpc_file[fd - 1];
}

static struct frpc_dir *
frpc_dir_get(uint64_t handle)
{
	uint64_t index;

	if (handle < FRPC_DIR_BASE || handle >= FRPC_DIR_BASE + FRPC_MAX_DIRS) {
		return NULL;
	}
	index = handle - FRPC_DIR_BASE;
	if (!frpc_dir[index].used) {
		return NULL;
	}
	return &frpc_dir[index];
}

static int
frpc_file_slot(void)
{
	for (unsigned i = 0; i < FRPC_MAX_FILES; i++) {
		if (!frpc_file[i].used) {
			return (int)i;
		}
	}
	return -1;
}

static int
frpc_dir_slot(void)
{
	for (unsigned i = 0; i < FRPC_MAX_DIRS; i++) {
		if (!frpc_dir[i].used) {
			return (int)i;
		}
	}
	return -1;
}

static bool
frpc_dir_exists(const char *path)
{
	char full[FRPC_PATH];
	int e;

	frpc_copy(full, sizeof(full), path);
	frpc_trim_slash(full);
	if (strcmp(full, "/sys/devices/soc0") == 0) {
		return true;
	}
	e = frpc_pak_find(full);
	return e >= 0 && frpc_pak[e].mode == 1;
}

/* The name of the entry-th child of dir, or false at the end. */
static bool
frpc_dir_child(const struct frpc_dir *dir, uint32_t entry, char *name, size_t cap)
{
	if (dir->virt) {
		if (entry >= FRPC_VIRT_COUNT) {
			return false;
		}
		frpc_copy(name, cap, frpc_virt[entry].path + strlen("/sys/devices/soc0/"));
		return true;
	}
	{
		size_t plen = strlen(dir->path);
		uint32_t seen = 0;

		for (uint32_t i = 0; i < frpc_pak_count; i++) {
			const struct frpc_pak_entry *e = &frpc_pak[i];
			const char *child;
			uint32_t child_len;
			bool nested = false;

			if (e->path == NULL || e->path_len <= plen + 1) {
				continue;
			}
			if (memcmp(e->path, dir->path, plen) != 0 || e->path[plen] != '/') {
				continue;
			}
			child = (const char *)e->path + plen + 1;
			child_len = e->path_len - plen - 1;
			for (uint32_t k = 0; k < child_len; k++) {
				if (child[k] == '/') {
					nested = true;
					break;
				}
			}
			if (nested) {
				continue;
			}
			if (seen == entry) {
				uint32_t n = child_len < cap - 1 ? child_len : (uint32_t)cap - 1;

				memcpy(name, child, n);
				name[n] = 0;
				return true;
			}
			seen++;
		}
	}
	return false;
}

/* --------------------------------------------------------------------------------------- */
/* apps_std                                                                                */
/* --------------------------------------------------------------------------------------- */

/* The 96-byte stat block hexagonrpcd fills (hexagonrpcd/apps_std.c:289-352). */
static void
frpc_stat_fill(uint8_t *out, uint32_t size, uint32_t ino, uint32_t mode, uint32_t file_size)
{
	memset(out, 0, size);
	put64(out + 0, 0);		/* tsz: unknown purpose, zero (apps_std.c:336) */
	put64(out + 8, 0);		/* dev */
	put64(out + 16, ino);
	put32(out + 24, mode);
	put32(out + 28, 1);		/* nlink */
	put64(out + 32, 0);		/* rdev */
	put64(out + 40, file_size);
	/* atime, atimensec, mtime, mtimensec, ctime, ctimensec: all zero. */
}

/* Does this input buffer look like a NUL-terminated string (hexagonrpcd/apps_std.c:162-165)? */
static bool
frpc_string_ok(const struct frpc_in *in)
{
	return in->size != 0 && in->data[in->size - 1] == 0;
}

/*
 * One request from the DSP. Fills reply[] with the encoded output buffers and returns the AEE
 * result the DSP's blocked call will see. Every known method announces its output buffers even
 * when it fails, so the DSP's decoder always finds the sizes it expects.
 */
static uint32_t
frpc_serve_request(uint32_t handle, uint32_t sc, const uint8_t *blob, uint32_t blob_len,
    uint8_t *reply, uint32_t *reply_len)
{
	struct frpc_in in[FRPC_MAX_IN];
	struct frpc_out out[FRPC_MAX_OUT];
	uint32_t nin = FRPC_SC_IN(sc);
	uint32_t nout = FRPC_SC_OUT(sc);
	uint32_t method, result = AEE_SUCCESS, outc = 0;
	uint8_t prim[264];		/* readdir's {inode, name[256], eof} is the largest */
	uint8_t statb[104];
	struct {
		uint32_t written;
		uint32_t eof;
	} fread_out;
	uint32_t fd_out = 0;
	uint64_t dir_out = 0;

	*reply_len = 0;
	frpc_requests++;
	if (frpc_requests <= 32) {
		say("vayu-frpc: request %u: handle %u method %u sc %#x, %u inbuf(s), %u outbuf(s)\n",
		    frpc_requests, handle, FRPC_SC_METHOD(sc), sc, nin, nout);
	}
	if (nin > FRPC_MAX_IN || nout > FRPC_MAX_OUT) {
		say("vayu-frpc: request with %u in / %u out buffers is beyond the table\n", nin, nout);
		result = AEE_EBADPARM;
		if (nin > FRPC_MAX_IN) {
			nin = 0;		/* do not hand a half-known argument list to a method */
		}
		nout = frpc_min32(nout, FRPC_MAX_OUT);
	}
	if (!frpc_decode_inbufs(nin, blob, blob_len, in, FRPC_MAX_IN)) {
		say("vayu-frpc: handle %u sc %#x: the %u input buffer(s) do not decode\n", handle, sc, nin);
		result = AEE_EBADPARM;
		nin = 0;
	}
	method = FRPC_SC_METHOD(sc);
	if (method == FRPC_STAT_MSG_ID && nin > 0 && in[0].size >= 4) {
		/* Extended method id: the first u32 of the primary input buffer
		 * (hexagonrpcd/listener.c:501-513). */
		method = get32(in[0].data);
	}

	if (handle == 0) {
		/* remotectl: the DSP may try to resolve an interface by name. Not served here. */
		if (frpc_unsupported++ < 32) {
			say("vayu-frpc: handle 0 (remotectl) method %u: not supported\n", method);
		}
		result = AEE_EUNSUPPORTED;
	} else if (handle != 1) {
		if (frpc_unsupported++ < 32) {
			say("vayu-frpc: handle %u method %u: not supported\n", handle, method);
		}
		result = AEE_EUNSUPPORTED;
	} else {
		switch (method) {
		case 2:		/* fflush: {fd}, nothing out (apps_std.c:53-66) */
			result = AEE_SUCCESS;
			break;

		case 3:		/* fclose: {fd} (apps_std.c:68-87) */
			if (nin < 1 || in[0].size < 4) {
				result = AEE_EBADPARM;
				break;
			}
			{
				struct frpc_file *f = frpc_file_get(get32(in[0].data));

				if (f == NULL) {
					result = AEE_EFAILED;
				} else {
					f->used = false;
				}
			}
			break;

		case 4: {	/* fread: {fd, size} -> {written, eof} + data (apps_std.c:89-121) */
			uint32_t fd, size = 0;
			struct frpc_file *f;

			memset(&fread_out, 0, sizeof(fread_out));
			out[0].data = &fread_out;
			out[0].size = 8;
			out[1].data = frpc_read_buf;
			out[1].size = 0;
			outc = 2;
			if (nin < 1 || in[0].size < 8) {
				result = AEE_EBADPARM;
				break;
			}
			fd = get32(in[0].data);
			size = get32(in[0].data + 4);
			if (size > FRPC_MAX_READ) {
				say("vayu-frpc: fread of %u bytes refused (at most %u)\n", size, FRPC_MAX_READ);
				result = AEE_EBUFFERTOOSMALL;
				break;
			}
			out[1].size = size;	/* the DSP expects this many bytes back */
			memset(frpc_read_buf, 0, size);
			f = frpc_file_get(fd);
			if (f == NULL) {
				result = AEE_EBADPARM;
				break;
			}
			{
				uint32_t n = frpc_min32(size, f->size - f->pos);

				if (n != 0) {
					memcpy(frpc_read_buf, f->data + f->pos, n);
				}
				f->pos += n;
				fread_out.written = n;
				fread_out.eof = n < size;	/* apps_std.c:118 */
				frpc_bytes_read += n;
			}
			break;
		}

		case 9: {	/* fseek: {fd, pos, whence} (apps_std.c:123-151) */
			uint32_t fd, pos, whence, newpos;
			struct frpc_file *f;

			if (nin < 1 || in[0].size < 12) {
				result = AEE_EBADPARM;
				break;
			}
			fd = get32(in[0].data);
			pos = get32(in[0].data + 4);
			whence = get32(in[0].data + 8);
			f = frpc_file_get(fd);
			if (f == NULL || whence > 2) {
				result = AEE_EBADPARM;
				break;
			}
			switch (whence) {
			case 0:			/* SEEK_SET */
				newpos = pos;
				break;
			case 1:			/* SEEK_CUR */
				newpos = f->pos + pos;
				break;
			default:		/* SEEK_END */
				newpos = f->size + pos;
				break;
			}
			if (newpos > f->size) {
				newpos = f->size;
			}
			f->pos = newpos;
			break;
		}

		case 19: {	/* fopen_with_env: {env, -, path, mode} -> {fd} (apps_std.c:153-209) */
			const char *env, *path, *mode;
			char full[FRPC_PATH];
			int e;

			put32(prim, 0);
			out[0].data = prim;
			out[0].size = 4;
			outc = 1;
			if (nin < 5 || in[0].size < 16 || !frpc_string_ok(&in[1]) ||
			    !frpc_string_ok(&in[3]) || !frpc_string_ok(&in[4])) {
				result = AEE_EBADPARM;
				break;
			}
			env = (const char *)in[1].data;
			path = (const char *)in[3].data;
			mode = (const char *)in[4].data;
			if (mode[0] == 'w' || mode[0] == 'a') {
				say("vayu-frpc: fopen \"%s\" (env %s): write mode \"%s\" refused\n", path, env, mode);
				result = AEE_EUNSUPPORTED;	/* apps_std.c:168-173 */
				break;
			}
			frpc_resolve(env, path, NULL, full, sizeof(full));
			e = frpc_pak_find(full);
			if (e >= 0 && frpc_pak[e].mode == 0) {
				int slot = frpc_file_slot();

				if (slot < 0) {
					say("vayu-frpc: fopen \"%s\" (env %s): all %u files are open\n", path, env,
					    FRPC_MAX_FILES);
					result = AEE_ENOMEMORY;
					break;
				}
				frpc_file[slot].used = true;
				frpc_file[slot].virt = 0;
				frpc_file[slot].ino = e + 1;
				frpc_file[slot].data = frpc_pak_data + frpc_pak[e].offset;
				frpc_file[slot].size = frpc_pak[e].size;
				frpc_file[slot].pos = 0;
				fd_out = slot + 1;
				say("vayu-frpc: fopen \"%s\" (env %s) -> fd %u, size %u\n", path, env,
				    fd_out, frpc_file[slot].size);
			} else {
				int v = frpc_virt_find(full);

				if (v >= 0) {
					int slot = frpc_file_slot();

					if (slot < 0) {
						say("vayu-frpc: fopen \"%s\" (env %s): all %u files are open\n",
						    path, env, FRPC_MAX_FILES);
						result = AEE_ENOMEMORY;
						break;
					}
					frpc_file[slot].used = true;
					frpc_file[slot].virt = 1;
					frpc_file[slot].ino = 1000 + v;
					frpc_file[slot].data = (const uint8_t *)frpc_virt[v].value;
					frpc_file[slot].size = (uint32_t)strlen(frpc_virt[v].value);
					frpc_file[slot].pos = 0;
					fd_out = slot + 1;
					{
						const char *value = frpc_virt[v].value;
						int vlen = (int)strlen(value);

						if (vlen > 0 && value[vlen - 1] == '\n') {
							vlen--;		/* (the file keeps the newline; the log does not) */
						}
						say("vayu-frpc: fopen \"%s\" (env %s) -> fd %u, size %u; "
						    "virtual value \"%.*s\"\n", path, env, fd_out,
						    frpc_file[slot].size, vlen, value);
					}
				} else {
					say("vayu-frpc: fopen \"%s\" (env %s) -> ENOENT\n", path, env);
					result = AEE_EFAILED;
				}
			}
			put32(prim, fd_out);
			break;
		}

		case 26: {	/* opendir: {path} -> {dir u64} (apps_std.c:211-238) */
			char full[FRPC_PATH];

			put64(prim, 0);
			out[0].data = prim;
			out[0].size = 8;
			outc = 1;
			if (nin < 2 || in[0].size < 4 || !frpc_string_ok(&in[1])) {
				result = AEE_EBADPARM;
				break;
			}
			frpc_resolve_abs((const char *)in[1].data, full, sizeof(full));
			frpc_trim_slash(full);
			if (frpc_dir_exists(full)) {
				int slot = frpc_dir_slot();

				if (slot < 0) {
					say("vayu-frpc: opendir \"%s\": all %u dirs are open\n",
					    (const char *)in[1].data, FRPC_MAX_DIRS);
					result = AEE_ENOMEMORY;
					break;
				}
				frpc_dir[slot].used = true;
				frpc_dir[slot].virt = strcmp(full, "/sys/devices/soc0") == 0;
				frpc_dir[slot].next = 0;
				frpc_copy(frpc_dir[slot].path, sizeof(frpc_dir[slot].path), full);
				dir_out = FRPC_DIR_BASE + slot;
				say("vayu-frpc: opendir \"%s\" -> dir %llu\n",
				    (const char *)in[1].data, (unsigned long long)dir_out);
			} else {
				say("vayu-frpc: opendir \"%s\" -> ENOENT\n", (const char *)in[1].data);
				result = AEE_EFAILED;
			}
			put64(prim, dir_out);
			break;
		}

		case 27:	/* closedir: {dir u64} (apps_std.c:240-257) */
			if (nin < 1 || in[0].size < 8) {
				result = AEE_EBADPARM;
				break;
			}
			{
				struct frpc_dir *d = frpc_dir_get(get64(in[0].data));

				if (d == NULL) {
					result = AEE_EFAILED;
				} else {
					d->used = false;
				}
			}
			break;

		case 28: {	/* readdir: {dir u64} -> {inode, name[256], eof} (apps_std.c:259-287) */
			struct frpc_dir *d;
			char name[256];

			memset(prim, 0, sizeof(prim));
			out[0].data = prim;
			out[0].size = 264;
			outc = 1;
			if (nin < 1 || in[0].size < 8) {
				result = AEE_EBADPARM;
				break;
			}
			d = frpc_dir_get(get64(in[0].data));
			if (d == NULL) {
				result = AEE_EFAILED;
				break;
			}
			if (frpc_dir_child(d, d->next, name, sizeof(name))) {
				d->next++;
				memcpy(prim + 4, name, frpc_min32((uint32_t)strlen(name), 255));
				put32(prim, 0);				/* inode: apps_std.c:283 */
				put32(prim + 260, 0);			/* not eof */
			} else {
				put32(prim, 0);
				put32(prim + 260, 1);			/* eof (apps_std.c:284) */
			}
			break;
		}

		case 31: {	/* stat: {ext id, path} -> 96 (or 104) bytes (apps_std.c:289-352) */
			char full[FRPC_PATH];
			const char *path;
			uint32_t out_size = nout >= 2 ? 104 : 96;
			uint32_t ino = 0, mode = 0, file_size = 0;
			int e, v;

			frpc_stat_fill(statb, sizeof(statb), 0, 0, 0);
			out[0].data = statb;
			out[0].size = out_size;
			outc = 1;
			if (nout >= 2) {		/* the 104-byte variant: 96 + 8 */
				out[1].data = statb + 96;
				out[1].size = 8;
				outc = 2;
			}
			if (nin < 2 || in[0].size < 8 || !frpc_string_ok(&in[1])) {
				result = AEE_EBADPARM;
				break;
			}
			path = (const char *)in[1].data;
			/* hexagonrpcd resolves a relative stat under /usr/lib/qcom/adsp/
			 * (apps_std.c:316). */
			frpc_resolve("", path, "/usr/lib/qcom/adsp/", full, sizeof(full));
			e = frpc_pak_find(full);
			if (e >= 0) {
				ino = e + 1;
				mode = frpc_pak[e].mode == 1 ? 0040000 | 0555 : 0100000 | 0444;
				file_size = frpc_pak[e].mode == 1 ? 0 : frpc_pak[e].size;
			} else {
				v = frpc_virt_find(full);
				if (v >= 0) {
					ino = 1000 + v;
					mode = 0100000 | 0444;
					file_size = (uint32_t)strlen(frpc_virt[v].value);
				} else if (strcmp(full, "/sys/devices/soc0") == 0) {
					ino = 2000;
					mode = 0040000 | 0555;
					file_size = 0;
				} else {
					say("vayu-frpc: stat \"%s\" -> ENOENT (out %u bytes)\n", path, out_size);
					result = AEE_EFAILED;
					break;
				}
			}
			frpc_stat_fill(statb, out_size, ino, mode, file_size);
			say("vayu-frpc: stat \"%s\" -> size %u, mode %#x, out buffer %u bytes "
			    "(request says %u out)\n", path, file_size, mode, out_size, nout);
			break;
		}

		default:
			if (frpc_unsupported++ < 32) {
				say("vayu-frpc: apps_std method %u (sc %#x) is not supported\n", method, sc);
			}
			result = AEE_EUNSUPPORTED;
			break;
		}
	}

	/* The DSP's listener decodes exactly sc's out count (hexagonrpcd/iobuffer.c:130-192). */
	if (outc > nout) {
		say("vayu-frpc: method %u produced %u out buffers, the request expects %u\n", method, outc, nout);
		outc = nout;
	}
	while (outc < nout) {
		out[outc].data = NULL;
		out[outc].size = 0;
		outc++;
	}
	if (frpc_outbufs_size(outc, out) > FRPC_REPLY_BYTES ||
	    !frpc_encode_outbufs(outc, out, reply, FRPC_REPLY_BYTES, reply_len)) {
		say("vayu-frpc: method %u: the answer does not fit %u bytes\n", method, FRPC_REPLY_BYTES);
		*reply_len = 0;
		result = AEE_EBUFFERTOOSMALL;
	}
	return result;
}

/* --------------------------------------------------------------------------------------- */
/* The listener sequence                                                                   */
/* --------------------------------------------------------------------------------------- */

static bool
frpc_attach(void)
{
	int32_t client = FRPC_CLIENT_ID;
	struct frpc_arg arg;
	int32_t ret;

	arg.data = &client;
	arg.length = sizeof(client);
	arg.out = false;
	/* INIT_ATTACH: handle 1, method 0, one int in, no out (ref/fastrpc.c:1842-1856). */
	ret = frpc_invoke(FRPC_INIT_HANDLE, FRPC_SC(0, 1, 0), &arg, 1, 5000);
	say("vayu-frpc: INIT_ATTACH for the sensors PD (client %#x): retval %d\n", FRPC_CLIENT_ID, ret);
	return ret == 0;
}

static bool
frpc_remotectl_open(uint32_t *handle)
{
	char name[] = "adsp_default_listener";
	uint32_t prim[2];
	struct {
		uint32_t handle;
		uint32_t error;
	} ret;
	uint8_t err[256];
	struct frpc_arg args[4];
	int32_t rv;

	prim[0] = sizeof(name);			/* strlen + 1 (libhexagonrpc/handle.c:34) */
	prim[1] = sizeof(err);
	memset(&ret, 0, sizeof(ret));
	memset(err, 0, sizeof(err));
	args[0].data = prim;
	args[0].length = sizeof(prim);
	args[0].out = false;
	args[1].data = name;
	args[1].length = sizeof(name);
	args[1].out = false;
	args[2].data = &ret;
	args[2].length = sizeof(ret);
	args[2].out = true;
	args[3].data = err;
	args[3].length = sizeof(err);
	args[3].out = true;
	/* remotectl open: handle 0, method 0, 2 in, 2 out
	 * (libhexagonrpc/interface/remotectl.c:25-38). */
	rv = frpc_invoke(0, FRPC_SC(0, 2, 2), args, 4, 5000);
	err[sizeof(err) - 1] = 0;
	say("vayu-frpc: remotectl open \"%s\": retval %d, handle %u, dlerr %u%s%s\n", name, rv,
	    ret.handle, ret.error, ret.error != 0 && err[0] != 0 ? ", " : "",
	    ret.error != 0 && err[0] != 0 ? (const char *)err : "");
	if (rv != 0 || ret.error != 0 || ret.handle == 0) {
		/* Handle 0 is the static remotectl itself, never the listener (ref/fastrpc.c:49
		 * FASTRPC_MAX_STATIC_HANDLE is 20 and the DSP's dynamic handles are above it). */
		return false;
	}
	*handle = ret.handle;
	return true;
}

static bool
frpc_register(uint32_t handle)
{
	int32_t rv = frpc_invoke(handle, 0, NULL, 0, 5000);

	say("vayu-frpc: adsp_default_listener register (handle %u): retval %d\n", handle, rv);
	return rv == 0;
}

static bool
frpc_init2(void)
{
	int32_t rv = frpc_invoke(FRPC_LISTENER_HANDLE, FRPC_SC(3, 0, 0), NULL, 0, 5000);

	say("vayu-frpc: adsp_listener init2 (handle %u, method 3): retval %d\n", FRPC_LISTENER_HANDLE, rv);
	return rv == 0;
}

/*
 * next2: carry the previous answer, wait for the next request. No timeout -- the DSP may ask
 * nothing for minutes; vayu_fastrpc_quiesce() ends the wait.
 */
static void
frpc_listener_loop(void)
{
	uint32_t rctx = 0, result = 0xffffffff;	/* the first call (hexagonrpcd/listener.c:569-572) */
	uint32_t reply_len = 0;
	uint32_t errors = 0;

	for (;;) {
		struct frpc_arg args[4];
		uint32_t prim[4];
		uint32_t back[4];
		uint8_t inbufs[256];
		int32_t rv;

		prim[0] = rctx;
		prim[1] = result;
		prim[2] = reply_len;
		prim[3] = sizeof(inbufs);
		memset(back, 0, sizeof(back));
		memset(inbufs, 0, sizeof(inbufs));

		args[0].data = prim;
		args[0].length = sizeof(prim);
		args[0].out = false;
		args[1].data = frpc_reply;
		args[1].length = reply_len;
		args[1].out = false;
		args[2].data = back;
		args[2].length = sizeof(back);
		args[2].out = true;
		args[3].data = inbufs;
		args[3].length = sizeof(inbufs);
		args[3].out = true;

		rv = frpc_invoke(FRPC_LISTENER_HANDLE, FRPC_SC(4, 2, 2), args, 4, 0);
		if (frpc_leaving) {
			return;
		}
		if (rv != 0) {
			if (errors++ < 8) {
				say("vayu-frpc: next2 returned %d; the listener is not answering\n", rv);
			}
			if (errors > 100) {
				say("vayu-frpc: next2 failed 100 times: giving up on the listener\n");
				return;
			}
			IOSleep(100);
			continue;
		}
		errors = 0;

		{
			uint32_t inlen = back[3];

			if (inlen > sizeof(inbufs)) {
				say("vayu-frpc: next2 says %u bytes of input, only %u fit: cut\n", inlen,
				    (uint32_t)sizeof(inbufs));
				inlen = sizeof(inbufs);
			}
			rctx = back[0];
			result = frpc_serve_request(back[1], back[2], inbufs, inlen, frpc_reply, &reply_len);
		}
	}
}

/* --------------------------------------------------------------------------------------- */
/* Thread and API                                                                          */
/* --------------------------------------------------------------------------------------- */

static void
frpc_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t handle = 0;
	unsigned waited;

	frpc_running = true;
	say("vayu-frpc: thread started (vayu_slpi level %u, vayu_frpc_cb %u)\n", frpc_level, frpc_cb);

	if (frpc_leaving) {			/* (the SLPI is already on its way down) */
		goto out;
	}
	/* 1. The sensors SIDs' S1 bypass: without it the DSP's addresses are not physical. */
	frpc_smmu = vayu_smmu_claim(FRPC_SID, FRPC_SID_MASK, true);
	if (frpc_smmu < 0) {
		say("vayu-frpc: the SMMU refused SMR %#x mask %#x: no FastRPC\n", FRPC_SID, FRPC_SID_MASK);
		goto out;
	}
	frpc_claimed = true;
	say("vayu-frpc: SMMU SMR %#x mask %#x is bypassed for the sensors SIDs\n", FRPC_SID, FRPC_SID_MASK);

	/* The SLPI opens the channel by name when its FastRPC service comes up; wait for it. */
	for (waited = 0; waited < 100 && !frpc_leaving; waited++) {
		if (frpc_channel->open) {
			break;
		}
		IOSleep(100);
	}
	if (!frpc_channel->open) {
		say("vayu-frpc: the SLPI never opened \"%s\"; nothing is sent\n", frpc_channel->name);
		goto out;
	}
	say("vayu-frpc: \"%s\" is open\n", frpc_channel->name);

	frpc_pak_load();
	if (!frpc_buffers_alloc()) {
		goto out;
	}
	if (!frpc_attach()) {
		goto out;
	}
	if (!frpc_remotectl_open(&handle)) {
		goto out;
	}
	if (!frpc_register(handle)) {
		goto out;
	}
	if (!frpc_init2()) {
		goto out;
	}
	frpc_last_summary_ns = frpc_now_ns();
	say("vayu-frpc: listener running; serving apps_std for the sensors PD\n");
	frpc_listener_loop();

out:
	/* The packets are NOT freed here: the DSP may still be writing into an outstanding
	 * next2 until the SLPI is stopped. vayu_fastrpc_release() frees them after that. */
	frpc_running = false;
	say("vayu-frpc: thread done; the SMR and the packets stay until the SLPI is stopped%s\n",
	    frpc_claimed ? "" : " (nothing was claimed)");
	thread_terminate(current_thread());
}

void
vayu_fastrpc_start(struct vayu_glink *g, struct vayu_glink_channel *channel, uint32_t level)
{
	uint32_t frpc = 0;
	thread_t thread;

	if (g == NULL || channel == NULL) {
		return;
	}
	if (!PE_parse_boot_argn("vayu_frpc", &frpc, sizeof(frpc)) || frpc < 1) {
		return;
	}
	PE_parse_boot_argn("vayu_frpc_cb", &frpc_cb, sizeof(frpc_cb));
	frpc_glink = g;
	frpc_channel = channel;
	frpc_level = level;
	frpc_mode = frpc;
	frpc_leaving = false;

	if (!frpc_lock_ready) {
		frpc_lock_group = lck_grp_alloc_init("vayu-frpc", LCK_GRP_ATTR_NULL);
		if (frpc_lock_group == NULL) {
			say("vayu-frpc: no lock group; nothing is done\n");
			return;
		}
		lck_spin_init(&frpc_lock, frpc_lock_group, LCK_ATTR_NULL);
		frpc_lock_ready = true;
	}
	lck_spin_lock(&frpc_lock);
	frpc_ring_head = frpc_ring_tail = 0;
	lck_spin_unlock(&frpc_lock);
	say("vayu-frpc: vayu_frpc=%u vayu_frpc_cb=%u\n", frpc, frpc_cb);
	if (kernel_thread_start(frpc_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("vayu-frpc: no thread; nothing is done\n");
	}
}

void
vayu_fastrpc_quiesce(void)
{
	/* No locks, no sleeping: a panic's way back to the loader runs with interrupts off. */
	frpc_leaving = true;
}

void
vayu_fastrpc_release(void)
{
	/* Only after the SLPI has been stopped: while it runs, its DSP may still hold addresses
	 * that go through this bypass and may still write into an outstanding next2's packet. */
	if (frpc_running) {
		for (unsigned i = 0; i < 100 && frpc_running; i++) {
			IOSleep(10);		/* (it exits within one poll after vayu_fastrpc_quiesce) */
		}
	}
	frpc_buffers_free();
	if (!frpc_claimed) {
		return;
	}
	vayu_smmu_release(frpc_smmu);
	frpc_smmu = -1;
	frpc_claimed = false;
	say("vayu-frpc: SMMU SMR %#x mask %#x given back\n", FRPC_SID, FRPC_SID_MASK);
}
