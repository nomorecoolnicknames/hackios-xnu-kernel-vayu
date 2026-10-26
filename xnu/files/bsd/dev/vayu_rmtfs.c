/*
 * rmtfs for the POCO X3 Pro (vayu): the EFS block service the modem asks for over QRTR before its
 * firmware leaves the boot phase (QMI service 14, version 1, instance 0).
 *
 * Why this file exists.  Every Linux setup for this SoC starts the modem with rmtfs next to it
 * (rmtfs, pd-mapper, tqftpserv); without it the modem's EFS layer has no modemst1/modemst2/fsg/fsc
 * and the firmware says FATAL about 40 s after TrustZone lets it out of reset.  The same FATAL, at
 * the same place, is what this lab saw in runs 012936, 033841, 041436 and 044058, and the only
 * candidate left after the pd-mapper, the extra GLINK channels and the rails were answered or
 * rejected.  Wi-Fi's WLFW (service 0x45) and the GNSS service both live behind that point, so this
 * server is the common gate for modem, Wi-Fi and GPS.
 *
 * What it serves, and from where.  The modem opens "/boot/modem_fs1", "/boot/modem_fs2",
 * "/boot/modem_fsc" and "/boot/modem_fsg" (the paths of rmtfs's storage table).  They are the
 * phone's GPT partitions modemst1, modemst2, fsc and fsg.  The kernel already has a read-only block
 * path to them (bsd/dev/hakios.ufs.c, ufs=2 to the loader): pages are read on demand through that
 * path and kept in RAM.  A write from the modem is applied to the RAM page only; nothing is ever
 * written to the phone's storage, so an EFS image cannot be corrupted and no identifier can be
 * changed by this server.  Every read made after a write sees the write (the page is the same).
 *
 * The shared buffer.  The protocol has the modem read and write sectors through a physical buffer
 * whose address the host returns in ALLOC_BUFF.  A 3 MB buffer is allocated as contiguous kernel
 * memory, mapped write-combined for the AP, and given to the modem's VMID by TrustZone's
 * MEM_PROT_ASSIGN call (the call and the two permission entries are the vendor
 * msm_sharedmem.c:setup_shared_ram_perms path: source VMID 3, destinations VMID 3 and 15, both
 * read/write).  If TrustZone refuses, ALLOC_BUFF is answered with an error and the modem is never
 * handed an address it cannot use.
 *
 * Boot args:
 *   vayu_rmtfs=0   nothing (the default)
 *   vayu_rmtfs=1   publish service 14 and log every request; answers nothing (the safe first look)
 *   vayu_rmtfs=2   the server above
 *
 * Nothing here logs bytes of an EFS image, an identifier, or a path the modem did not ask for.
 * The first few requests are logged with their sizes and sectors only; after that only totals.
 *
 * Facts:
 *   - QMI service 14, version 1, instance 0: rmtfs_rmtfs.c:24-26.
 *   - Messages and structures: qmi_rmtfs.h (OPEN 1, CLOSE 2, RW_IOVEC 3, ALLOC_BUFF 4,
 *     GET_DEV_ERROR 5); the sector size is 512 (rmtfs_rmtfs.h:7).
 *   - Paths and their partitions: rmtfs_storage.c partition_table.
 *   - Buffer size and the TrustZone permission pair: vendor drivers/uio/msm_sharedmem/
 *     msm_sharedmem.c:83-92 and the stock DT qcom,rmtfs_sharedmem@0 (reg 0x300000).
 *   - The UFS names are the GPT names of the phone (a backup of the phone's own Android, receipt
 *     2026-10-23-vayu-backup-fw-hwruns.md).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.h>
#include <sys/systm.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_rmtfs.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void loader_log_putc(char c);

#define VAYU_RMTFS_SERVICE      14
#define VAYU_RMTFS_VERSION      1
#define VAYU_RMTFS_INSTANCE     0

#define VAYU_RMTFS_SECTOR       512
#define VAYU_RMTFS_ALIGN        4096
#define VAYU_RMTFS_BUFFER_SIZE  0x300000        /* stock DT qcom,rmtfs_sharedmem@0: 3 MB */
#define VAYU_RMTFS_CALLERS      16
#define VAYU_RMTFS_PARTS        4
#define VAYU_RMTFS_LOG_REQUESTS 8

struct vayu_rmtfs_result {
	uint16_t result;
	uint16_t error;
};

struct vayu_rmtfs_open_req {
	char path[256];
};

struct vayu_rmtfs_open_resp {
	struct vayu_rmtfs_result result;
	uint8_t caller_id_valid;
	uint32_t caller_id;
};

struct vayu_rmtfs_close_req {
	uint32_t caller_id;
};

struct vayu_rmtfs_close_resp {
	struct vayu_rmtfs_result result;
};

