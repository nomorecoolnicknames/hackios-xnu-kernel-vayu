/*
 * A remote processor of the POCO X3 Pro (vayu, SM8150-AC) -- the audio DSP first, the modem and
 * the others in the same way -- loaded through TrustZone and watched over the shared memory. The
 * way is hakios.adsp.c's (steps 1 to 7 and its shutdown, whose comments say why each thing is
 * done so), taken over with vayu's numbers and made to take any processor (sys/vayu_pas.h):
 *
 *   - the shared memory is at 0x86000000, 2 MB (dt: reserved-memory smem@86000000); its
 *     allocation lock is TCSR mutex 3 at 0x01f43000 (dt: hwlock@1f40000, syscon 0x1f40000 with a
 *     stride of 0x1000; qcom,smem@8600000 hwlocks <.. 3>)
 *   - the doorbells to the processors are bits of APCS IPC, one register at 0x17c0000c, written
 *     one bit at a time (dt: mailbox@17c00000 qcom,sm8150-apcs-hmss-global; Linux's
 *     qcom-apcs-ipc-mailbox.c, offset 12): the always-on processor's is bit 0 (dt:
 *     qcom,qmp-aop@c300000, irq-reg-base 0x17c0000c, qcom,irq-mask 1), the audio DSP's GLINK 8 and
 *     SMP2P 10. Their rings to us are GIC interrupts, which nothing here listens to: the shared
 *     memory is looked at instead
 *   - the mailbox to the always-on processor is QMP at 0x0c300000 (the same as redwood's)
 *   - RPMh is asked through the application processors' sequencer at 0x18220000 (dt:
 *     mailbox@18220000 apps_rsc, DRV 2, TCSs at +0xd00, qcom,tcs-config <2 2 ...>: TCSs 0 and 1
 *     are the "active" ones), by positions out of the boot loader's command database at
 *     0x85f20000 (dt: reserved-memory cmd_db; the same as mainline's sm8150.dtsi)
 *   - TrustZone's calls are the same SIP service 2 as on redwood (qcom_scm.c: PAS_INIT_IMAGE 1,
 *     MEM_SETUP 2, AUTH_AND_RESET 5, SHUTDOWN 6)
 *
 * Nothing here runs unless a caller (vayu_audio.c) asks; every write is said before it is done,
 * and where it is done for the first time the loader is given vayu_pas_set_pause() ms to note
 * the log first. What is started is stopped: vayu_pas_stop(), and vayu_pas_quiesce() on a
 * panic's way back to the loader.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/kauth.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/vayu_pas.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */
extern void loader_log_putc(char c);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* (hakios.wifi.c's: whoever opens the mailbox or allocates in the shared memory takes them) */
extern lck_mtx_t hakios_aop_mailbox_lock;
extern lck_mtx_t hakios_smem_alloc_lock;

static LCK_GRP_DECLARE(pas_lock_group, "vayu-pas");
static LCK_MTX_DECLARE(pas_lock, &pas_lock_group);      /* a processor being started or stopped */
static LCK_MTX_DECLARE(rpmh_tcs_lock, &pas_lock_group); /* TCS 0 of the apps RSC: every vayu writer takes it */

static uint32_t pause_ms = 2200;
static bool leaving;                    /* on a panic's way back to the loader: no lock, no sleep, the loader's log only */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving) {
		printf("%s", line);
	}
	return length;
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

static void
spin_us(uint32_t us)
{
	uint64_t until = now_us() + us;

	while (now_us() < until) {
	}
}

static void
sleep_us(uint32_t us)
{
	if (leaving) {
		spin_us(us);
	} else {
		delay_for_interval(us, NSEC_PER_USEC);
	}
}

static void
sleep_ms(uint32_t ms)
{
	sleep_us(ms * 1000);
}

static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, before a write that has not been seen to be harmless. */
static void
hold(void)
{
	if (pause_ms != 0 && !leaving) {
		sleep_ms(pause_ms);
	}
}

void
vayu_pas_set_pause(uint32_t ms)
{
	pause_ms = ms;
}

/* ---- the shared memory ------------------------------------------------------------------------------ */

#define SMEM_PHYS               0x86000000UL
#define SMEM_SIZE               0x00200000u
#define SMEM_CANARY             0xa5a5
#define CMD_DB_PHYS             0x85f20000UL
#define CMD_DB_SIZE             0x20000UL

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};

static const volatile uint8_t *smem;

bool
vayu_smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (smem == NULL || offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

uint32_t
vayu_smem_word(uint32_t offset)
{
	uint32_t value = 0;

	vayu_smem_peek(offset, &value, sizeof(value));
	return value;
}

void
vayu_smem_poke(uint32_t offset, uint32_t value)
{
	if (smem != NULL && offset + 4 <= SMEM_SIZE) {
		wr((vm_offset_t)smem + offset, value);
	}
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!vayu_smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 ||
	    table.version != 1 || table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!vayu_smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) ||
		    entry.offset == 0 || entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !vayu_smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		return entry.offset;
	}
	return 0;
}

/* Where an item is in one partition (uncached items from the front, cached ones from the back); 0: not there. */
static uint32_t
smem_find(uint32_t partition, uint32_t cacheline, uint32_t item, uint32_t *size)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;

	if (partition == 0 || !vayu_smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!vayu_smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (entry.item == item) {
			*size = entry.size - entry.padding_data;
			return partition + at + sizeof(entry) + entry.padding_header;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return 0;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!vayu_smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (entry.item == item) {
			*size = entry.size - entry.padding_data;
			return partition + at - entry.size;
		}
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return 0;
}

/*
 * What an item says, for the census: its first printable run of 8 or more characters (at most 72),
 * with any run of 14 or more digits masked, so that an identity number never reaches the log.
 */
static void
census_text(uint32_t at, uint32_t size, char *text, size_t text_size)
{
	uint8_t bytes[256];
	uint32_t length = size < sizeof(bytes) ? size : (uint32_t)sizeof(bytes), start = 0, run = 0, digits = 0;

	text[0] = 0;
	if (!vayu_smem_peek(at, bytes, length)) {
		return;
	}
	for (uint32_t i = 0; i <= length; i++) {
		bool printable = i < length && bytes[i] >= 0x20 && bytes[i] < 0x7f;

		if (printable) {
			if (run++ == 0) {
				start = i;
			}
			continue;
		}
		if (run >= 8) {
			size_t n = run < text_size - 1 ? run : text_size - 1;

			n = n < 72 ? n : 72;
			memcpy(text, bytes + start, n);
			text[n] = 0;
			for (size_t k = 0; k <= n; k++) {
				if (k < n && text[k] >= '0' && text[k] <= '9') {
					digits++;
					continue;
				}
				if (digits >= 14) {
					memset(text + k - digits, '#', digits);
				}
				digits = 0;
			}
			return;
		}
		run = 0;
	}
}

/* Items 600-605 (run 033841: 602 held "LBNETRTSSEY"): their first 128 bytes in hex, 32 a line. */
static void
census_hex(const char *who, uint32_t item, uint32_t at, uint32_t size)
{
	uint8_t bytes[128];
	uint32_t length = size < sizeof(bytes) ? size : (uint32_t)sizeof(bytes);
	char hex[3 * 32 + 1];

	if (!vayu_smem_peek(at, bytes, length)) {
		return;
	}
	for (uint32_t line = 0; line < length; line += 32) {
		uint32_t n = length - line < 32 ? length - line : 32;

		for (uint32_t i = 0; i < n; i++) {
			snprintf(hex + 3 * i, 4, " %02x", bytes[line + i]);
		}
		hex[3 * n] = 0;
		say("%s: census: item %u +%03x:%s\n", who, item, line, hex);
	}
}

/*
 * The first 16 bytes of an item in hex, for the all-partitions census (vayu_smem_census_all). An identity number may
 * be stored as BCD: any run of 7 or more bytes whose two nibbles are both decimal digits is printed as "##".
 */
static void
census_first(uint32_t at, uint32_t size, char *hex, size_t hex_size)
{
	uint8_t bytes[16];
	uint32_t length = size < sizeof(bytes) ? size : (uint32_t)sizeof(bytes);
	bool masked[16] = { false };

	hex[0] = 0;
	if (length == 0 || hex_size < 3 * sizeof(bytes) + 1 || !vayu_smem_peek(at, bytes, length)) {
		return;
	}
	for (uint32_t i = 0, run = 0; i <= length; i++) {
		if (i < length && (bytes[i] >> 4) <= 9 && (bytes[i] & 0xf) <= 9) {
			run++;
			continue;
		}
		for (uint32_t k = i - run; run >= 7 && k < i; k++) {
			masked[k] = true;
		}
		run = 0;
	}
	for (uint32_t i = 0; i < length; i++) {
		if (masked[i]) {
			snprintf(hex + 3 * i, 4, " ##");
		} else {
			snprintf(hex + 3 * i, 4, " %02x", bytes[i]);
		}
	}
}

static bool census_with_bytes;                  /* set by vayu_smem_census_all(): each line also has the first bytes */

/* Every item of one partition (both of its lists), with its size and any text in it. Reads only. */
static unsigned
census_partition(const char *who, const char *label, uint32_t partition, uint32_t cacheline, unsigned budget)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned said = 0;
	char text[80], first[3 * 16 + 1];

	if (partition == 0 || !vayu_smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		say("%s: census: %s: no such partition\n", who, label);
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached && said < budget;) {
		if (!vayu_smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		census_text(partition + at + sizeof(entry) + entry.padding_header, entry.size - entry.padding_data, text, sizeof(text));
		first[0] = 0;
		if (census_with_bytes) {
			census_first(partition + at + sizeof(entry) + entry.padding_header, entry.size - entry.padding_data, first,
			    sizeof(first));
		}
		say("%s: census: %s: item %u, %u bytes%s%s%s%s%s%s\n", who, label, entry.item, entry.size - entry.padding_data,
		    first[0] ? " [" : "", first, first[0] ? " ]" : "", text[0] ? ": \"" : "", text, text[0] ? "\"" : "");
		if (entry.item >= 600 && entry.item <= 605) {
			census_hex(who, entry.item, partition + at + sizeof(entry) + entry.padding_header, entry.size - entry.padding_data);
		}
		said++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return said;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size && said < budget;) {
		if (!vayu_smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		census_text(partition + at - entry.size, entry.size - entry.padding_data, text, sizeof(text));
		first[0] = 0;
		if (census_with_bytes) {
			census_first(partition + at - entry.size, entry.size - entry.padding_data, first, sizeof(first));
		}
		say("%s: census: %s: cached item %u, %u bytes%s%s%s%s%s%s\n", who, label, entry.item, entry.size - entry.padding_data,
		    first[0] ? " [" : "", first, first[0] ? " ]" : "", text[0] ? ": \"" : "", text, text[0] ? "\"" : "");
		said++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return said;
}

/*
 * The shared memory's items, after a processor has said it has given up and its own item (the crash
 * reason) is not where Linux looks: the partition of us and that host, and the global one. At most
 * 96 lines. Reads only.
 */
void
vayu_smem_census(const char *who, uint16_t host)
{
	uint32_t cacheline = 0, partition;
	unsigned said;
	char label[24];

	snprintf(label, sizeof(label), "hosts 0+%u", host);
	partition = smem_partition(VAYU_SMEM_HOST_APPS, host, &cacheline);
	said = census_partition(who, label, partition, cacheline, 64);
	cacheline = 0;
	partition = smem_partition(VAYU_SMEM_HOST_GLOBAL, VAYU_SMEM_HOST_GLOBAL, &cacheline);
	said += census_partition(who, "global", partition, cacheline, 96 - said);
	say("%s: census: %u items listed\n", who, said);
}

/*
 * After a processor said FATAL with no crash text where Linux looks: the whole table of contents (read from its own
 * page only), then the items of every partition that the application processors may read -- one of its hosts is
 * us (0), or it is the global one. A partition of two other hosts is NOT read: run 061253 (M1) hung core 7 at the
 * first one (its header read: the memory is most likely access-protected for us, and the bus does not answer),
 * which is also why Linux never looks there. Each item: size, first 16 bytes (BCD-looking runs masked), text
 * (digit runs masked); at most 256 item lines. Reads only.
 */
void
vayu_smem_census_all(const char *who)
{
	struct smem_table table;
	struct smem_table_entry entry;
	unsigned said = 0, partitions = 0;
	char label[32];

	if (!vayu_smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 ||
	    table.version != 1 || table.count > 64) {
		say("%s: census: no table of contents\n", who);
		return;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (vayu_smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) && entry.size != 0) {
			say("%s: census: toc %u: hosts %u+%u at %#x, %u bytes, flags %#x, cacheline %u\n", who, i, entry.host0, entry.host1,
			    entry.offset, entry.size, entry.flags, entry.cacheline);
		}
	}
	census_with_bytes = true;
	for (uint32_t i = 0; i < table.count && said < 256; i++) {
		struct smem_partition header;
		bool ours;

		if (!vayu_smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) ||
		    entry.offset == 0 || entry.size == 0) {
			continue;
		}
		ours = entry.host0 == VAYU_SMEM_HOST_APPS || entry.host1 == VAYU_SMEM_HOST_APPS ||
		    (entry.host0 == VAYU_SMEM_HOST_GLOBAL && entry.host1 == VAYU_SMEM_HOST_GLOBAL);
		if (!ours) {
			continue;                       /* (two other hosts': listed above, not read) */
		}
		snprintf(label, sizeof(label), "toc %u hosts %u+%u", i, entry.host0, entry.host1);
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !vayu_smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			say("%s: census: %s: no partition header there\n", who, label);
			continue;
		}
		say("%s: census: %s: %u used from the front, %u from the back\n", who, label, header.free_uncached,
		    header.size - header.free_cached);
		partitions++;
		said += census_partition(who, label, entry.offset, entry.cacheline, 256 - said);
	}
	census_with_bytes = false;
	say("%s: census: %u partitions of ours read, %u items listed%s\n", who, partitions, said, said >= 256 ? " (stopped at 256)" : "");
}