struct vayu_rmtfs_iovec_entry {
	uint32_t sector_addr;
	uint32_t phys_offset;
	uint32_t num_sector;
};

struct vayu_rmtfs_iovec_req {
	uint32_t caller_id;
	uint8_t direction;
	uint32_t iovec_len;
	struct vayu_rmtfs_iovec_entry iovec[255];
	uint8_t is_force_sync;
};

struct vayu_rmtfs_iovec_resp {
	struct vayu_rmtfs_result result;
};

struct vayu_rmtfs_alloc_req {
	uint32_t caller_id;
	uint32_t buff_size;
};

struct vayu_rmtfs_alloc_resp {
	struct vayu_rmtfs_result result;
	uint8_t buff_address_valid;
	uint64_t buff_address;
};

struct vayu_rmtfs_dev_error_req {
	uint32_t caller_id;
};

struct vayu_rmtfs_dev_error_resp {
	struct vayu_rmtfs_result result;
	uint8_t status_valid;
	uint8_t status;
};

/* ---- element tables ------------------------------------------------------------------------------ */

static const struct qmi_elem_info vayu_rmtfs_result_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t),
	  .array_type = NO_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE,
	  .offset = offsetof(struct vayu_rmtfs_result, result) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t),
	  .array_type = NO_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE,
	  .offset = offsetof(struct vayu_rmtfs_result, error) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_open_req_ei[] = {
	{ .data_type = QMI_STRING, .elem_len = 256, .elem_size = sizeof(char), .array_type = NO_ARRAY,
	  .tlv_type = 1, .offset = offsetof(struct vayu_rmtfs_open_req, path) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_open_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_rmtfs_result),
	  .array_type = NO_ARRAY, .tlv_type = 2, .offset = offsetof(struct vayu_rmtfs_open_resp, result),
	  .ei_array = vayu_rmtfs_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 16, .offset = offsetof(struct vayu_rmtfs_open_resp, caller_id_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 16, .offset = offsetof(struct vayu_rmtfs_open_resp, caller_id) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_close_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 1, .offset = offsetof(struct vayu_rmtfs_close_req, caller_id) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_close_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_rmtfs_result),
	  .array_type = NO_ARRAY, .tlv_type = 2, .offset = offsetof(struct vayu_rmtfs_close_resp, result),
	  .ei_array = vayu_rmtfs_result_ei },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_iovec_entry_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_rmtfs_iovec_entry, sector_addr) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_rmtfs_iovec_entry, phys_offset) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_rmtfs_iovec_entry, num_sector) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_iovec_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 1, .offset = offsetof(struct vayu_rmtfs_iovec_req, caller_id) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 2, .offset = offsetof(struct vayu_rmtfs_iovec_req, direction) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 3, .offset = offsetof(struct vayu_rmtfs_iovec_req, iovec_len) },
	{ .data_type = QMI_STRUCT, .elem_len = 255, .elem_size = sizeof(struct vayu_rmtfs_iovec_entry),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 3, .offset = offsetof(struct vayu_rmtfs_iovec_req, iovec),
	  .ei_array = vayu_rmtfs_iovec_entry_ei },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 4, .offset = offsetof(struct vayu_rmtfs_iovec_req, is_force_sync) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_iovec_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_rmtfs_result),
	  .array_type = NO_ARRAY, .tlv_type = 2, .offset = offsetof(struct vayu_rmtfs_iovec_resp, result),
	  .ei_array = vayu_rmtfs_result_ei },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_alloc_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 1, .offset = offsetof(struct vayu_rmtfs_alloc_req, caller_id) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 2, .offset = offsetof(struct vayu_rmtfs_alloc_req, buff_size) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_alloc_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_rmtfs_result),
	  .array_type = NO_ARRAY, .tlv_type = 2, .offset = offsetof(struct vayu_rmtfs_alloc_resp, result),
	  .ei_array = vayu_rmtfs_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 16, .offset = offsetof(struct vayu_rmtfs_alloc_resp, buff_address_valid) },
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(uint64_t), .array_type = NO_ARRAY,
	  .tlv_type = 16, .offset = offsetof(struct vayu_rmtfs_alloc_resp, buff_address) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_dev_error_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 1, .offset = offsetof(struct vayu_rmtfs_dev_error_req, caller_id) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_rmtfs_dev_error_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_rmtfs_result),
	  .array_type = NO_ARRAY, .tlv_type = 2, .offset = offsetof(struct vayu_rmtfs_dev_error_resp, result),
	  .ei_array = vayu_rmtfs_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 16, .offset = offsetof(struct vayu_rmtfs_dev_error_resp, status_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 16, .offset = offsetof(struct vayu_rmtfs_dev_error_resp, status) },
	{ .data_type = QMI_EOTI },
};

/* ---- state --------------------------------------------------------------------------------------- */

static LCK_GRP_DECLARE(vayu_rmtfs_lck_grp, "vayu-rmtfs");
static LCK_MTX_DECLARE(vayu_rmtfs_lock, &vayu_rmtfs_lck_grp);

struct vayu_rmtfs_part {
	const char *path;               /* what the modem asks for */
	const char *name;               /* the GPT name on the phone */
	uint32_t ufs_part;              /* index in the loader's table */
	uint64_t bytes;
	bool ufs_found;
	bool read_failed;
	uint8_t **page;                 /* PAGE_SIZE pages, made when first touched */
	uint32_t page_count;
};

static struct vayu_rmtfs_part rmtfs_parts[VAYU_RMTFS_PARTS] = {
	{ .path = "/boot/modem_fs1", .name = "modemst1" },
	{ .path = "/boot/modem_fs2", .name = "modemst2" },
	{ .path = "/boot/modem_fsc", .name = "fsc" },
	{ .path = "/boot/modem_fsg", .name = "fsg" },
};

struct vayu_rmtfs_caller {
	bool used;
	uint32_t node;
	uint32_t caller_id;
	int part;
};

static struct vayu_rmtfs_caller rmtfs_callers[VAYU_RMTFS_CALLERS];

static uint32_t rmtfs_level;
static bool rmtfs_started, rmtfs_stopping;
static uint32_t rmtfs_busy;
static struct hakios_qrtr_port *rmtfs_port;

static volatile uint8_t *rmtfs_shared;
static uint64_t rmtfs_shared_pa;
static bool rmtfs_shared_ready;
static bool rmtfs_buffer_allocated;   /* the 3 MB contiguous buffer has been taken and mapped */
static uint64_t rmtfs_alloc_next;

static uint32_t rmtfs_opens, rmtfs_allocs, rmtfs_iovecs, rmtfs_reads, rmtfs_writes, rmtfs_unknown;
static uint64_t rmtfs_sectors_in, rmtfs_sectors_out, rmtfs_bytes_in, rmtfs_bytes_out;
static uint32_t rmtfs_logged;

static struct vayu_rmtfs_iovec_req rmtfs_request;
static uint8_t rmtfs_answer[256];

/* Every line to the loader's log as well: what is done to the modem should be known even if the kernel dies. */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return length;
}

/* ---- the EFS pages ------------------------------------------------------------------------------- */

static uint8_t *
rmtfs_page(struct vayu_rmtfs_part *part, uint64_t offset)
{
	uint64_t index = offset / PAGE_SIZE;
	uint8_t *page;

	if (part->page == NULL || index >= part->page_count) {
		return NULL;
	}
	page = part->page[index];
	if (page != NULL) {
		return page;
	}
	page = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_ZERO);
	if (page == NULL) {
		return NULL;
	}
	if (part->ufs_found) {
		uint64_t at = index * PAGE_SIZE;
		uint64_t length = part->bytes > at ? part->bytes - at : 0;

		if (length > PAGE_SIZE) {
			length = PAGE_SIZE;
		}
		if (length != 0 && hakios_ufs_part_read(part->ufs_part, at, page, length) != 0) {
			if (!part->read_failed) {
				part->read_failed = true;
				say("vayu-rmtfs: %s: the storage read failed at %llu; zeros are served\n", part->name, at);
			}
			memset(page, 0, PAGE_SIZE);
		}
	}
	part->page[index] = page;
	return page;
}

static bool
rmtfs_read_sector(struct vayu_rmtfs_part *part, uint64_t sector, uint8_t *to)
{
	uint64_t offset = sector * VAYU_RMTFS_SECTOR;
	uint8_t *page;

	if (part->bytes != 0 && (offset > part->bytes || VAYU_RMTFS_SECTOR > part->bytes - offset)) {
		return false;
	}
	page = rmtfs_page(part, offset);
	if (page == NULL) {
		memset(to, 0, VAYU_RMTFS_SECTOR);       /* no storage: a blank EFS is better than a failed request */
		return true;
	}
	memcpy(to, page + offset % PAGE_SIZE, VAYU_RMTFS_SECTOR);
	return true;
}

static bool
rmtfs_write_sector(struct vayu_rmtfs_part *part, uint64_t sector, const uint8_t *from)
{
	uint64_t offset = sector * VAYU_RMTFS_SECTOR;
	uint8_t *page;

	if (part->bytes != 0 && (offset > part->bytes || VAYU_RMTFS_SECTOR > part->bytes - offset)) {
		return false;
	}
	page = rmtfs_page(part, offset);
	if (page == NULL) {
		return true;                            /* nowhere to keep it: the write is dropped, as always */
	}
	memcpy(page + offset % PAGE_SIZE, from, VAYU_RMTFS_SECTOR);
	return true;
}