/* An item we share with that host: in the partition of the two, or else in the global one. */
uint32_t
vayu_smem_item(uint16_t host, uint32_t item, uint32_t *size)
{
	const uint16_t hosts[2][2] = { { VAYU_SMEM_HOST_APPS, host }, { VAYU_SMEM_HOST_GLOBAL, VAYU_SMEM_HOST_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline);
		uint32_t at = smem_find(partition, cacheline, item, size);

		if (at != 0) {
			return at;
		}
	}
	return 0;
}

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static bool
cmd_db_there(void)
{
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	return cmd_db != NULL && cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 &&
	       cmd_db->magic[3] == 0x0c;
}

bool
vayu_soc_check(const char *who)
{
	struct smem_table table;
	bool smem_ok = vayu_smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) && memcmp(table.magic, "$TOC", 4) == 0;
	bool db_ok = cmd_db_there();

	say("%s: the shared memory's table at %#lx: %s; RPMh's command database at %#lx: %s\n", who, SMEM_PHYS + SMEM_SIZE - 0x1000,
	    smem_ok ? "there" : "NOT THERE", CMD_DB_PHYS, db_ok ? "there" : "NOT THERE");
	return smem_ok && db_ok;
}

/* ---- doorbells, the always-on processor's mailbox, RPMh -------------------------------------------------- */

static struct block apcs = { "APCS IPC", 0x17c0000c, 4 };

void
vayu_apcs_ring(unsigned bit)
{
	need(&apcs);
	wr(apcs.base, 1u << bit);
}

#define APCS_AOP                0

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "AOP message RAM", 0x0c300000, 0x1000 };
static uint32_t our_box;

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

/* Linux's qmp_open() (qcom_aoss.c), as hakios.adsp.c does it; what is done already is found done. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	if (rd(aoss.base + QMP_MAGIC) != QMP_MAGIC_VALUE || rd(aoss.base + QMP_VERSION) != 1) {
		say("aop: not a mailbox this knows (magic %#x, version %u)\n", rd(aoss.base + QMP_MAGIC), rd(aoss.base + QMP_VERSION));
		return false;
	}
	box = rd(aoss.base + QMP_OUR_BOX);
	size = rd(aoss.base + QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		say("aop: our mailbox (%#x, %#x bytes) is not where a message fits\n", box, size);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		say("aop: the mailbox is open already\n");
		our_box = box;
		return true;
	}
	say("aop: about to open the mailbox at %#x: our link and channel up, doorbell APCS bit %u\n", aoss.phys, APCS_AOP);
	hold();
	wr(aoss.base + QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	wr(aoss.base + QMP_OUR_LINK, QMP_UP);
	vayu_apcs_ring(APCS_AOP);
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		say("aop: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n");
		wr(aoss.base + QMP_OUR_LINK, QMP_DOWN);
		vayu_apcs_ring(APCS_AOP);
		return false;
	}
	wr(aoss.base + QMP_OUR_CHANNEL, QMP_UP);
	vayu_apcs_ring(APCS_AOP);
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		say("aop: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n");
		wr(aoss.base + QMP_OUR_CHANNEL, QMP_DOWN);
		wr(aoss.base + QMP_OUR_LINK, QMP_DOWN);
		vayu_apcs_ring(APCS_AOP);
		return false;
	}
	wr(aoss.base + QMP_THEIR_CHANNEL_ACK, QMP_UP);
	vayu_apcs_ring(APCS_AOP);
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		say("aop: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n");
		wr(aoss.base + QMP_OUR_CHANNEL, QMP_DOWN);
		wr(aoss.base + QMP_OUR_LINK, QMP_DOWN);
		vayu_apcs_ring(APCS_AOP);
		return false;
	}
	say("aop: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n");
	our_box = box;
	return true;
}

/* Linux's qmp_send(). */
bool
vayu_aop_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&hakios_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	say("aop: about to tell the always-on processor \"%s\"\n", text);
	if (rd(aoss.base + our_box) != 0) {
		say("aop: our mailbox still holds a message of %u bytes: not sent\n", rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	vayu_apcs_ring(APCS_AOP);
	if (!mailbox_wait(our_box, 0, 1000)) {
		say("aop: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n");
		wr(aoss.base + our_box, 0);
		goto out;
	}
	say("aop: taken\n");
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&hakios_aop_mailbox_lock);
	}
	return sent;
}

static struct block rsc = { "apps RSC", 0x18220000, 0x10000 };
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    /* not zero: idle */
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)
#define RPMH_TCS                0

/* The level position RPMh takes for that resource and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (!cmd_db_there()) {
		say("rpmh: the boot loader's command database is not at %#lx\n", CMD_DB_PHYS);
		return -1;
	}
	memcpy(id, name, strnlen(name, sizeof(id)));   /* (a cmd-db id fills all 8 bytes with no NUL: "lnbclka2"; strlcpy kept 7) */
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			say("rpmh: %s\n", levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	say("rpmh: no \"%s\"\n", name);
	return -1;
}

/* The address of that resource in the command database; 0: not there. No levels are read. */
uint32_t
vayu_rpmh_address(const char *name)
{
	char id[8] = { 0 };

	if (!cmd_db_there()) {
		say("rpmh: the boot loader's command database is not at %#lx\n", CMD_DB_PHYS);
		return 0;
	}
	memcpy(id, name, strnlen(name, sizeof(id)));   /* (a cmd-db id fills all 8 bytes with no NUL: "lnbclka2"; strlcpy kept 7) */
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				say("rpmh: no \"%s\"\n", name);
				return 0;
			}
			say("rpmh: %s at %#x\n", name, entry[i].address);
			return entry[i].address;
		}
	}
	say("rpmh: no \"%s\"\n", name);
	return 0;
}

/*
 * TCS 0 of the apps RSC is the one command slot set every vayu driver writes (this file, vayu_cpufreq.c's
 * bandwidth vote, vayu_qup_spi.c's QUP vote): one request at a time, through this lock. On the way out
 * (leaving, or interrupts off: the other processors are already stopped) there is nobody to wait for and
 * a mutex cannot be taken, so it is not. Returns whether it was taken, for vayu_rpmh_tcs_unlock().
 */
bool
vayu_rpmh_tcs_lock(void)
{
	if (leaving || !ml_get_interrupts_enabled()) {
		return false;
	}
	lck_mtx_lock(&rpmh_tcs_lock);
	return true;
}

void
vayu_rpmh_tcs_unlock(bool held)
{
	if (held) {
		lck_mtx_unlock(&rpmh_tcs_lock);
	}
}

/* Raw writes of those values to those addresses through TCS 0, waited for; true: RPMh has said done. */
bool
vayu_rpmh_send(const uint32_t addresses[], const uint32_t values[], unsigned count, const char *who)
{
	char line[400];
	uint32_t done = 0, mask;
	uint64_t began;
	int at;
	bool held;

	if (count == 0 || count > 4) {
		say("rpmh: %s asks for %u commands: one to four at a time: not asked\n", who, count);
		return false;
	}
	for (unsigned r = 0; r < count; r++) {
		if (addresses[r] == 0) {
			say("rpmh: %s: the address of command %u is 0: not asked\n", who, r);
			return false;
		}
	}
	need(&rsc);
	hold();                                 /* (the step pause, if any, before the lock: other voters do not wait it out) */
	held = vayu_rpmh_tcs_lock();
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		say("rpmh: TCS %u is busy (status %#x, control %#x): not asked\n", RPMH_TCS, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS),
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		vayu_rpmh_tcs_unlock(held);
		return false;
	}
	at = snprintf(line, sizeof(line), "rpmh: about to ask, through TCS %u of %#x, %s for", RPMH_TCS, rsc.phys, who);
	for (unsigned r = 0; r < count; r++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, "%s %#x = %u", r == 0 ? "" : r + 1 == count ? " and" : ",", addresses[r], values[r]);
	}
	say("%s\n", line);
	mask = (1u << count) - 1;
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	for (unsigned r = 0; r < count; r++) {
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 4, addresses[r]);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, mask);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, mask);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	at = snprintf(line, sizeof(line), "rpmh: %s after %llu us; the commands' status", done ? "DONE" : "HAS NOT SAID DONE",
	    now_us() - began);
	for (unsigned r = 0; r < count; r++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 12));
	}
	say("%s\n", line);
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	vayu_rpmh_tcs_unlock(held);
	return done != 0;
}

/* Those resources at that level (Linux's proxy vote), or given back (level 0). True: RPMh has said done. */
bool
vayu_rpmh_vote(const char *const names[], unsigned count, uint32_t level)
{
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 };

	if (count == 0 || count > 2) {
		return count == 0;
	}
	for (unsigned r = 0; r < count; r++) {
		int position = rail_position(names[r], level, &addresses[r]);

		if (position < 0 || addresses[r] == 0) {
			say("rpmh: no position for %s at level %u: not asked\n", names[r], level);
			return false;
		}
		values[r] = level == 0 ? 0 : (uint32_t)position;
	}
	return vayu_rpmh_send(addresses, values, count, "rail");
}

static unsigned
rail_count(const struct vayu_rproc *rp)
{
	return rp->rails[0] == NULL ? 0 : rp->rails[1] == NULL ? 1 : 2;
}

/* Our vote ends -- except on a shared rail (cx.lvl): writing it down would take away what UEFI, the display and UFS stand on. */
static void
rails_give_back(struct vayu_rproc *rp)
{
	if (!rp->rails_asked) {
		return;
	}
	if (rp->rails_shared) {
		say("%s: %s stays at level %u: a shared rail, never voted down from here\n", rp->name, rp->rails[0], rp->rail_level);
	} else {
		vayu_rpmh_vote(rp->rails, rail_count(rp), 0);
	}
	rp->rails_asked = false;
}

/* ---- SMP2P --------------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))
#define OUR_ENTRY               "master-kernel"         /* dt: qcom,smem-states ... "qcom,force-stop" */
#define OUR_STOP                (1u << 0)