/* ---- the shared buffer --------------------------------------------------------------------------- */

static bool
rmtfs_buffer_setup(void)
{
	if (rmtfs_shared_ready) {
		return true;
	}
	if (!rmtfs_buffer_allocated) {
		vm_offset_t memory = 0;
		vm_size_t size = VAYU_RMTFS_BUFFER_SIZE;
		kern_return_t kr;
		volatile uint8_t *mapped;

		/*
		 * The QMI iovec's phys_offset is an offset into this buffer, not an
		 * absolute physical address (see rmtfs_iovec(): it adds the sector
		 * offset and indexes rmtfs_shared).  All usable DRAM on vayu is above
		 * 4 GB, so any below-4-GB request is wrong; the stock Android rmtfs
		 * buffer is above 4 GB too.
		 */
		kr = kmem_alloc_contig(kernel_map, &memory, size, PAGE_MASK, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
		if (kr != KERN_SUCCESS || memory == 0) {
			say("vayu-rmtfs: no %u KB of contiguous memory for the shared buffer (%d)\n",
			    VAYU_RMTFS_BUFFER_SIZE >> 10, kr);
			return false;
		}
		FlushPoC_DcacheRegion(memory, size);
		rmtfs_shared_pa = kvtophys(memory);
		mapped = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)rmtfs_shared_pa, size, VM_WIMG_WCOMB);
		if (mapped == NULL) {
			say("vayu-rmtfs: the shared buffer at %#llx cannot be mapped write-combined\n", rmtfs_shared_pa);
			return false;
		}
		memset((void *)mapped, 0, size);
		rmtfs_shared = mapped;
		rmtfs_buffer_allocated = true;
	}
	int ret = vayu_scm_assign_shared(rmtfs_shared_pa, VAYU_RMTFS_BUFFER_SIZE, 15);
	if (ret != 0) {
		say("vayu-rmtfs: TrustZone did not give the buffer at %#llx to the modem (%d); ALLOC is refused\n",
		    rmtfs_shared_pa, ret);
		return false;
	}
	rmtfs_shared_ready = true;
	say("vayu-rmtfs: shared buffer %#llx, %u KB, mapped write-combined, given to VMIDs 3 and 15 read/write\n",
	    rmtfs_shared_pa, VAYU_RMTFS_BUFFER_SIZE >> 10);
	return true;
}

/* Called early from vayu_modem_init(): take the 3 MB before the heap fragments. */
void
vayu_rmtfs_prepare(void)
{
	uint32_t level = 0;

	if (!PE_parse_boot_argn("vayu_rmtfs", &level, sizeof(level)) || level < 2 || rmtfs_shared_ready) {
		return;
	}
	say("vayu-rmtfs: early buffer allocation (vayu_rmtfs=%u)\n", level);
	(void)rmtfs_buffer_setup();
}

/* ---- the callers --------------------------------------------------------------------------------- */

static struct vayu_rmtfs_caller *
rmtfs_caller(uint32_t caller_id)
{
	for (unsigned int i = 0; i < VAYU_RMTFS_CALLERS; i++) {
		if (rmtfs_callers[i].used && rmtfs_callers[i].caller_id == caller_id) {
			return &rmtfs_callers[i];
		}
	}
	return NULL;
}

static int
rmtfs_part_number(const char *path)
{
	for (unsigned int i = 0; i < VAYU_RMTFS_PARTS; i++) {
		if (strcmp(rmtfs_parts[i].path, path) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static void
rmtfs_send(uint32_t node, uint32_t to_port, uint16_t message, uint16_t transaction,
    const struct qmi_elem_info *ei, const void *structure)
{
	int length;

	length = hakios_qmi_encode(QMI_RESPONSE, message, transaction, ei, structure, rmtfs_answer, sizeof(rmtfs_answer));
	if (length < 0) {
		say("vayu-rmtfs: message %u: cannot be encoded (%d)\n", message, length);
		return;
	}
	if (vayu_qrtr_ops.send(rmtfs_port, node, to_port, rmtfs_answer, (size_t)length) != 0) {
		say("vayu-rmtfs: message %u: cannot be sent\n", message);
	}
}

static void
rmtfs_fail(uint32_t node, uint32_t to_port, uint16_t message, uint16_t transaction, uint16_t error)
{
	struct vayu_rmtfs_result result = { .result = 1, .error = error };

	switch (message) {
	case 1: {
		struct vayu_rmtfs_open_resp resp = { .result = result };
		rmtfs_send(node, to_port, message, transaction, vayu_rmtfs_open_resp_ei, &resp);
		break;
	}
	case 2: {
		struct vayu_rmtfs_close_resp resp = { .result = result };
		rmtfs_send(node, to_port, message, transaction, vayu_rmtfs_close_resp_ei, &resp);
		break;
	}
	case 3: {
		struct vayu_rmtfs_iovec_resp resp = { .result = result };
		rmtfs_send(node, to_port, message, transaction, vayu_rmtfs_iovec_resp_ei, &resp);
		break;
	}
	case 4: {
		struct vayu_rmtfs_alloc_resp resp = { .result = result };
		rmtfs_send(node, to_port, message, transaction, vayu_rmtfs_alloc_resp_ei, &resp);
		break;
	}
	case 5: {
		struct vayu_rmtfs_dev_error_resp resp = { .result = result };
		rmtfs_send(node, to_port, message, transaction, vayu_rmtfs_dev_error_resp_ei, &resp);
		break;
	}
	}
}

/* ---- the requests --------------------------------------------------------------------------------- */

static void
rmtfs_open(uint32_t node, uint32_t from_port, uint16_t transaction, const uint8_t *message, size_t length)
{
	struct vayu_rmtfs_open_req req = { 0 };
	struct vayu_rmtfs_open_resp resp = { 0 };
	struct vayu_rmtfs_caller *caller = NULL;
	int part;

	if (hakios_qmi_decode(message, length, vayu_rmtfs_open_req_ei, &req) < 0) {
		say("vayu-rmtfs: OPEN: a message that does not decode\n");
		rmtfs_fail(node, from_port, 1, transaction, 2);
		return;
	}
	req.path[255] = 0;
	part = rmtfs_part_number(req.path);
	if (part < 0) {
		say("vayu-rmtfs: OPEN: \"%s\" is not a partition we serve\n", req.path);
		rmtfs_fail(node, from_port, 1, transaction, 1);
		return;
	}
	for (unsigned int i = 0; i < VAYU_RMTFS_CALLERS; i++) {
		if (!rmtfs_callers[i].used) {
			caller = &rmtfs_callers[i];
			caller->caller_id = i + 1;
			break;
		}
	}
	if (caller == NULL) {
		say("vayu-rmtfs: OPEN: no free caller\n");
		rmtfs_fail(node, from_port, 1, transaction, 1);
		return;
	}
	caller->used = true;
	caller->node = node;
	caller->part = part;
	rmtfs_opens++;
	resp.result.result = 0;
	resp.result.error = 0;
	resp.caller_id_valid = 1;
	resp.caller_id = caller->caller_id;
	say("vayu-rmtfs: OPEN \"%s\" from node %u port %u -> caller %u (%s, %llu bytes%s)\n", req.path, node,
	    from_port, caller->caller_id, rmtfs_parts[part].name,
	    (unsigned long long)rmtfs_parts[part].bytes,
	    rmtfs_parts[part].ufs_found ? "" : ", NOT on the storage");
	rmtfs_send(node, from_port, 1, transaction, vayu_rmtfs_open_resp_ei, &resp);
}

static void
rmtfs_close(uint32_t node, uint32_t from_port, uint16_t transaction, const uint8_t *message, size_t length)
{
	struct vayu_rmtfs_close_req req = { 0 };
	struct vayu_rmtfs_close_resp resp = { 0 };
	struct vayu_rmtfs_caller *caller;

	if (hakios_qmi_decode(message, length, vayu_rmtfs_close_req_ei, &req) < 0) {
		rmtfs_fail(node, from_port, 2, transaction, 2);
		return;
	}
	caller = rmtfs_caller(req.caller_id);
	if (caller == NULL || caller->node != node) {
		rmtfs_fail(node, from_port, 2, transaction, 1);
		return;
	}
	caller->used = false;
	resp.result.result = 0;
	resp.result.error = 0;
	rmtfs_send(node, from_port, 2, transaction, vayu_rmtfs_close_resp_ei, &resp);
}

static void
rmtfs_alloc(uint32_t node, uint32_t from_port, uint16_t transaction, const uint8_t *message, size_t length)
{
	struct vayu_rmtfs_alloc_req req = { 0 };
	struct vayu_rmtfs_alloc_resp resp = { 0 };
	struct vayu_rmtfs_caller *caller;
	uint64_t size, at;

	if (hakios_qmi_decode(message, length, vayu_rmtfs_alloc_req_ei, &req) < 0) {
		rmtfs_fail(node, from_port, 4, transaction, 2);
		return;
	}
	caller = rmtfs_caller(req.caller_id);
	if (caller == NULL || caller->node != node) {
		rmtfs_fail(node, from_port, 4, transaction, 1);
		return;
	}
	if (!rmtfs_shared_ready) {
		rmtfs_fail(node, from_port, 4, transaction, 1);
		return;
	}
	size = (req.buff_size + VAYU_RMTFS_ALIGN - 1) & ~(uint64_t)(VAYU_RMTFS_ALIGN - 1);
	if (size == 0) {
		size = VAYU_RMTFS_ALIGN;
	}
	if (size > VAYU_RMTFS_BUFFER_SIZE || rmtfs_alloc_next > VAYU_RMTFS_BUFFER_SIZE - size) {
		say("vayu-rmtfs: ALLOC: %u bytes do not fit (used %llu of %u)\n", req.buff_size,
		    (unsigned long long)rmtfs_alloc_next, VAYU_RMTFS_BUFFER_SIZE);
		rmtfs_fail(node, from_port, 4, transaction, 1);
		return;
	}
	at = rmtfs_alloc_next;
	rmtfs_alloc_next += size;
	resp.result.result = 0;
	resp.result.error = 0;
	resp.buff_address_valid = 1;
	resp.buff_address = rmtfs_shared_pa + at;
	rmtfs_allocs++;
	say("vayu-rmtfs: ALLOC caller %u, %u bytes -> %#llx (of %u KB)\n", req.caller_id, req.buff_size,
	    (unsigned long long)resp.buff_address, VAYU_RMTFS_BUFFER_SIZE >> 10);
	rmtfs_send(node, from_port, 4, transaction, vayu_rmtfs_alloc_resp_ei, &resp);
}

static void
rmtfs_iovec(uint32_t node, uint32_t from_port, uint16_t transaction, const uint8_t *message, size_t length)
{
	struct vayu_rmtfs_iovec_resp resp = { 0 };
	struct vayu_rmtfs_caller *caller;
	uint8_t sector[VAYU_RMTFS_SECTOR];
	uint8_t *buffer = (uint8_t *)rmtfs_shared;
	bool write;
	uint32_t entries, sectors = 0;
	int decoded;

	memset(&rmtfs_request, 0, sizeof(rmtfs_request));
	decoded = hakios_qmi_decode(message, length, vayu_rmtfs_iovec_req_ei, &rmtfs_request);
	if (decoded < 0) {
		say("vayu-rmtfs: RW_IOVEC: a message that does not decode (%d)\n", decoded);
		rmtfs_fail(node, from_port, 3, transaction, 2);
		return;
	}
	caller = rmtfs_caller(rmtfs_request.caller_id);
	if (caller == NULL || caller->node != node) {
		rmtfs_fail(node, from_port, 3, transaction, 1);
		return;
	}
	if (!rmtfs_shared_ready || rmtfs_request.iovec_len == 0 || rmtfs_request.iovec_len > 255) {
		rmtfs_fail(node, from_port, 3, transaction, 1);
		return;
	}
	write = rmtfs_request.direction != 0;
	entries = rmtfs_request.iovec_len;
	if (write) {
		rmtfs_writes++;
	} else {
		rmtfs_reads++;
	}
	for (uint32_t i = 0; i < entries; i++) {
		struct vayu_rmtfs_iovec_entry *entry = &rmtfs_request.iovec[i];

		if (entry->num_sector > 0x10000) {
			resp.result.result = 1;
			resp.result.error = 1;
			break;
		}
		for (uint32_t j = 0; j < entry->num_sector; j++) {
			uint64_t physical = (uint64_t)entry->phys_offset + (uint64_t)j * VAYU_RMTFS_SECTOR;
			uint64_t offset;

			if (physical >= rmtfs_shared_pa && physical - rmtfs_shared_pa <= VAYU_RMTFS_BUFFER_SIZE - VAYU_RMTFS_SECTOR) {
				offset = physical - rmtfs_shared_pa;    /* a full 64-bit address */
			} else if (physical <= VAYU_RMTFS_BUFFER_SIZE - VAYU_RMTFS_SECTOR) {
				offset = physical;      /* some firmware passes the offset from the buffer's base */
			} else {
				/*
				 * The modem's iovec field is 32 bits and carries the low half of the
				 * 64-bit buffer address (log 191124: base 0x19c6ac000, the first
				 * request says 0x9c6ac200).  Rebuild the high half from the buffer.
				 */
				uint64_t low = physical & 0xffffffffULL;
				uint64_t carry = physical >> 32;
				uint64_t full = (rmtfs_shared_pa & ~0xffffffffULL) + (carry << 32) + low;

				if (full >= rmtfs_shared_pa && full - rmtfs_shared_pa <= VAYU_RMTFS_BUFFER_SIZE - VAYU_RMTFS_SECTOR) {
					offset = full - rmtfs_shared_pa;
				} else {
					say("vayu-rmtfs: RW_IOVEC: physical %#llx (full %#llx) is outside the buffer\n",
					    (unsigned long long)physical, (unsigned long long)full);
					resp.result.result = 1;
					resp.result.error = 1;
					break;
				}
			}
			if (write) {
				memcpy(sector, buffer + offset, VAYU_RMTFS_SECTOR);
				if (!rmtfs_write_sector(&rmtfs_parts[caller->part], entry->sector_addr + j, sector)) {
					resp.result.result = 1;
					resp.result.error = 1;
					break;
				}
			} else {
				if (!rmtfs_read_sector(&rmtfs_parts[caller->part], entry->sector_addr + j, sector)) {
					resp.result.result = 1;
					resp.result.error = 1;
					break;
				}
				memcpy(buffer + offset, sector, VAYU_RMTFS_SECTOR);
			}
			sectors++;
		}
		if (resp.result.result != 0) {
			break;
		}
	}
	if (write) {
		rmtfs_sectors_out += sectors;
		rmtfs_bytes_out += (uint64_t)sectors * VAYU_RMTFS_SECTOR;
	} else {
		rmtfs_sectors_in += sectors;
		rmtfs_bytes_in += (uint64_t)sectors * VAYU_RMTFS_SECTOR;
	}
	rmtfs_iovecs++;
	if (rmtfs_logged < VAYU_RMTFS_LOG_REQUESTS) {
		rmtfs_logged++;
		say("vayu-rmtfs: RW_IOVEC %s caller %u: %u entr%s, %u sector(s), first %u@%#x phys %#x%s\n",
		    write ? "write" : "read", rmtfs_request.caller_id, entries, entries == 1 ? "y" : "ies",
		    sectors, entries != 0 ? rmtfs_request.iovec[0].num_sector : 0,
		    entries != 0 ? rmtfs_request.iovec[0].sector_addr : 0,
		    entries != 0 ? rmtfs_request.iovec[0].phys_offset : 0,
		    resp.result.result != 0 ? " (FAILED)" : "");
	}
	rmtfs_send(node, from_port, 3, transaction, vayu_rmtfs_iovec_resp_ei, &resp);
}

static void
rmtfs_dev_error(uint32_t node, uint32_t from_port, uint16_t transaction, const uint8_t *message, size_t length)
{
	struct vayu_rmtfs_dev_error_req req = { 0 };
	struct vayu_rmtfs_dev_error_resp resp = { 0 };
	struct vayu_rmtfs_caller *caller;

	if (hakios_qmi_decode(message, length, vayu_rmtfs_dev_error_req_ei, &req) < 0) {
		rmtfs_fail(node, from_port, 5, transaction, 2);
		return;
	}
	caller = rmtfs_caller(req.caller_id);
	if (caller == NULL || caller->node != node) {
		rmtfs_fail(node, from_port, 5, transaction, 1);
		return;
	}
	resp.result.result = 0;
	resp.result.error = 0;
	resp.status_valid = 1;
	resp.status = 0;                        /* no error: the RAM shadow has no device to fail */
	rmtfs_send(node, from_port, 5, transaction, vayu_rmtfs_dev_error_resp_ei, &resp);
}

static void
rmtfs_receive(void *context __unused, uint32_t node, uint32_t from_port, const void *data, size_t length)
{
	const uint8_t *message = data;
	uint16_t transaction, id;

	lck_mtx_lock(&vayu_rmtfs_lock);
	if (rmtfs_stopping || rmtfs_port == NULL) {
		lck_mtx_unlock(&vayu_rmtfs_lock);
		return;
	}
	rmtfs_busy++;
	lck_mtx_unlock(&vayu_rmtfs_lock);

	if (length < QMI_HEADER_SIZE || message[0] != QMI_REQUEST) {
		goto out;
	}
	transaction = (uint16_t)(message[1] | (uint16_t)message[2] << 8);
	id = (uint16_t)(message[3] | (uint16_t)message[4] << 8);
	if (rmtfs_level < 2) {
		if (rmtfs_logged < VAYU_RMTFS_LOG_REQUESTS) {
			rmtfs_logged++;
			say("vayu-rmtfs: listener: message %u, %llu bytes, from node %u port %u; not answered\n", id,
			    (unsigned long long)length, node, from_port);
		}
		goto out;
	}
	switch (id) {
	case 1:
		rmtfs_open(node, from_port, transaction, message, length);
		break;
	case 2:
		rmtfs_close(node, from_port, transaction, message, length);
		break;
	case 3:
		rmtfs_iovec(node, from_port, transaction, message, length);
		break;
	case 4:
		rmtfs_alloc(node, from_port, transaction, message, length);
		break;
	case 5:
		rmtfs_dev_error(node, from_port, transaction, message, length);
		break;
	default:
		rmtfs_unknown++;
		if (rmtfs_unknown <= VAYU_RMTFS_LOG_REQUESTS) {
			say("vayu-rmtfs: message %u is not one of rmtfs's\n", id);
		}
		break;
	}
out:
	lck_mtx_lock(&vayu_rmtfs_lock);
	rmtfs_busy--;
	lck_mtx_unlock(&vayu_rmtfs_lock);
}

/* ---- start and stop ------------------------------------------------------------------------------ */

static void
rmtfs_parts_setup(void)
{
	for (unsigned int i = 0; i < VAYU_RMTFS_PARTS; i++) {
		struct vayu_rmtfs_part *part = &rmtfs_parts[i];
		uint32_t index;

		part->ufs_found = hakios_ufs_part(part->name, &index, &part->bytes);
		if (!part->ufs_found) {
			say("vayu-rmtfs: %s (%s) is not in the storage table; reads will be zeros\n", part->name, part->path);
			continue;
		}
		part->ufs_part = index;
		part->page_count = (uint32_t)((part->bytes + PAGE_SIZE - 1) / PAGE_SIZE);
		part->page = kalloc_data((size_t)part->page_count * sizeof(uint8_t *), Z_WAITOK | Z_ZERO);
		if (part->page == NULL) {
			say("vayu-rmtfs: %s: no memory for its page table\n", part->name);
			part->ufs_found = false;
			part->page_count = 0;
			continue;
		}
		say("vayu-rmtfs: %s -> %s, %llu bytes, read through the kernel's UFS path\n", part->path, part->name,
		    (unsigned long long)part->bytes);
	}
}

void
vayu_rmtfs_start(struct vayu_qrtr *q)
{
	int ret;

	(void)q;                                /* the QMI helpers' transport is already attached to it */
	if (rmtfs_started) {
		say("vayu-rmtfs: already started\n");
		return;
	}
	if (!PE_parse_boot_argn("vayu_rmtfs", &rmtfs_level, sizeof(rmtfs_level)) || rmtfs_level == 0) {
		return;
	}
	if (rmtfs_level > 2) {
		rmtfs_level = 2;
	}
	say("vayu-rmtfs: vayu_rmtfs=%u\n", rmtfs_level);
	rmtfs_started = true;
	rmtfs_stopping = false;
	rmtfs_alloc_next = 0;
	rmtfs_logged = 0;
	rmtfs_parts_setup();
	if (rmtfs_level >= 2 && !rmtfs_shared_ready) {
		(void)rmtfs_buffer_setup();
	}
	rmtfs_port = vayu_qrtr_ops.open(rmtfs_receive, NULL, NULL);
	if (rmtfs_port == NULL) {
		say("vayu-rmtfs: no port\n");
		return;
	}
	ret = vayu_qrtr_publish(rmtfs_port, VAYU_RMTFS_SERVICE, VAYU_RMTFS_VERSION, VAYU_RMTFS_INSTANCE);
	if (ret != 0) {
		say("vayu-rmtfs: cannot publish service 14: %d\n", ret);
		vayu_qrtr_ops.close(rmtfs_port);
		rmtfs_port = NULL;
		return;
	}
	say("vayu-rmtfs: service 14 version 1 instance 0 at port %u\n", vayu_qrtr_port_number(rmtfs_port));
}

void
vayu_rmtfs_stop(void)
{
	struct hakios_qrtr_port *port;
	uint32_t opens, allocs, iovecs, reads, writes, unknown;
	uint64_t in, out, bytes_in, bytes_out;

	if (!rmtfs_started) {
		return;
	}
	lck_mtx_lock(&vayu_rmtfs_lock);
	rmtfs_stopping = true;
	lck_mtx_unlock(&vayu_rmtfs_lock);

	for (uint32_t waited = 0; waited < 1000; waited += 10) {
		bool busy;

		lck_mtx_lock(&vayu_rmtfs_lock);
		busy = rmtfs_busy != 0;
		lck_mtx_unlock(&vayu_rmtfs_lock);
		if (!busy) {
			break;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	lck_mtx_lock(&vayu_rmtfs_lock);
	port = rmtfs_port;
	rmtfs_port = NULL;
	opens = rmtfs_opens;
	allocs = rmtfs_allocs;
	iovecs = rmtfs_iovecs;
	reads = rmtfs_reads;
	writes = rmtfs_writes;
	unknown = rmtfs_unknown;
	in = rmtfs_sectors_in;
	out = rmtfs_sectors_out;
	bytes_in = rmtfs_bytes_in;
	bytes_out = rmtfs_bytes_out;
	lck_mtx_unlock(&vayu_rmtfs_lock);

	if (port != NULL) {
		vayu_qrtr_ops.close(port);
	}
	say("vayu-rmtfs: done: %u open(s), %u alloc(s), %u iovec(s) (%u read, %u write), %llu sectors in, "
	    "%llu out (%llu bytes read, %llu written and kept in RAM), %u unknown message(s)\n", opens, allocs,
	    iovecs, reads, writes, (unsigned long long)in, (unsigned long long)out,
	    (unsigned long long)bytes_in, (unsigned long long)bytes_out, unknown);
	rmtfs_started = false;
}