static bool
their_words(const struct vayu_rproc *rp, struct smp2p *words)
{
	uint32_t size = 0, at = vayu_smem_item(rp->smem_host, rp->smp2p_theirs, &size);

	return at != 0 && size >= sizeof(*words) && vayu_smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
slave_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

uint32_t
vayu_pas_state(struct vayu_rproc *rp)
{
	struct smp2p words;

	return their_words(rp, &words) ? slave_state(&words) : 0;
}

static void
show_words(const struct vayu_rproc *rp, const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	say("%s: %s: SMP2P version %u, features %#x, %u to %u, flags %#x, %u of %u entries:%s\n", rp->name, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(const struct vayu_rproc *rp)
{
	char text[81];
	uint32_t size = 0, at = vayu_smem_item(rp->smem_host, rp->crash_item, &size);

	if (at == 0) {
		say("%s: item %u, its last words: ABSENT (no such item in partition 0+%u or the global one)\n", rp->name,
		    rp->crash_item, rp->smem_host);
		return;
	}
	if (size == 0) {
		say("%s: item %u, its last words: PRESENT BUT EMPTY (0 bytes, at %#lx)\n", rp->name, rp->crash_item, SMEM_PHYS + at);
		return;
	}
	bzero(text, sizeof(text));
	vayu_smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		uint32_t nonzero = 0;
		uint8_t byte;

		for (uint32_t i = 0; i < size && i < 4096; i++) {
			nonzero += vayu_smem_peek(at + i, &byte, 1) && byte != 0;
		}
		say("%s: item %u, its last words: PRESENT, %u bytes at %#lx, NO TEXT (first byte 0; %u nonzero bytes in the first %u)\n",
		    rp->name, rp->crash_item, size, SMEM_PHYS + at, nonzero, size < 4096 ? size : 4096);
		return;
	}
	say("%s: item %u, its last words (%u bytes): %s%s%s\n", rp->name, rp->crash_item, size, text[0] ? "\"" : "none",
	    text, text[0] ? "\"" : "");
}

void
vayu_pas_survey(struct vayu_rproc *rp)
{
	struct smp2p words;
	uint32_t size = 0, cacheline = 0;

	say("%s: SMEM partition of hosts 0 and %u: %s\n", rp->name, rp->smem_host,
	    smem_partition(VAYU_SMEM_HOST_APPS, rp->smem_host, &cacheline) ? "there" : "NOT THERE");
	if (their_words(rp, &words)) {
		show_words(rp, "its SMP2P item is THERE: it has run since the phone was started", &words);
	} else {
		say("%s: its SMP2P item (%u) is not there: it has not run since the phone was started\n", rp->name, rp->smp2p_theirs);
	}
	say("%s: our SMP2P item (%u): %s; GLINK's descriptor (478): %s, our queue (479): %s, its queue (480): %s\n", rp->name,
	    rp->smp2p_ours, vayu_smem_item(rp->smem_host, rp->smp2p_ours, &size) ? "there" : "not there",
	    vayu_smem_item(rp->smem_host, 478, &size) ? "there" : "NOT THERE", vayu_smem_item(rp->smem_host, 479, &size) ? "there" :
	    "NOT THERE", vayu_smem_item(rp->smem_host, 480, &size) ? "there" : "not there (it makes it)");
	show_crash_reason(rp);
}

/* What Linux does when it rings for SMP2P: agree on the features once, and answer a restart. */
static void
smp2p_listen(struct vayu_rproc *rp, const struct smp2p *theirs)
{
	struct smp2p ours;

	if (rp->our_item == 0 || !vayu_smem_peek(rp->our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!rp->smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			vayu_smem_poke(rp->our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		rp->smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		rp->smp2p_agreed = true;
		say("%s: SMP2P agreed: version %u, features %#x\n", rp->name, ours.version & 0xff, features);
	}
	if (rp->smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != rp->smp2p_restart_acked) {
		rp->smp2p_restart_acked = !rp->smp2p_restart_acked;
		say("%s: its flags say it has restarted (%#x): answered (bit 1 = %u)\n", rp->name, theirs->flags,
		    rp->smp2p_restart_acked);
		vayu_smem_poke(rp->our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) |
		    (rp->smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		vayu_apcs_ring(rp->apcs_smp2p);
	}
}

static uint32_t
our_item_find(const struct vayu_rproc *rp)
{
	uint32_t size = 0, at = vayu_smem_item(rp->smem_host, rp->smp2p_ours, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

static struct block smem_lock = { "TCSR mutex 3", 0x01f43000, 0x1000 };
#define LOCK_US                 1u

/*
 * A new item in our partition with that host, as Linux's qcom_smem_alloc_private() makes one
 * (hakios.adsp.c: our_item_make()), its bytes zeroed; an item already there is kept as it is.
 * Returns its offset; 0: not made.
 */
uint32_t
vayu_smem_alloc(uint16_t host, uint32_t item, uint32_t size, const char *who)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (size + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at, cacheline = 0, partition, found_size = 0, made = 0;

	if ((made = vayu_smem_item(host, item, &found_size)) != 0) {
		say("%s: item %u is there already (%u bytes at %#lx): kept\n", who, item, found_size, SMEM_PHYS + made);
		return found_size >= size ? made : 0;
	}
	partition = smem_partition(VAYU_SMEM_HOST_APPS, host, &cacheline);
	if (partition == 0 || size == 0 || size > 0x10000) {
		say("%s: item %u: no partition for hosts 0 and %u (or a size of %u)\n", who, item, host, size);
		return 0;
	}
	lck_mtx_lock(&hakios_smem_alloc_lock);
	need(&smem_lock);
	say("%s: about to take the lock (%#x), add item %u (%u bytes) at the partition's mark, and release the lock\n", who,
	    smem_lock.phys, item, item_size);
	hold();
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base, LOCK_US);
		owner = rd(smem_lock.base);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		say("%s: THE LOCK IS HELD BY PROCESSOR %u FOR A SECOND: nothing is written\n", who, owner);
		lck_mtx_unlock(&hakios_smem_alloc_lock);
		return 0;
	}
	if (!vayu_smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		say("%s: the partition's marks make no sense: nothing is written\n", who);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!vayu_smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached || at + need_bytes > header.free_cached) {
		say("%s: the entries end at +%#x, the mark is +%#x, free to +%#x: nothing is written\n", who, at,
		    header.free_uncached, header.free_cached);
		goto out;
	}
	for (uint32_t i = 0; i < item_size; i += 4) {
		vayu_smem_poke(partition + at + sizeof(entry) + i, 0);
	}
	vayu_smem_poke(partition + at, SMEM_CANARY | item << 16);
	vayu_smem_poke(partition + at + 4, item_size);
	vayu_smem_poke(partition + at + 8, item_size - size);
	vayu_smem_poke(partition + at + 12, 0);
	settle();
	vayu_smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = partition + at + sizeof(entry);
out:
	wr(smem_lock.base, 0);
	say("%s: the lock is released (it reads %u)%s\n", who, rd(smem_lock.base), made ? "; the item is made" : "");
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return made;
}

static bool
our_item_fill(struct vayu_rproc *rp)
{
	struct smp2p ours;
	uint32_t cacheline = 0, partition, name[4] = { 0 }, extra;

	partition = smem_partition(VAYU_SMEM_HOST_APPS, rp->smem_host, &cacheline);
	if (partition == 0) {
		say("%s: there is no partition for hosts 0 and %u\n", rp->name, rp->smem_host);
		return false;
	}
	rp->our_item = our_item_find(rp);
	if (rp->our_item == 0) {
		if (vayu_smem_alloc(rp->smem_host, rp->smp2p_ours, sizeof(struct smp2p), rp->name) == 0 ||
		    (rp->our_item = our_item_find(rp)) == 0) {
			say("%s: no SMP2P item of ours\n", rp->name);
			return false;
		}
		say("%s: item %u made, at %#lx\n", rp->name, rp->smp2p_ours, SMEM_PHYS + rp->our_item);
	} else {
		say("%s: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", rp->name,
		    rp->smp2p_ours, SMEM_PHYS + rp->our_item);
	}
	/* zeroes; from us to it, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		vayu_smem_poke(rp->our_item + at, 0);
	}
	settle();
	vayu_smem_poke(rp->our_item, SMP2P_MAGIC);
	vayu_smem_poke(rp->our_item + SMP2P_AT_PROCESSORS, VAYU_SMEM_HOST_APPS | (uint32_t)rp->smem_host << 16);
	strlcpy((char *)name, OUR_ENTRY, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		vayu_smem_poke(rp->our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	/*
	 * vayu_smp2p_extra=1: the device tree's second outgoing entry for this processor (the modem: "ipa", dt
	 * qcom,smp2p-ipa-1-out of qcom,smp2p-modem@1799000c) at index 1, value 0. Off by default: Linux's IPA driver
	 * makes it, which we do not run (critique 2026-10-25 W2 #5: a low-priority HYPOTHESIS for the modem's FATAL).
	 */
	extra = 0;
	if (rp->smp2p_extra != NULL && PE_parse_boot_argn("vayu_smp2p_extra", &extra, sizeof(extra)) && extra != 0) {
		bzero(name, sizeof(name));
		strlcpy((char *)name, rp->smp2p_extra, sizeof(name));
		for (uint32_t word = 0; word < 4; word++) {
			vayu_smem_poke(rp->our_item + SMP2P_AT_NAME(1) + 4 * word, name[word]);
		}
		vayu_smem_poke(rp->our_item + SMP2P_AT_VALUE(1), 0);
		say("%s: our item also has entry 1 \"%s\" = 0 (vayu_smp2p_extra=1)\n", rp->name, rp->smp2p_extra);
	}
	vayu_smem_poke(rp->our_item + SMP2P_AT_COUNTS, 16 | (extra != 0 && rp->smp2p_extra != NULL ? 2u : 1u) << 16);
	vayu_smem_poke(rp->our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	vayu_smem_poke(rp->our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	rp->smp2p_agreed = rp->smp2p_restart_acks = rp->smp2p_restart_acked = false;
	vayu_apcs_ring(rp->apcs_smp2p);
	if (!vayu_smem_peek(rp->our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY) < 0) {
		say("%s: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", rp->name);
		return false;
	}
	show_words(rp, "our item reads back", &ours);
	return true;
}

/* ---- apps SMMU: a stream let through untranslated (bypass), for a driver whose DSP does DMA ---- */
extern lck_mtx_t hakios_smmu_lock;     /* the apps SMMU's match registers: audio, camera, Wi-Fi, GPU, QUP take it too */

int  vayu_smmu_claim(uint16_t sid, uint16_t mask, bool bypass);   /* returns a handle >= 0, or -1 */
void vayu_smmu_release(int handle);
void vayu_smmu_quiesce(void);

#define VSMMU_PHYS              0x15000000UL    /* dt: apps-smmu@15000000 */
#define VSMMU_SIZE              0x2000
#define VSMMU_SCR0              0x000
#define VSMMU_IDR0              0x020
#define VSMMU_SMR(n)            (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define VSMMU_S2CR(n)           (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */

#define VSMMU_CLAIMS            4

static vm_offset_t smmu_base;
static struct {
	bool used;
	uint32_t n;
	uint32_t old_smr, old_s2cr;
	uint16_t sid, mask;
} smmu_claims[VSMMU_CLAIMS];

static bool
smmu_map(void)
{
	if (smmu_base == 0) {
		smmu_base = ml_io_map(VSMMU_PHYS, VSMMU_SIZE);
	}
	return smmu_base != 0;
}

static void smmu_quiet_log(const char *format, ...) __printflike(1, 2);

static void
smmu_quiet_log(const char *format, ...)
{
	char line[160];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/*
 * A stream of the apps SMMU let through untranslated: a DSP's DMA reaches the memory its driver
 * prepared without a page table of ours.  The match register and its type are saved here, and given
 * back by vayu_smmu_release() or, on a panic's way to the loader, vayu_smmu_quiesce().  The lock is
 * the one every other user of these registers takes (vayu_audio.c:791).
 */
int
vayu_smmu_claim(uint16_t sid, uint16_t mask, bool bypass)
{
	uint32_t scr0, idr0, free_n = ~0u, written_smr, written_s2cr, smr, s2cr, now_smr, now_s2cr;
	int slot;

	if (!bypass) {
		say("vayu-pas: smmu: only bypass is supported: stream %#x mask %#x not let through\n", (unsigned)sid,
		    (unsigned)mask);
		return -1;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_map()) {
		say("vayu-pas: smmu: the apps SMMU at %#lx cannot be mapped\n", (unsigned long)VSMMU_PHYS);
		lck_mtx_unlock(&hakios_smmu_lock);
		return -1;
	}
	scr0 = rd(smmu_base + VSMMU_SCR0);
	idr0 = rd(smmu_base + VSMMU_IDR0);
	if (scr0 & (1u << 3)) {
		say("vayu-pas: smmu: 16-bit stream numbers (EXIDS), which nothing here is written for\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return -1;
	}
	for (uint32_t n = 0; n < (idr0 & 0xff) && VSMMU_SMR(n) < 0xc00; n++) {
		uint32_t stream, smr_mask;

		smr = rd(smmu_base + VSMMU_SMR(n));
		s2cr = rd(smmu_base + VSMMU_S2CR(n));
		if ((smr & (1u << 31)) == 0) {
			if (free_n == ~0u) {
				free_n = n;
			}
			continue;
		}
		stream = smr & 0x7fff;
		smr_mask = smr >> 16 & 0x7fff;
		if (((stream ^ sid) & ~(smr_mask | mask) & 0x7fff) == 0) {
			say("vayu-pas: smmu: stream %#x mask %#x already matched by someone else: match register %u, stream %#x "
			    "mask %#x, s2cr %#x\n", (unsigned)sid, (unsigned)mask, n, stream, smr_mask, s2cr);
			lck_mtx_unlock(&hakios_smmu_lock);
			return -1;
		}
	}
	if (free_n == ~0u) {
		say("vayu-pas: smmu: no match register free for stream %#x mask %#x\n", (unsigned)sid, (unsigned)mask);
		lck_mtx_unlock(&hakios_smmu_lock);
		return -1;
	}
	for (slot = 0; slot < VSMMU_CLAIMS; slot++) {
		if (!smmu_claims[slot].used) {
			break;
		}
	}
	if (slot == VSMMU_CLAIMS) {
		say("vayu-pas: smmu: all %u claims are used: stream %#x mask %#x not let through\n", (unsigned)VSMMU_CLAIMS,
		    (unsigned)sid, (unsigned)mask);
		lck_mtx_unlock(&hakios_smmu_lock);
		return -1;
	}
	smr = rd(smmu_base + VSMMU_SMR(free_n));
	s2cr = rd(smmu_base + VSMMU_S2CR(free_n));
	written_s2cr = 1u << 16;
	written_smr = (1u << 31) | ((uint32_t)(mask & 0x7fff) << 16) | (sid & 0x7fff);
	wr(smmu_base + VSMMU_S2CR(free_n), written_s2cr);
	settle();
	wr(smmu_base + VSMMU_SMR(free_n), written_smr);
	settle();
	now_smr = rd(smmu_base + VSMMU_SMR(free_n));
	now_s2cr = rd(smmu_base + VSMMU_S2CR(free_n));
	if (now_smr != written_smr || now_s2cr != written_s2cr) {
		say("vayu-pas: smmu: match register %u does not read back as written: smr %#x (wrote %#x), s2cr %#x (wrote "
		    "%#x); the old values are given back\n", free_n, now_smr, written_smr, now_s2cr, written_s2cr);
		wr(smmu_base + VSMMU_SMR(free_n), smr);
		settle();
		wr(smmu_base + VSMMU_S2CR(free_n), s2cr);
		settle();
		lck_mtx_unlock(&hakios_smmu_lock);
		return -1;
	}
	smmu_claims[slot].used = true;
	smmu_claims[slot].n = free_n;
	smmu_claims[slot].old_smr = smr;
	smmu_claims[slot].old_s2cr = s2cr;
	smmu_claims[slot].sid = sid;
	smmu_claims[slot].mask = mask;
	say("vayu-pas: smmu: stream %#x mask %#x let through (bypass) on match register %u (was smr %#x s2cr %#x)\n",
	    (unsigned)sid, (unsigned)mask, free_n, smr, s2cr);
	lck_mtx_unlock(&hakios_smmu_lock);
	return slot;
}

void
vayu_smmu_release(int handle)
{
	uint32_t n, smr, s2cr;

	if (handle < 0 || handle >= VSMMU_CLAIMS || !smmu_claims[handle].used) {
		say("vayu-pas: smmu: no claim %d to give back\n", handle);
		return;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	n = smmu_claims[handle].n;
	wr(smmu_base + VSMMU_SMR(n), smmu_claims[handle].old_smr);
	settle();
	wr(smmu_base + VSMMU_S2CR(n), smmu_claims[handle].old_s2cr);
	settle();
	smr = rd(smmu_base + VSMMU_SMR(n));
	s2cr = rd(smmu_base + VSMMU_S2CR(n));
	say("vayu-pas: smmu: match register %u given back (smr %#x s2cr %#x)\n", n, smr, s2cr);
	smmu_claims[handle].used = false;
	lck_mtx_unlock(&hakios_smmu_lock);
}

/*
 * On the way back to the loader, after the DSP has been stopped: no lock, no sleep, no printf.
 * The only output allowed here is loader_log_putc, through smmu_quiet_log().
 */
void
vayu_smmu_quiesce(void)
{
	if (smmu_base == 0) {
		return;
	}
	for (int i = 0; i < VSMMU_CLAIMS; i++) {
		uint32_t n, smr, s2cr;

		if (!smmu_claims[i].used) {
			continue;
		}
		n = smmu_claims[i].n;
		wr(smmu_base + VSMMU_SMR(n), smmu_claims[i].old_smr);
		settle();
		wr(smmu_base + VSMMU_S2CR(n), smmu_claims[i].old_s2cr);
		settle();
		smr = rd(smmu_base + VSMMU_SMR(n));
		s2cr = rd(smmu_base + VSMMU_S2CR(n));
		smmu_quiet_log("vayu-pas: smmu: match register %u given back (smr %#x s2cr %#x)\n", n, smr, s2cr);
		smmu_claims[i].used = false;
	}
}

/* ---- TrustZone ------------------------------------------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)

static uint64_t scm_convention = SCM_64;        /* SMC64, as Linux finds first; 0: the 32-bit calls (vayu_pas_supported()) */

static int32_t
scm(const char *who, const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	uint64_t session = 0;

	function |= scm_convention | SCM_SIP;
	say("%s: about to ask TrustZone %s: smc %#llx (%#llx, %#llx, %#llx)\n", who, what, function, first, second, third);
	hold();
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			say("%s: TrustZone returns %d, answer %#llx\n", who, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	say("%s: TrustZone keeps saying the call was interrupted\n", who);
	return -1;
}

/*
 * TrustZone's MEM_PROT_ASSIGN (SIP service 0x0c, command 0x16) for one range: the vendor
 * msm_sharedmem.c's setup_shared_ram_perms sequence -- source VMID 3 (HLOS), destinations VMID 3
 * and VMID 15 (MSS_MSA), both read/write.  Linux's qcom_scm_assign_mem packs the same three
 * arrays (a mem_map, the source VMID list, the destination list) behind seven arguments; the
 * first three go in x2..x4 and the rest behind the pointer in x5.
 *
 * The page holds: source list at +0, mem_map at +64, destinations at +128, extended arguments at
 * +512.  Everything is flushed to the point of coherency before the call, because TrustZone reads
 * it as a non-caching observer.  The destination entries must be the vendor's 24-byte
 * dest_vm_and_perm_info (u32 vm, u32 perm, u64 ctx, u32 ctx_size, 4 bytes pad); a 16-byte struct
 * makes the TZ read the second VMID from zeroed memory and reject the call with 0x2000000d.
 */
#define SCM_MEM_ASSIGN          SCM_CALL(0x0c, 0x16)
#define SCM_SEVEN_ARGS          0x1117ull       /* 7 args: RO, VAL, RO, VAL, RO, VAL, VAL */

struct scm_mem_map {
	uint64_t address;
	uint64_t size;
};

struct scm_dest_perm {
	uint32_t vmid;
	uint32_t perm;                  /* Linux's QCOM_SCM_PERM_READ 0x4 | WRITE 0x2 */
	uint64_t context;               /* the TZ's dest_vm_and_perm_info: ctx is 64-bit */
	uint32_t context_size;
	uint32_t pad;                   /* sizeof(struct dest_vm_and_perm_info) is 24 on arm64 */
};

int
vayu_scm_assign_shared(uint64_t physical, uint64_t size, uint32_t vmid)
{
	static uint8_t *page;
	uint32_t *source;
	struct scm_mem_map *map;
	struct scm_dest_perm *dest;
	uint64_t *ext;
	uint64_t session = 0, answer = ~0ull, function;

	if (page == NULL) {
		page = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_ZERO);
		if (page == NULL) {
			return -ENOMEM;
		}
	}
	memset(page, 0, PAGE_SIZE);
	source = (uint32_t *)page;
	map = (struct scm_mem_map *)(void *)(page + 64);
	dest = (struct scm_dest_perm *)(void *)(page + 128);
	ext = (uint64_t *)(void *)(page + 512);
	source[0] = 3;                          /* VMID_HLOS */
	map->address = physical;
	map->size = size;
	dest[0].vmid = 3;
	dest[0].perm = 6;                       /* read | write */
	dest[1].vmid = vmid;                    /* 15 = MSS_MSA, 0x18 = WLAN */
	dest[1].perm = 6;
	ext[0] = sizeof(uint32_t);              /* length of the source list */
	ext[1] = kvtophys((vm_offset_t)(page + 128));   /* the destination list */
	ext[2] = sizeof(struct scm_dest_perm) * 2;   /* 48: two 24-byte dest_vm_and_perm_info entries */
	ext[3] = 0;
	FlushPoC_DcacheRegion((vm_offset_t)page, PAGE_SIZE);
	function = SCM_MEM_ASSIGN | scm_convention | SCM_SIP;
	say("scm: about to ask TrustZone to give %#llx..%#llx to VMIDs 3 and 15 read/write: smc %#llx\n",
	    physical, physical + size - 1, function);
	hold();
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = SCM_SEVEN_ARGS;
		register uint64_t x2 __asm__("x2") = kvtophys((vm_offset_t)(page + 64));
		register uint64_t x3 __asm__("x3") = sizeof(struct scm_mem_map);
		register uint64_t x4 __asm__("x4") = kvtophys((vm_offset_t)page);
		register uint64_t x5 __asm__("x5") = kvtophys((vm_offset_t)(page + 512));
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			answer = x1;
			say("scm: TrustZone returns %d, answer %#llx\n", (int32_t)x0, answer);
			return (int32_t)x0 != 0 ? (int32_t)x0 : (int32_t)answer;
		}
	}
	say("scm: TrustZone keeps saying the assign call was interrupted\n");
	return -1;
}

/*
 * The MSA assignment the Linux ath10k qmi.c performs (qmi.c:36-62, :825-850):
 * every region returned by msa_info, secure included, is given from HLOS to
 * {MSS_MSA 15, WLAN 0x18}, plus {WLAN_CE 0x19} for a non-secure region.  HLOS
 * is the source and is NOT in the destination list.  This is called before
 * MSA_READY; a failure aborts the sequence.
 */
int
vayu_scm_assign_msa(uint64_t physical, uint64_t size, bool secure)
{
	static uint8_t *page;
	uint32_t *source;
	struct scm_mem_map *map;
	struct scm_dest_perm *dest;
	uint64_t *ext;
	uint64_t session = 0, answer = ~0ull, function;
	uint32_t count = secure ? 2 : 3;

	if (page == NULL) {
		page = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_ZERO);
		if (page == NULL) {
			return -ENOMEM;
		}
	}
	memset(page, 0, PAGE_SIZE);
	source = (uint32_t *)page;
	map = (struct scm_mem_map *)(void *)(page + 64);
	dest = (struct scm_dest_perm *)(void *)(page + 128);
	ext = (uint64_t *)(void *)(page + 512);
	source[0] = 3;                          /* VMID_HLOS: source only */
	map->address = physical;
	map->size = size;
	dest[0].vmid = 15;                      /* VMID_MSS_MSA */
	dest[0].perm = 6;
	dest[1].vmid = 0x18;                    /* VMID_WLAN */
	dest[1].perm = 6;
	dest[2].vmid = 0x19;                    /* VMID_WLAN_CE, non-secure only */
	dest[2].perm = 6;
	ext[0] = sizeof(uint32_t);
	ext[1] = kvtophys((vm_offset_t)(page + 128));
	ext[2] = sizeof(struct scm_dest_perm) * count;
	ext[3] = 0;
	FlushPoC_DcacheRegion((vm_offset_t)page, PAGE_SIZE);
	function = SCM_MEM_ASSIGN | scm_convention | SCM_SIP;
	say("scm: MSA %#llx..%#llx -> MSS 15, WLAN 0x18%s (source HLOS 3, %u entries)\n",
	    physical, physical + size - 1, secure ? "" : ", WLAN_CE 0x19", count);
	hold();
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = SCM_SEVEN_ARGS;
		register uint64_t x2 __asm__("x2") = kvtophys((vm_offset_t)(page + 64));
		register uint64_t x3 __asm__("x3") = sizeof(struct scm_mem_map);
		register uint64_t x4 __asm__("x4") = kvtophys((vm_offset_t)page);
		register uint64_t x5 __asm__("x5") = kvtophys((vm_offset_t)(page + 512));
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			answer = x1;
			say("scm: MSA TrustZone returns %d, answer %#llx\n", (int32_t)x0, answer);
			return (int32_t)x0 != 0 ? (int32_t)x0 : (int32_t)answer;
		}
	}
	say("scm: MSA TrustZone keeps saying the call was interrupted\n");
	return -1;
}

/*
 * What Linux asks TrustZone first (qcom_scm.c: __get_convention(), qcom_scm_pas_supported()):
 * whether "is this call available" is available, the 64-bit way and else the 32-bit way, which then
 * stays the way every call here is made; and what PAS_IS_SUPPORTED says for its image (vayu_adreno.c,
 * step 11, asked the same for images 1, 4, 9 and 13) -- said, not obeyed (below). False only if
 * TrustZone answers neither way.
 */
bool
vayu_pas_supported(struct vayu_rproc *rp)
{
	uint64_t answer = 0;

	scm_convention = SCM_64;
	if (scm(rp->name, "whether it can be asked (SMC64)", SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE, 0,
	    0, &answer) != 0 || answer != 1) {
		scm_convention = 0;
		answer = 0;
		if (scm(rp->name, "whether it can be asked (SMC32)", SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
		    SCM_SIP | SCM_IS_CALL_AVAILABLE, 0, 0, &answer) != 0 || answer != 1) {
			scm_convention = SCM_64;
			say("%s: TrustZone does not answer the way it answers Linux\n", rp->name);
			return false;
		}
	}
	/*
	 * Logged, not obeyed: on vayu it answers 0 for image 1 (runs 173758, 173903) and the 3D stream
	 * had image 13 taken through INIT_IMAGE/MEM_SETUP/AUTH_AND_RESET (run 172532) without asking it.
	 * INIT_IMAGE's own answer is what counts.
	 */
	answer = 0;
	if (scm(rp->name, "whether it authenticates this image", SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, rp->pas_id, 0, 0, &answer) == 0 &&
	    answer == 1) {
		say("%s: TrustZone says it authenticates image %u (%s calls)\n", rp->name, rp->pas_id, scm_convention ? "64-bit" :
		    "32-bit");
	} else {
		say("%s: TrustZone does not say it authenticates image %u (answer %#llx): logged only, INIT_IMAGE decides (%s calls)\n",
		    rp->name, rp->pas_id, answer, scm_convention ? "64-bit" : "32-bit");
	}
	return true;
}

/* ---- the image ------------------------------------------------------------------------------------------- */

/* A file of /fw on the root, read whole into memory of ours (hakios.adsp.c: root_file()). */
static const uint8_t *
root_file(const struct vayu_rproc *rp, const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > rp->memory_size) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		say("%s: %s: read error %d, %d bytes left\n", rp->name, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

/* fw/ of the small image, else /fw of the root; *allocated: not 0 if it is memory of ours, to be given back. */
static const uint8_t *
image_file(const struct vayu_rproc *rp, const char *name, uint64_t *size, uint64_t *allocated)
{
	const uint8_t *data = hakios_image_file("fw", name, size, NULL);

	*allocated = 0;
	if (data != NULL) {
		return data;
	}
	data = root_file(rp, name, size);
	if (data != NULL) {
		*allocated = *size;
		return data;
	}
	say("%s: no fw/%s in the small image nor on the root (this port repo's firmware/vayu/modem/%s.* into fw-audio/)\n", rp->name,
	    name, rp->firmware);
	return NULL;
}

static void
image_forget(struct vayu_rproc *rp)
{
	for (unsigned s = 0; s < rp->image.segments; s++) {
		if (rp->image.segment[s].file_alloc != 0) {
			uint8_t *ours = __DECONST(uint8_t *, rp->image.segment[s].file);

			kfree_data(ours, rp->image.segment[s].file_alloc);
		}
		rp->image.segment[s].file = NULL;
		rp->image.segment[s].file_alloc = 0;
	}
	if (rp->image.header_alloc != 0) {
		uint8_t *ours = __DECONST(uint8_t *, rp->image.header);

		kfree_data(ours, rp->image.header_alloc);
	}
	rp->image.header = NULL;
	rp->image.header_alloc = 0;
	rp->image.segments = 0;
}

#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MDT_RELOCATABLE         (1u << 27)      /* mdt_loader.h:9: QCOM_MDT_RELOCATABLE, bit 27 of p_flags */

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static bool
image_check(struct vayu_rproc *rp)
{
	struct elf_segment segment;
	const uint8_t *header;
	char name[24];
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, base, lowest = ~0u, highest = 0;
	bool relocatable = false;

	image_forget(rp);
	snprintf(name, sizeof(name), "%s.mdt", rp->firmware);
	header = image_file(rp, name, &size, &rp->image.header_alloc);
	if (header == NULL) {
		return false;
	}
	rp->image.header = header;
	rp->image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > VAYU_PAS_MAX_SEGMENTS || 52 + 32 * count > size ||
	    size > PAGE_SIZE) {
		say("%s: %s is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", rp->name, name, header[0], header[1],
		    header[2], header[3], count, size);
		goto fail;
	}
	/*
	 * Linux's rule (mdt_loader.c:268-269, 368-369): an image is relocatable if any loadable
	 * segment has BIT(27) of p_flags, and is then placed from its lowest address; only an image
	 * that is not is placed from the start of its memory (mdt_loader.c:271-275, 375-386).
	 */
	for (uint32_t i = 0; i < count; i++) {
		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		relocatable |= (segment.flags & MDT_RELOCATABLE) != 0;
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
	}
	base = relocatable ? lowest : rp->memory;
	for (uint32_t i = 0; i < count; i++) {
		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.paddr < base || segment.file_size > segment.memory_size ||
		    (uint64_t)(segment.paddr - base) + segment.memory_size > rp->memory_size) {
			say("%s: segment %u (%#x, %#x bytes) is not within its memory (%#x, %#x from %#x)\n", rp->name, i,
			    segment.paddr, segment.memory_size, rp->memory, rp->memory_size, base);
			goto fail;
		}
		rp->image.segment[rp->image.segments].at = segment.paddr - base;
		rp->image.segment[rp->image.segments].file_size = segment.file_size;
		rp->image.segment[rp->image.segments].memory_size = segment.memory_size;
		rp->image.segment[rp->image.segments].file = NULL;
		rp->image.segment[rp->image.segments].file_alloc = 0;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "%s.b%02u", rp->firmware, i);
			rp->image.segment[rp->image.segments].file = image_file(rp, name, &file_size,
			    &rp->image.segment[rp->image.segments].file_alloc);
			if (rp->image.segment[rp->image.segments].file == NULL) {
				goto fail;
			}
			rp->image.segments++;           /* (from here its file is given back with the rest) */
			if (file_size != segment.file_size) {
				say("%s: %s has %llu bytes, the header says %u\n", rp->name, name, file_size, segment.file_size);
				goto fail;
			}
			files += file_size;
		} else {
			rp->image.segments++;
		}
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
	}
	if (rp->image.segments == 0 || (!relocatable && lowest != rp->memory) ||
	    52 + 32 * count + (uint64_t)hashes != size) {
		say("%s: not what TrustZone takes: %u segments, the lowest at %#x; the header file has %llu bytes, the header %u and "
		    "the hashes %u\n", rp->name, rp->image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	rp->image.size = ((highest + 0xfff) & ~0xfffu) - base;
	say("%s: the image is whole: a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x bytes of its %#x "
	    "from %#x, linked at %#x%s\n", rp->name, size, hashes, rp->image.segments, files, rp->image.size, rp->memory_size,
	    rp->memory, lowest, relocatable ? " (relocatable: placed at the start of its memory)" : "");
	return true;
fail:
	image_forget(rp);
	return false;
}

static uint8_t
segment_byte(const struct vayu_rproc *rp, unsigned s, uint32_t at)
{
	return at < rp->image.segment[s].file_size ? rp->image.segment[s].file[at] : 0;
}

static uint64_t
segment_word(const struct vayu_rproc *rp, unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8; b++) {
		word |= (uint64_t)segment_byte(rp, s, at + b) << (8 * b);
	}
	return word;
}

/*
 * One segment written (wrong = false) or compared (true): eight bytes at a time where its place in
 * memory is a multiple of eight and eight are left, single bytes elsewhere -- a segment need not
 * begin or end on eight (the modem's phdr 28 is 0x824dc bytes long). Returns the units that differ.
 */
static uint64_t
segment_pass(const struct vayu_rproc *rp, volatile uint8_t *memory, unsigned s, bool compare)
{
	uint32_t at = rp->image.segment[s].at, size = rp->image.segment[s].memory_size;
	uint64_t wrong = 0;

	for (uint32_t i = 0; i < size;) {
		if (((at + i) & 7) == 0 && size - i >= 8) {
			volatile uint64_t *word = (volatile uint64_t *)(memory + at + i);
			uint64_t value = segment_word(rp, s, i);

			if (compare) {
				wrong += *word != value;
			} else {
				*word = value;
			}
			i += 8;
		} else {
			uint8_t value = segment_byte(rp, s, i);

			if (compare) {
				wrong += memory[at + i] != value;
			} else {
				memory[at + i] = value;
			}
			i++;
		}
	}
	return wrong;
}

static bool
image_write(struct vayu_rproc *rp)
{
	volatile uint8_t *memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	say("%s: about to write the image into its memory (%#x, %#x bytes; nobody's until TrustZone starts it) and read it back\n",
	    rp->name, rp->memory, rp->memory_size);
	hold();
	memory = (volatile uint8_t *)ml_io_map_unmappable(rp->memory, rp->memory_size, VM_WIMG_WCOMB);
	if (memory == NULL) {
		say("%s: its memory cannot be mapped\n", rp->name);
		return false;
	}
	for (unsigned s = 0; s < rp->image.segments; s++) {
		segment_pass(rp, memory, s, false);
		written += rp->image.segment[s].file_size;
		cleared += rp->image.segment[s].memory_size - rp->image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < rp->image.segments; s++) {
		wrong += segment_pass(rp, memory, s, true);
	}
	settle();
	ml_io_unmap((vm_offset_t)memory, rp->memory_size);
	say("%s: %u segments, %llu bytes written and %llu cleared; read back, %llu words or bytes differ\n", rp->name,
	    rp->image.segments, written, cleared, wrong);
	return wrong == 0;
}

static void smp2p_poll(struct vayu_rproc *rp, struct smp2p *last, bool *gave_up);

/* ---- starting and stopping ----------------------------------------------------------------------------------- */

/* Linux's qcom_q6v5_request_stop(): our bit "stop", its "stop ack" waited for, our bit taken back. */
static bool
ask_to_stop(struct vayu_rproc *rp, uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(rp), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !vayu_smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC ||
	    (ours.version & 0xff) != SMP2P_VERSION || (entry = smp2p_entry(&ours, OUR_ENTRY)) < 0) {
		say("%s: not asked to stop first: no SMP2P item of ours to ask with\n", rp->name);
		return false;
	}
	if (!their_words(rp, &theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		say("%s: not asked to stop first: its item has no entry to answer in\n", rp->name);
		return false;
	}
	state = slave_state(&theirs);
	if (state & VAYU_RPROC_STOP_ACK) {
		say("%s: not asked to stop: its entry says %#x, stopped when asked before\n", rp->name, state);
		return true;
	}
	if (!(state & VAYU_RPROC_READY) || (state & VAYU_RPROC_FATAL)) {
		say("%s: not asked to stop first: its entry says %#x, not \"up\"\n", rp->name, state);
		return false;
	}
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	say("%s: about to ask it to stop: bit 0 of our \"%s\" at %#lx, and APCS bit %u\n", rp->name, OUR_ENTRY,
	    SMEM_PHYS + value_at, rp->apcs_smp2p);
	vayu_smem_poke(value_at, ours.entry[entry].value | OUR_STOP);
	settle();
	vayu_apcs_ring(rp->apcs_smp2p);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (their_words(rp, &theirs) && (slave_state(&theirs) & (VAYU_RPROC_STOP_ACK | VAYU_RPROC_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = their_words(rp, &theirs) ? slave_state(&theirs) : 0;
	say("%s: %s after %llu.%03llu ms; its entry says %#x\n", rp->name, state & VAYU_RPROC_STOP_ACK ? "IT ACKNOWLEDGES THE STOP" :
	    "IT DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000, (now - asked) % 1000, state);
	vayu_smem_poke(value_at, ours.entry[entry].value & ~OUR_STOP);
	settle();
	vayu_apcs_ring(rp->apcs_smp2p);
	return (state & VAYU_RPROC_STOP_ACK) != 0;
}

static bool
rproc_shutdown(struct vayu_rproc *rp, const char *why)
{
	char text[QMP_MESSAGE];
	uint64_t answer = ~0ull;
	int32_t result;

	say("%s: %s: asked to stop, then shut down by TrustZone\n", rp->name, why);
	if (rp->glink != NULL) {
		if (leaving) {
			rp->glink->up = false;
		} else {
			vayu_glink_down(rp->glink);
		}
	}
	rp->stopped_by_its_word = ask_to_stop(rp, leaving ? 2000 : 5000);
	result = scm(rp->name, "to shut it down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, rp->pas_id, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		say("%s: TRUSTZONE DOES NOT SHUT IT DOWN: %d, answer %#llx\n", rp->name, result, answer);
		return false;
	}
	say("%s: SHUT DOWN\n", rp->name);
	rp->started = false;
	snprintf(text, sizeof(text), "{class: image, res: load_state, name: %s, val: off}", rp->name);
	vayu_aop_send(text);
	rails_give_back(rp);
	return true;
}

bool
vayu_pas_load(struct vayu_rproc *rp)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;
	bool loaded = false;

	lck_mtx_lock(&pas_lock);
	rp->image.written = false;
	if (!image_check(rp)) {
		goto out;
	}
	if (their_words(rp, &words) && (slave_state(&words) & VAYU_RPROC_READY)) {
		show_words(rp, "it has left words in the shared memory and may be running", &words);
		if (!rproc_shutdown(rp, "it may be running")) {
			goto out;
		}
		if (!rp->stopped_by_its_word) {
			say("%s: NOT STARTED AGAIN: it was shut down without having acknowledged a stop (redwood's Wi-Fi DSP reset the "
			    "phone when started after that)\n", rp->name);
			goto out;
		}
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("%s: no memory\n", rp->name);
		goto out;
	}
	memcpy((void *)page, rp->image.header, rp->image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm(rp->name, "to take its header (signature and hashes)", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, rp->pas_id,
	    kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		say("%s: TRUSTZONE DOES NOT TAKE THE HEADER: %d, answer %#llx\n", rp->name, result, answer);
		goto out;
	}
	say("%s: TRUSTZONE ACCEPTS THE SIGNATURE (image %u)\n", rp->name, rp->pas_id);
	result = scm(rp->name, "where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, rp->pas_id, rp->memory, rp->image.size,
	    &answer);
	if (result != 0 || answer != 0) {
		say("%s: TRUSTZONE DOES NOT TAKE THE PLACE: %d, answer %#llx\n", rp->name, result, answer);
		goto out;
	}
	say("%s: TrustZone takes the place: %#x bytes at %#x\n", rp->name, rp->image.size, rp->memory);
	rp->image.written = image_write(rp);
	loaded = rp->image.written;
out:
	if (page != 0) {
		kmem_free(kernel_map, page, PAGE_SIZE);
	}
	image_forget(rp);
	lck_mtx_unlock(&pas_lock);
	return loaded;
}

bool
vayu_pas_start(struct vayu_rproc *rp, uint32_t wait_ms)
{
	struct smp2p words, last;
	char text[QMP_MESSAGE];
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, changes = 0;
	int32_t result;
	bool have, handed_over = false;

	if (!rp->image.written) {
		say("%s: the image is not in place\n", rp->name);
		return false;
	}
	if (!our_item_fill(rp)) {
		return false;
	}
	bzero(&last, sizeof(last));
	have = their_words(rp, &last);
	lck_mtx_lock(&pas_lock);
	if (rail_count(rp) != 0) {
		if (!vayu_rpmh_vote(rp->rails, rail_count(rp), rp->rail_level)) {
			lck_mtx_unlock(&pas_lock);
			return false;
		}
		rp->rails_asked = true;
	}
	snprintf(text, sizeof(text), "{class: image, res: load_state, name: %s, val: on}", rp->name);
	if (!vayu_aop_send(text)) {
		goto undo;
	}
	result = scm(rp->name, "to authenticate the image and let it out of reset", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE,
	    rp->pas_id, 0, 0, &answer);
	started = now_us();
	rp->image.written = false;
	if (result != 0 || answer != 0) {
		say("%s: TRUSTZONE DOES NOT START IT: %d, answer %#llx\n", rp->name, result, answer);
		snprintf(text, sizeof(text), "{class: image, res: load_state, name: %s, val: off}", rp->name);
		vayu_aop_send(text);
		goto undo;
	}
	rp->started = true;
	lck_mtx_unlock(&pas_lock);
	say("%s: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET IT OUT OF RESET; watching for %u ms\n", rp->name, wait_ms);
	limit = started + wait_ms * 1000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !rp->started) {
			break;
		}
		if (their_words(rp, &words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: its item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(rp, when, &words);
			}
			last = words;
			have = true;
			smp2p_listen(rp, &words);
			state = slave_state(&words);
			if ((state & VAYU_RPROC_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull < limit ? now + 3000000ull : limit;
				say("%s: IT SAYS IT IS UP (\"slave-kernel\" %#x), %llu.%03llu ms after the start\n", rp->name, state,
				    (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & VAYU_RPROC_HANDOVER) && !handed_over) {
				handed_over = true;
				say("%s: it has asked for its own power (bit 2): our vote ends\n", rp->name);
				lck_mtx_lock(&pas_lock);
				rails_give_back(rp);
				lck_mtx_unlock(&pas_lock);
			}
			if (state & VAYU_RPROC_FATAL) {
				say("%s: IT SAYS IT HAS GIVEN UP\n", rp->name);
				show_crash_reason(rp);
				break;
			}
		}
		sleep_us(200);
	}
	if (ready_at == 0 && !(state & VAYU_RPROC_FATAL)) {
		say("%s: %s\n", rp->name, have ? "IT RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM IT");
		show_crash_reason(rp);
	}
	return ready_at != 0 && !(state & VAYU_RPROC_FATAL);
undo:
	rails_give_back(rp);
	lck_mtx_unlock(&pas_lock);
	return false;
}

bool
vayu_pas_stop(struct vayu_rproc *rp, const char *why)
{
	bool down = true;

	lck_mtx_lock(&pas_lock);
	if (rp->started) {
		down = rproc_shutdown(rp, why);
	}
	lck_mtx_unlock(&pas_lock);
	return down;
}

void
vayu_pas_quiesce(struct vayu_rproc *rp)
{
	if (rp == NULL || !rp->started) {
		return;
	}
	leaving = true;
	rproc_shutdown(rp, "the kernel is on its way back to the loader");
}

/* What changed in its SMP2P item since *last, acted on (from an edge's thread). */
static void
smp2p_poll(struct vayu_rproc *rp, struct smp2p *last, bool *gave_up)
{
	struct smp2p words;

	if (their_words(rp, &words) && memcmp(&words, last, sizeof(words)) != 0) {
		*last = words;
		smp2p_listen(rp, &words);
		if ((slave_state(&words) & VAYU_RPROC_FATAL) && !*gave_up) {
			*gave_up = true;
			say("%s: link: IT SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", rp->name, slave_state(&words));
			show_crash_reason(rp);
		}
	}
}

/* ---- GLINK (hakios.adsp.c's, one edge per struct vayu_glink) ---------------------------------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};

static void
glink_lock_ready(struct vayu_glink *g)
{
	if (!g->lock_ready) {
		lck_mtx_init(&g->lock, &pas_lock_group, LCK_ATTR_NULL);
		g->lock_ready = true;
	}
}

/*
 * The descriptor (four words: our queue's tail and head, its queue's tail and head) and our queue,
 * made by us as Linux's qcom_glink_smem_register() makes them when the processor starts; its queue
 * (480) is made by it, and found once it is there. On vayu nobody made 478/479 before us (hardware
 * run 172548: "NOT THERE").
 */
bool
vayu_glink_prepare(struct vayu_glink *g)
{
	uint32_t size = 0;

	glink_lock_ready(g);
	g->rp->glink = g;
	if (g->fifo_size == 0) {
		g->fifo_size = 16384;
	}
	g->descriptor = vayu_smem_alloc(g->rp->smem_host, ITEM_GLINK_DESCRIPTOR, 32, g->rp->name);
	g->ours = vayu_smem_alloc(g->rp->smem_host, ITEM_GLINK_OUR_QUEUE, g->fifo_size, g->rp->name);
	if (g->descriptor == 0 || g->ours == 0) {
		say("%s: glink: the descriptor (%#x) or our queue (%#x) could not be made\n", g->rp->name, g->descriptor, g->ours);
		return false;
	}
	g->ours = vayu_smem_item(g->rp->smem_host, ITEM_GLINK_OUR_QUEUE, &size);
	g->ours_size = size & ~7u;
	vayu_smem_poke(g->descriptor + GLINK_OUR_HEAD, 0);
	vayu_smem_poke(g->descriptor + GLINK_THEIR_TAIL, 0);
	settle();
	say("%s: glink: descriptor at %#lx; our queue %u bytes at %#lx; marks: ours %#x/%#x, its %#x/%#x\n", g->rp->name,
	    SMEM_PHYS + g->descriptor, g->ours_size, SMEM_PHYS + g->ours, vayu_smem_word(g->descriptor + GLINK_OUR_TAIL),
	    vayu_smem_word(g->descriptor + GLINK_OUR_HEAD), vayu_smem_word(g->descriptor + GLINK_THEIR_TAIL),
	    vayu_smem_word(g->descriptor + GLINK_THEIR_HEAD));
	return g->ours_size >= 1024;
}

static bool
glink_theirs_queue(struct vayu_glink *g)
{
	uint32_t size = 0;

	if (g->theirs != 0) {
		return true;
	}
	g->theirs = vayu_smem_item(g->rp->smem_host, ITEM_GLINK_THEIR_QUEUE, &size);
	g->theirs_size = size & ~7u;
	if (g->theirs != 0 && g->theirs_size >= 1024) {
		say("%s: glink: its queue (480): %u bytes at %#lx\n", g->rp->name, g->theirs_size, SMEM_PHYS + g->theirs);
		return true;
	}
	g->theirs = 0;
	return false;
}

static struct vayu_glink_channel *
glink_theirs(struct vayu_glink *g, uint32_t theirs)
{
	for (unsigned c = 0; c < g->channels; c++) {
		if (g->channel[c].theirs != 0 && g->channel[c].theirs == theirs) {
			return &g->channel[c];
		}
	}
	return NULL;
}

static uint32_t
glink_waiting(struct vayu_glink *g)
{
	uint32_t head, tail;

	if (!glink_theirs_queue(g)) {
		return 0;
	}
	head = vayu_smem_word(g->descriptor + GLINK_THEIR_HEAD);
	tail = vayu_smem_word(g->descriptor + GLINK_THEIR_TAIL);
	if (head >= g->theirs_size || tail >= g->theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : g->theirs_size - tail + head;
}

static void
glink_peek(struct vayu_glink *g, void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (vayu_smem_word(g->descriptor + GLINK_THEIR_TAIL) + offset) % g->theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[g->theirs + (at + i) % g->theirs_size];
	}
}

static void
glink_advance(struct vayu_glink *g, uint32_t count)
{
	vayu_smem_poke(g->descriptor + GLINK_THEIR_TAIL, (vayu_smem_word(g->descriptor + GLINK_THEIR_TAIL) + count) % g->theirs_size);
}

static uint32_t
glink_room(struct vayu_glink *g)
{
	uint32_t head = vayu_smem_word(g->descriptor + GLINK_OUR_HEAD), tail = vayu_smem_word(g->descriptor + GLINK_OUR_TAIL), room;

	if (head >= g->ours_size || tail >= g->ours_size) {
		return 0;
	}
	room = tail <= head ? g->ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(struct vayu_glink *g, const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + g->ours;
	uint32_t head = vayu_smem_word(g->descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!g->up || glink_room(g) < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % g->ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	vayu_smem_poke(g->descriptor + GLINK_OUR_HEAD, (head + padded) % g->ours_size);
	settle();
	vayu_apcs_ring(g->apcs_glink);
	g->said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

static void
glink_say(struct vayu_glink *g, unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(g, &message, sizeof(message), more, more_length);

	if (g->trace > 0 || !sent) {
		g->trace -= g->trace > 0;
		say("%s: glink: > %s %u %#x%s\n", g->rp->name, glink_name(command), param1, param2,
		    sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static bool
glink_offer(struct vayu_glink *g, struct vayu_glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data != NULL && channel->our_intents[i].size != size) {
		kfree_data(channel->our_intents[i].data, channel->our_intents[i].size);
	}
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intent_count++;
	glink_say(g, GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything it has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With the edge's lock held. */
static void
glink_pump(struct vayu_glink *g)
{
	struct glink_message message;
	struct vayu_glink_channel *channel;
	uint32_t words[4];
	char name[33];

	while (g->up && !g->broken) {
		uint32_t waiting = glink_waiting(g), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(g, &message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(g, words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > g->theirs_size - 8) {
			say("%s: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", g->rp->name, length,
			    glink_name(message.command), message.param1, message.param2);
			g->broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		g->heard++;
		if (g->trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			g->trace--;
			say("%s: glink: < %s %u %#x\n", g->rp->name, glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				g->features &= message.param2;
			}
			glink_advance(g, length);
			glink_say(g, GLINK_VERSION_ACK, GLINK_VERSION_1, g->features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(g, length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == g->features) {
				g->agreed = true;
			} else if (message.param1 != 0) {
				g->features &= message.param2;
				glink_say(g, GLINK_VERSION, GLINK_VERSION_1, g->features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(g, name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(g, length);
			channel = NULL;
			for (unsigned c = 0; c < g->channels; c++) {
				if (strcmp(name, g->channel[c].name) == 0) {
					channel = &g->channel[c];
				}
			}
			if (channel == NULL) {
				say("%s: glink: it opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", g->rp->name, name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (g->others[i][0] == 0) {
						strlcpy(g->others[i], name, sizeof(g->others[i]));
						break;
					}
				}
				break;
			}
			say("%s: glink: it opens \"%s\" as its channel %u: acknowledged, and opened from our side as %u\n", g->rp->name,
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(g, GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(g, GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(g, length);
			for (unsigned c = 0; c < g->channels; c++) {
				if (message.param1 == g->channel[c].ours) {
					g->channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(g, message.param1);
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				glink_peek(g, words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						break;
					}
				}
			}
			glink_advance(g, length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(g, message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(g, length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(g, length);
			if ((channel = glink_theirs(g, message.param1)) != NULL) {
				bool granted = glink_offer(g, channel, message.param2);

				glink_say(g, GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(g, length);
			for (unsigned c = 0; c < g->channels; c++) {
				if (message.param1 == g->channel[c].theirs || message.param1 == g->channel[c].ours) {
					g->channel[c].request_answered = true;
					g->channel[c].request_granted = message.param2 != 0;
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(g, message.param1);
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				say("%s: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", g->rp->name,
				    words[0], message.param1, message.param2);
				glink_advance(g, length);
				break;
			}
			glink_peek(g, channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(g, length);
			if (words[1] == 0) {
				struct vayu_glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(g, GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(g, length);
			vayu_apcs_ring(g->apcs_glink);
			break;
		case GLINK_CLOSE:
			glink_advance(g, length);
			glink_say(g, GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(g, message.param1)) != NULL) {
				say("%s: glink: IT CLOSES \"%s\"\n", g->rp->name, channel->name);
				channel->open = false;
				channel->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(g, length);
			break;
		default:
			say("%s: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", g->rp->name, message.command,
			    message.param1, message.param2);
			g->broken = true;
			return;
		}
		for (unsigned c = 0; c < g->channels; c++) {
			channel = &g->channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				say("%s: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, its %u); our intents told\n", g->rp->name,
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(g, channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/* One whole message on a channel; waits up to five seconds for an intent of its that it fits. */
int
vayu_glink_send(struct vayu_glink *g, struct vayu_glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&g->lock);
	if (g->up && length + sizeof(header) + 8 > g->ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&g->lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump(g);
		if (!g->up || !channel->open || g->broken) {
			lck_mtx_unlock(&g->lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room(g) >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink_say(g, GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			lck_mtx_unlock(&g->lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&g->lock);
		sleep_ms(1);
		lck_mtx_lock(&g->lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(g, &header, sizeof(header), data, length);
	lck_mtx_unlock(&g->lock);
	return 0;
}

/* The edge's thread: every millisecond its SMP2P item and its queue looked at; what arrived handed to the channels' inputs. */
static void
glink_thread(void *parameter, __unused wait_result_t result)
{
	struct vayu_glink *g = parameter;
	struct smp2p last;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	their_words(g->rp, &last);
	while (g->thread_wanted) {
		sleep_us(1000);
		smp2p_poll(g->rp, &last, &gave_up);
		for (;;) {
			struct vayu_glink_packet *packet = NULL;
			struct vayu_glink_channel *from = NULL;

			lck_mtx_lock(&g->lock);
			glink_pump(g);
			for (unsigned c = 0; c < g->channels && packet == NULL; c++) {
				struct vayu_glink_channel *channel = &g->channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = channel;
				}
			}
			lck_mtx_unlock(&g->lock);
			if (packet == NULL) {
				break;
			}
			if (from->input != NULL) {
				from->input(from->context, packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		if (g->tick != NULL) {
			g->tick(g->tick_context);
		}
	}
	g->thread_runs = false;
	thread_terminate(current_thread());
}

bool
vayu_glink_up(struct vayu_glink *g, uint32_t wait_ms)
{
	thread_t thread;
	uint64_t began;

	if (!g->rp->started || g->descriptor == 0 || g->channels == 0 || g->channels > VAYU_GLINK_CHANNELS) {
		say("%s: glink: not taken up: %s\n", g->rp->name, !g->rp->started ? "the processor does not run" :
		    g->descriptor == 0 ? "no descriptor (vayu_glink_prepare)" : "no channels");
		return false;
	}
	say("%s: glink: about to take it up: our version, then whatever its messages call for; doorbell APCS bit %u\n", g->rp->name,
	    g->apcs_glink);
	hold();
	lck_mtx_lock(&g->lock);
	g->features = GLINK_INTENT_REUSE;
	g->agreed = g->broken = false;
	g->said = g->heard = 0;
	for (unsigned c = 0; c < g->channels; c++) {
		struct vayu_glink_channel *channel = &g->channel[c];

		channel->ours = c + 1;
		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	bzero(g->others, sizeof(g->others));
	g->trace = 60;
	g->up = true;
	began = now_us();
	glink_say(g, GLINK_VERSION, GLINK_VERSION_1, g->features, NULL, 0);
	lck_mtx_unlock(&g->lock);
	g->thread_wanted = g->thread_runs = true;
	if (kernel_thread_start(glink_thread, g, &thread) != KERN_SUCCESS) {
		g->thread_wanted = g->thread_runs = false;
		say("%s: glink: no thread\n", g->rp->name);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < wait_ms && !g->channel[0].open && !g->broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	say("%s: glink: %llu ms after our version: versions %s (features %#x); its queue %s; \"%s\" %s; it also opened \"%s\" \"%s\" "
	    "\"%s\" \"%s\"\n", g->rp->name, (now_us() - began) / 1000, g->agreed ? "agreed" : "NOT AGREED", g->features,
	    g->theirs ? "found" : "NOT FOUND", g->channel[0].name, g->channel[0].open ? "OPEN" : "NOT OPEN", g->others[0],
	    g->others[1], g->others[2], g->others[3]);
	return g->channel[0].open;
}

void
vayu_glink_down(struct vayu_glink *g)
{
	struct vayu_glink_packet *packet;

	if (!g->lock_ready) {
		return;
	}
	if (g->thread_runs) {
		g->thread_wanted = false;
		for (unsigned ms = 0; ms < 2000 && g->thread_runs; ms++) {
			sleep_ms(1);
		}
		say("%s: glink: %s; said %u and heard %u messages\n", g->rp->name, g->thread_runs ? "THE THREAD DOES NOT END" : "down",
		    g->said, g->heard);
	}
	lck_mtx_lock(&g->lock);
	g->up = false;
	for (unsigned c = 0; c < g->channels; c++) {
		struct vayu_glink_channel *channel = &g->channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
	}
	lck_mtx_unlock(&g->lock);
}

/* ---- QRTR (hakios.adsp.c's: hello answered, the services written down; ports of our own for the QMI helpers, and
 *      the services of our node announced to the peer) -------------------------------------------------------- */

#define VAYU_QRTR_MAX           8192            /* the most a datagram holds, either way */
#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
#define QRTR_FIRST_PORT         0x4000u         /* where Linux begins numbering ports that ask for no number */
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];                      /* a server: service, instance, node, port; a client: node, port */
};

/* A port of our node (sys/hakios.qrtr.h): what the QMI helpers hold while they speak. */
struct hakios_qrtr_port {
	uint32_t number;                        /* 0: free */
	struct vayu_qrtr *router;               /* the router it speaks through: bound by its first send, datagram or server
	                                         * callback (NULL: not yet); its callbacks come from that router's thread only */
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
	struct {
		uint32_t service, instance;     /* the instance as on the wire: version | instance << 8; 0: any */
		bool asked;
		uint8_t fresh;                  /* a bit per router slot: not yet told of that router's services from before */
	} lookups[4];
};

/*
 * More than one router at a time (the modem's, the sensors DSP's, the audio DSP's): our node is one, so
 * its ports and the services it publishes are shared by all of them. A datagram goes out through the
 * router behind its destination node (the peer itself, or a node one of its services is at), else
 * through the port's own; a lookup is answered from every router, and the first answer binds the port
 * to that router (a port speaks through one router); our services are told to each.
 * Linux does the same with one node and several endpoints (net/qrtr/af_qrtr.c qrtr_node_lookup()).
 */
#define QRTR_ROUTERS            4
static LCK_MTX_DECLARE(qrtr_lock, &pas_lock_group);     /* the routers, the ports and the published table */
static struct vayu_qrtr *qrtr_routers[QRTR_ROUTERS];
static uint32_t qrtr_next_port = QRTR_FIRST_PORT;
static struct hakios_qrtr_port qrtr_ports[16];
static struct {
	struct hakios_qrtr_port *port;
	uint32_t service, instance;
	bool there;
} qrtr_published[8];                            /* the services of ours the peers are told of */
static uint32_t qrtr_gone;                      /* (only the first 16 clients' going are said) */

static bool
qrtr_is_attached(const struct vayu_qrtr *q)
{
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		if (q != NULL && qrtr_routers[r] == q) {
			return true;
		}
	}
	return false;
}

/* q's slot in the table; -1: not attached. */
static int
qrtr_slot(const struct vayu_qrtr *q)
{
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		if (q != NULL && qrtr_routers[r] == q) {
			return (int)r;
		}
	}
	return -1;
}

/* Under qrtr_lock: whether port p may hear from q (bound to it, or not bound yet: then it is bound to q now). */
static bool
qrtr_bind(struct hakios_qrtr_port *port, struct vayu_qrtr *q)
{
	if (port->router == NULL) {
		port->router = q;
	}
	return port->router == q;
}

static bool
qrtr_any_attached(void)
{
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		if (qrtr_routers[r] != NULL) {
			return true;
		}
	}
	return false;
}

/* Whether this is the link thread of a router (a callback's: a close from there must not wait on callbacks). */
static bool
qrtr_on_link_thread(void)
{
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		if (qrtr_routers[r] != NULL && qrtr_routers[r]->link_thread == current_thread()) {
			return true;
		}
	}
	return false;
}

/* The router behind that node: its peer, else the router one of whose services is at that node; NULL: none known. */
static struct vayu_qrtr *
qrtr_route(uint32_t node)
{
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		if (qrtr_routers[r] != NULL && qrtr_routers[r]->peer_known && qrtr_routers[r]->peer == node) {
			return qrtr_routers[r];
		}
	}
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		struct vayu_qrtr *q = qrtr_routers[r];

		for (unsigned i = 0; q != NULL && i < sizeof(q->servers) / sizeof(q->servers[0]); i++) {
			if (q->servers[i].there && q->servers[i].node == node) {
				return q;
			}
		}
	}
	return NULL;
}

/* One datagram out, of any type. */
static int
qrtr_out_packet(struct vayu_qrtr *q, uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data,
    uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	if (length == 0 || length > VAYU_QRTR_MAX) {
		return EMSGSIZE;
	}
	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = vayu_glink_send(q->glink, q->channel, packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	q->out += error == 0;
	return error;
}

/* Everybody who looked a service up (and speaks through q, or through no router yet) is told that it has come, or gone.
 * From q's link thread. */
static void
qrtr_tell(struct vayu_qrtr *q, uint32_t service, uint32_t instance, uint32_t node, uint32_t port, bool arrived)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			hakios_qrtr_server_t server = NULL;
			void *context = NULL;
			int slot;

			lck_mtx_lock(&q->calling);      /* (a close from another thread waits for this callback) */
			lck_mtx_lock(&qrtr_lock);
			slot = qrtr_slot(q);
			if (slot >= 0 && qrtr_ports[p].number != 0 && qrtr_ports[p].lookups[l].asked &&
			    !(qrtr_ports[p].lookups[l].fresh & (1u << slot)) && qrtr_ports[p].lookups[l].service == service &&
			    (qrtr_ports[p].lookups[l].instance == 0 || qrtr_ports[p].lookups[l].instance == instance) &&
			    (arrived ? qrtr_bind(&qrtr_ports[p], q) : qrtr_ports[p].router == q)) {
				server = qrtr_ports[p].server;
				context = qrtr_ports[p].context;
			}
			lck_mtx_unlock(&qrtr_lock);
			if (server != NULL) {
				server(context, arrived, service, instance & 0xff, instance >> 8, node, port);
			}
			lck_mtx_unlock(&q->calling);
		}
	}
}

/* One of ours to q's peer: Linux ns.c's service_announce_new() and service_announce_del(), through the control port. */
static void
qrtr_announce(struct vayu_qrtr *q, unsigned index, bool new)
{
	struct qrtr_control announce = { .command = new ? QRTR_NEW_SERVER : QRTR_DEL_SERVER };
	const char *who;
	uint32_t service, instance, port;
	bool send;
	int error = 0;

	if (q == NULL || index >= sizeof(qrtr_published) / sizeof(qrtr_published[0])) {
		return;
	}
	who = q->glink->rp->name;
	lck_mtx_lock(&qrtr_lock);
	if (!qrtr_published[index].there) {
		lck_mtx_unlock(&qrtr_lock);
		return;
	}
	service = qrtr_published[index].service;
	instance = qrtr_published[index].instance;
	port = qrtr_published[index].port != NULL ? qrtr_published[index].port->number : 0;
	lck_mtx_unlock(&qrtr_lock);
	announce.words[0] = service;
	announce.words[1] = instance;
	announce.words[2] = QRTR_US;
	announce.words[3] = port;
	send = q->peer_known && q->hellos != 0 && port != 0;
	if (send) {
		error = qrtr_out_packet(q, announce.command, QRTR_CONTROL, q->peer, QRTR_CONTROL, &announce, sizeof(announce));
	}
	if (new) {
		say("%s: qrtr: ours announced: service %u (%#x) instance word %#x at port %u: %s\n", who, service, service, instance, port,
		    send && error == 0 ? "SENT" : "NOT SENT");
	} else {
		say("%s: qrtr: ours withdrawn: service %u (%#x) instance word %#x at port %u: %s\n", who, service, service, instance, port,
		    send && error == 0 ? "SENT" : "NOT SENT");
	}
}

/* ... to every attached router. */
static void
qrtr_announce_all(unsigned index, bool new)
{
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		if (qrtr_routers[r] != NULL) {
			qrtr_announce(qrtr_routers[r], index, new);
		}
	}
}

static bool
qrtr_matches(const struct vayu_qrtr *q, uint32_t service, uint32_t instance, unsigned server)
{
	return q->servers[server].there && q->servers[server].service == service &&
	       (instance == 0 || q->servers[server].instance == instance);
}

/* Lookups made since q's last look are told of q's services that were there before them. From q's link thread. */
static void
qrtr_fresh(struct vayu_qrtr *q)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			hakios_qrtr_server_t server;
			uint32_t service, instance, number;
			void *context;
			int slot;

			lck_mtx_lock(&qrtr_lock);
			slot = qrtr_slot(q);
			if (slot < 0 || qrtr_ports[p].number == 0 || !(qrtr_ports[p].lookups[l].fresh & (1u << slot))) {
				lck_mtx_unlock(&qrtr_lock);
				continue;
			}
			qrtr_ports[p].lookups[l].fresh &= (uint8_t)~(1u << slot);
			number = qrtr_ports[p].number;
			server = qrtr_ports[p].server;
			service = qrtr_ports[p].lookups[l].service;
			instance = qrtr_ports[p].lookups[l].instance;
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			for (unsigned i = 0; i < sizeof(q->servers) / sizeof(q->servers[0]) && server != NULL; i++) {
				bool open;

				if (!qrtr_matches(q, service, instance, i)) {
					continue;
				}
				lck_mtx_lock(&q->calling);      /* (a close from another thread waits for this callback) */
				lck_mtx_lock(&qrtr_lock);
				open = qrtr_ports[p].number == number && qrtr_slot(q) >= 0 && qrtr_bind(&qrtr_ports[p], q);
				lck_mtx_unlock(&qrtr_lock);
				if (open) {
					server(context, true, q->servers[i].service, q->servers[i].instance & 0xff,
					    q->servers[i].instance >> 8, q->servers[i].node, q->servers[i].port);
				}
				lck_mtx_unlock(&q->calling);
				if (!open) {
					break;
				}
			}
		}
	}
}

/* The edge's thread, once per loop: the lookups made since the last one. */
static void
qrtr_fresh_tick(void *context)
{
	struct vayu_qrtr *q = context;

	if (qrtr_is_attached(q)) {
		q->link_thread = current_thread();
		qrtr_fresh(q);
	}
}

static void
qrtr_control(struct vayu_qrtr *q, const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	const char *who = q->glink->rp->name;
	bool ours = qrtr_is_attached(q);
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (q->hellos++ != 0) {
			break;
		}
		error = qrtr_out_packet(q, QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		say("%s: qrtr: HELLO FROM NODE %u; ours to it: %s\n", who, header->from_node, error == 0 ? "SENT" : "NOT SENT");
		if (ours) {
			for (unsigned i = 0; i < sizeof(qrtr_published) / sizeof(qrtr_published[0]); i++) {
				bool there;

				lck_mtx_lock(&qrtr_lock);
				there = qrtr_published[i].there;
				lck_mtx_unlock(&qrtr_lock);
				if (there) {
					qrtr_announce(q, i, true);
				}
			}
		}
		break;
	case QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(q->servers) / sizeof(q->servers[0]); i++) {
			if (q->servers[i].there && q->servers[i].node == control->words[2] && q->servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!q->servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			q->servers[found].there = true;
			q->servers[found].service = control->words[0];
			q->servers[found].instance = control->words[1];
			q->servers[found].node = control->words[2];
			q->servers[found].port = control->words[3];
		}
		say("%s: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", who, control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		if (ours) {
			qrtr_tell(q, control->words[0], control->words[1], control->words[2], control->words[3], true);
		}
		break;
	case QRTR_DEL_SERVER:
		say("%s: qrtr: service %u at node %u port %u has gone\n", who, control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(q->servers) / sizeof(q->servers[0]); i++) {
			bool gone = q->servers[i].there && q->servers[i].node == control->words[2] &&
			    q->servers[i].port == control->words[3];

			q->servers[i].there = q->servers[i].there && !gone;
			if (gone && ours) {
				qrtr_tell(q, q->servers[i].service, q->servers[i].instance, q->servers[i].node, q->servers[i].port, false);
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		if (q->lookups++ < 16) {
			say("%s: qrtr: it looks for service %u (%#x), instance %u: nobody here offers it\n", who, control->words[0],
			    control->words[0], control->words[1]);
		}
		break;
	case QRTR_DEL_CLIENT:
		if (qrtr_gone++ < 16) {
			say("%s: qrtr: client node %u port %u has gone\n", who, control->words[0], control->words[1]);
		}
		break;
	case QRTR_BYE:
		say("%s: qrtr: BYE from node %u\n", who, header->from_node);
		q->hellos = 0;                          /* (a restarted peer is answered again) */
		for (unsigned i = 0; i < sizeof(q->servers) / sizeof(q->servers[0]); i++) {
			bool gone = q->servers[i].there && q->servers[i].node == header->from_node;

			q->servers[i].there = q->servers[i].there && !gone;
			if (gone && ours) {
				qrtr_tell(q, q->servers[i].service, q->servers[i].instance, q->servers[i].node, q->servers[i].port, false);
			}
		}
		break;
	default:
		break;
	}
}

void
vayu_qrtr_input(void *context, const uint8_t *data, uint32_t length)
{
	struct vayu_qrtr *q = context;
	struct qrtr_header header;
	uint32_t at;

	q->link_thread = current_thread();
	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		say("%s: qrtr: %u bytes that are no datagram (they begin %#x)\n", q->glink->rp->name, length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		say("%s: qrtr: a datagram of %u bytes in a message of %u: dropped\n", q->glink->rp->name, header.size, length);
		return;
	}
	q->in++;
	if (!q->peer_known) {
		q->peer = header.from_node;
		q->peer_known = true;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(q, QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_DEL_CLIENT ||
	    header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(q, &header, &control);
		return;
	}
	if (qrtr_is_attached(q) && header.type == QRTR_DATA) {
		hakios_qrtr_receive_t receive = NULL;
		void *where = NULL;

		lck_mtx_lock(&q->calling);
		lck_mtx_lock(&qrtr_lock);
		for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]) && qrtr_slot(q) >= 0; p++) {
			if (qrtr_ports[p].number == header.to_port && qrtr_bind(&qrtr_ports[p], q)) {
				receive = qrtr_ports[p].receive;
				where = qrtr_ports[p].context;
			}
		}
		lck_mtx_unlock(&qrtr_lock);
		if (receive != NULL) {
			receive(where, header.from_node, header.from_port, data + at, header.size);
		}
		lck_mtx_unlock(&q->calling);
		if (receive != NULL) {
			return;
		}
	}
	say("%s: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", q->glink->rp->name,
	    header.size, header.from_node, header.from_port, header.to_port);
}

static struct hakios_qrtr_port *
qrtr_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	struct hakios_qrtr_port *port = NULL;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]) && port == NULL && qrtr_any_attached(); p++) {
		if (qrtr_ports[p].number == 0) {
			port = &qrtr_ports[p];
			bzero(port, sizeof(*port));
			port->number = qrtr_next_port++;
			port->receive = receive;
			port->server = server;
			port->context = context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return port;
}

static int
qrtr_send(struct hakios_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	struct vayu_qrtr *q;
	bool bound;

	if (port == NULL || port->number == 0 || length > VAYU_QRTR_MAX) {
		return EINVAL;
	}
	lck_mtx_lock(&qrtr_lock);
	q = qrtr_route(node);
	if (q == NULL) {
		q = port->router;
	}
	bound = q != NULL && qrtr_is_attached(q) && qrtr_bind(port, q);
	lck_mtx_unlock(&qrtr_lock);
	if (q == NULL || !qrtr_is_attached(q)) {
		return ENXIO;
	}
	if (!bound) {
		say("%s: qrtr: port %u speaks through %s; node %u is behind this router: not sent\n", q->glink->rp->name, port->number,
		    port->router != NULL ? port->router->glink->rp->name : "?", node);
		return EXDEV;
	}
	return qrtr_out_packet(q, QRTR_DATA, port->number, node, to, data, (uint32_t)length);
}

static int
qrtr_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	int error = ENOSPC;

	if (!qrtr_any_attached()) {
		return ENXIO;
	}
	lck_mtx_lock(&qrtr_lock);
	for (unsigned l = 0; l < 4 && error != 0 && port != NULL && port->number != 0; l++) {
		if (!port->lookups[l].asked) {
			port->lookups[l].service = service;
			port->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
			port->lookups[l].asked = true;
			port->lookups[l].fresh = (uint8_t)((1u << QRTR_ROUTERS) - 1);
			error = 0;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return error;
}

/*
 * No callback comes after this has returned. A bound port hears only from its router's thread: from any
 * other thread this waits for that router's running callback. An unbound one may hear from any router: from
 * outside every link thread this waits for all of them (in address order). From inside a callback of the
 * port's own router it must not wait. (A close from router A's callback of a port bound to router B, while
 * B's callback closes one bound to A, would wait on each other: no driver does that. Nor does any close an
 * unbound port from a link thread, where another router's thread could still bind it and call back.)
 */
static void
qrtr_close(struct hakios_qrtr_port *port)
{
	struct qrtr_control gone = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, 0 } };
	struct vayu_qrtr *held[QRTR_ROUTERS] = { NULL }, *router;
	unsigned count = 0;
	uint32_t number;

	if (port == NULL) {
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	router = port->router;
	if (router != NULL) {
		if (current_thread() != router->link_thread) {
			held[count++] = router;
		}
	} else if (!qrtr_on_link_thread()) {
		for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
			if (qrtr_routers[r] != NULL) {
				unsigned at = count++;

				while (at > 0 && (uintptr_t)held[at - 1] > (uintptr_t)qrtr_routers[r]) {
					held[at] = held[at - 1];
					at--;
				}
				held[at] = qrtr_routers[r];
			}
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	for (unsigned h = 0; h < count; h++) {
		lck_mtx_lock(&held[h]->calling);        /* (a callback of the port's that is running ends first) */
	}
	lck_mtx_lock(&qrtr_lock);
	number = port->number;
	router = port->router;
	lck_mtx_unlock(&qrtr_lock);
	if (number != 0) {
		/* The services of the port are withdrawn while its number is still the one to name. */
		for (unsigned i = 0; i < sizeof(qrtr_published) / sizeof(qrtr_published[0]); i++) {
			bool mine;

			lck_mtx_lock(&qrtr_lock);
			mine = qrtr_published[i].there && qrtr_published[i].port == port;
			lck_mtx_unlock(&qrtr_lock);
			if (mine) {
				qrtr_announce_all(i, false);
			}
		}
		lck_mtx_lock(&qrtr_lock);
		port->number = 0;
		port->router = NULL;
		for (unsigned i = 0; i < sizeof(qrtr_published) / sizeof(qrtr_published[0]); i++) {
			if (qrtr_published[i].port == port) {
				bzero(&qrtr_published[i], sizeof(qrtr_published[i]));
			}
		}
		lck_mtx_unlock(&qrtr_lock);
	}
	for (unsigned h = count; h-- > 0;) {
		lck_mtx_unlock(&held[h]->calling);
	}
	for (unsigned r = 0; r < QRTR_ROUTERS && number != 0; r++) {
		struct vayu_qrtr *q = qrtr_routers[r];

		if (q != NULL && q->peer_known && (router == NULL || router == q)) {
			gone.words[1] = number;
			qrtr_out_packet(q, QRTR_DEL_CLIENT, number, q->peer, QRTR_CONTROL, &gone, sizeof(gone));
		}
	}
}

const struct hakios_qrtr_ops vayu_qrtr_ops = {
	.open = qrtr_open,
	.send = qrtr_send,
	.lookup = qrtr_lookup,
	.close = qrtr_close,
};

void
vayu_qrtr_attach(struct vayu_qrtr *q)
{
	int slot = -1;
	bool first;

	if (q == NULL || q->glink == NULL) {
		return;
	}
	if (!q->lock_ready) {
		lck_mtx_init(&q->calling, &pas_lock_group, LCK_ATTR_NULL);
		q->lock_ready = true;
	}
	lck_mtx_lock(&qrtr_lock);
	first = !qrtr_any_attached();
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		if (qrtr_routers[r] == q) {
			slot = (int)r;
			break;
		}
		if (qrtr_routers[r] == NULL && slot < 0) {
			slot = (int)r;
		}
	}
	if (slot >= 0) {
		if (first) {
			bzero(qrtr_ports, sizeof(qrtr_ports));
			bzero(qrtr_published, sizeof(qrtr_published));
		}
		qrtr_routers[slot] = q;
	}
	lck_mtx_unlock(&qrtr_lock);
	if (slot < 0) {
		say("%s: qrtr: %u routers are attached already: not this one\n", q->glink->rp->name, QRTR_ROUTERS);
		return;
	}
	q->glink->tick = qrtr_fresh_tick;
	q->glink->tick_context = q;
	hakios_qmi_set_transport(&vayu_qrtr_ops);
	/* our services, if a peer that said HELLO before is told of them only now */
	for (unsigned i = 0; i < sizeof(qrtr_published) / sizeof(qrtr_published[0]) && !first && q->hellos != 0; i++) {
		qrtr_announce(q, i, true);
	}
}

void
vayu_qrtr_detach(struct vayu_qrtr *q)
{
	bool last;

	if (q == NULL || !qrtr_is_attached(q)) {
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	last = true;
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		last = last && (qrtr_routers[r] == NULL || qrtr_routers[r] == q);
	}
	lck_mtx_unlock(&qrtr_lock);
	if (last) {
		hakios_qmi_set_transport(NULL);        /* (first, as with one router: no new QMI client while the ports close) */
	}
	/* its ports; with the last router, the ports that never spoke through any too */
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		if (qrtr_ports[p].number != 0 && (qrtr_ports[p].router == q || (last && qrtr_ports[p].router == NULL))) {
			qrtr_close(&qrtr_ports[p]);
		}
	}
	lck_mtx_lock(&qrtr_lock);
	for (unsigned r = 0; r < QRTR_ROUTERS; r++) {
		if (qrtr_routers[r] == q) {
			qrtr_routers[r] = NULL;
		}
	}
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		if (qrtr_ports[p].router == q) {
			qrtr_ports[p].router = NULL;    /* (bound by q's thread after the scan above: free to bind again) */
		}
	}
	last = !qrtr_any_attached();
	lck_mtx_unlock(&qrtr_lock);
	/* the transport as the table now says: NULL with no router, else ours (one may have attached while this closed) */
	hakios_qmi_set_transport(last ? NULL : &vayu_qrtr_ops);
	/* a callback its thread had begun before it left the table ends before this returns (it re-checks inside) */
	if (current_thread() != q->link_thread) {
		lck_mtx_lock(&q->calling);
		lck_mtx_unlock(&q->calling);
	}
	if (q->glink != NULL) {
		q->glink->tick = NULL;
		q->glink->tick_context = NULL;
	}
}

int
vayu_qrtr_publish(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	int found = -1;

	if (!qrtr_any_attached()) {
		return ENXIO;
	}
	if (port == NULL || port->number == 0 || service == 0) {
		return EINVAL;
	}
	lck_mtx_lock(&qrtr_lock);
	for (unsigned i = 0; i < sizeof(qrtr_published) / sizeof(qrtr_published[0]); i++) {
		if (!qrtr_published[i].there) {
			found = (int)i;
			qrtr_published[i].port = port;
			qrtr_published[i].service = service;
			qrtr_published[i].instance = (version & 0xff) | instance << 8;
			qrtr_published[i].there = true;
			break;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	if (found < 0) {
		return ENOSPC;
	}
	qrtr_announce_all((unsigned)found, true);
	return 0;
}

uint32_t
vayu_qrtr_port_number(const struct hakios_qrtr_port *port)
{
	return port != NULL ? port->number : 0;
}

unsigned
vayu_qrtr_services(const struct vayu_qrtr *q)
{
	unsigned count = 0;

	for (unsigned i = 0; i < sizeof(q->servers) / sizeof(q->servers[0]); i++) {
		count += q->servers[i].there;
	}
	return count;
}

bool
vayu_qrtr_find(const struct vayu_qrtr *q, uint32_t service, uint32_t *node, uint32_t *port)
{
	for (unsigned i = 0; i < sizeof(q->servers) / sizeof(q->servers[0]); i++) {
		if (q->servers[i].there && q->servers[i].service == service) {
			*node = q->servers[i].node;
			*port = q->servers[i].port;
			return true;
		}
	}
	return false;
}
