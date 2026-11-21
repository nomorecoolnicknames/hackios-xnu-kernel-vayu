/*
 * Other UEFI applications started from this loader: see efi_chainload.h. The way it is done is the UEFI boot
 * manager's (UEFI 2.10 sections 3.1 and 7.4; edk2 MdeModulePkg UefiBootManagerLib BmBoot.c EfiBootManagerBoot): the
 * image's whole device path -- the volume's, then a file-path node -- given to LoadImage, so that the started
 * application finds its own volume in LoadedImage->DeviceHandle (Windows' boot manager reads its BCD from there,
 * systemd-boot its entries), the watchdog armed for 5 minutes, StartImage. Unlike that boot manager, what comes
 * back is not followed by the next boot option: the phone is reset.
 */
#include "efi_chainload.h"

void *memset(void *dst, int c, size_t n);       /* loader.c's */
void *memcpy(void *dst, const void *src, size_t n);

static const efi_guid sfs_guid = EFI_SIMPLE_FS_GUID;
static const efi_guid file_info_guid = EFI_FILE_INFO_GUID;
static const efi_guid loaded_image_guid = EFI_LOADED_IMAGE_GUID;
static const efi_guid device_path_guid = EFI_DEVICE_PATH_GUID;
static const efi_guid partition_info_guid = EFI_PARTITION_INFO_GUID;
static const efi_guid block_io_guid = EFI_BLOCK_IO_GUID;

/* one file's header, a directory's entry, entries.cfg: one at a time */
static uint8_t scratch[4096] __attribute__((aligned(8)));

static int
guid_eq(const efi_guid *a, const efi_guid *b)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;

	for (unsigned i = 0; i < sizeof(efi_guid); i++) {
		if (x[i] != y[i]) {
			return 0;
		}
	}
	return 1;
}

static void *
config_table(const efi_system_table *st, const efi_guid *g)
{
	for (uint64_t i = 0; i < st->nr_tables; i++) {
		if (guid_eq(&st->tables[i].guid, g)) {
			return st->tables[i].table;
		}
	}
	return NULL;
}

const char *
efi_chain_status_name(efi_status s)
{
	static const char *const names[] = {
		"success", "load error", "invalid parameter", "unsupported", "bad buffer size", "buffer too small",
		"not ready", "device error", "write protected", "out of resources", "volume corrupted", "volume full",
		"no media", "media changed", "not found", "access denied", "no response", "no mapping", "timeout",
		"not started", "already started", "aborted", "icmp error", "tftp error", "protocol error",
		"incompatible version", "security violation", "crc error", "end of media", "", "", "end of file",
		"invalid language", "compromised data",
	};
	uint64_t code = s & ~EFI_ERROR_BIT;

	if ((s == 0 || EFI_ERROR(s)) && code < sizeof(names) / sizeof(names[0])) {
		return names[code];
	}
	return EFI_ERROR(s) ? "" : "warning";
}

/* ------------------------------------------------------------------------ */
/* UTF-16 paths                                                             */

static char16
lower16(char16 c)
{
	return (c >= 'A' && c <= 'Z') ? (char16)(c + 32) : c;
}

static int
path_eq(const char16 *a, const char16 *b)
{
	while (*a && lower16(*a) == lower16(*b)) {
		a++, b++;
	}
	return lower16(*a) == lower16(*b);
}

static unsigned
len16(const char16 *s)
{
	unsigned n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

/* dst = a + b (b ASCII or UTF-16), within EFI_CHAIN_PATH_MAX; 0: too long */
static int
path_join(char16 *dst, const char16 *a, const char *b8, const char16 *b16)
{
	unsigned n = 0;

	for (; a != NULL && *a; a++) {
		if (n >= EFI_CHAIN_PATH_MAX - 1) {
			return 0;
		}
		dst[n++] = *a;
	}
	for (; b8 != NULL && *b8; b8++) {
		if (n >= EFI_CHAIN_PATH_MAX - 1) {
			return 0;
		}
		dst[n++] = (char16)(uint8_t)*b8;
	}
	for (; b16 != NULL && *b16; b16++) {
		if (n >= EFI_CHAIN_PATH_MAX - 1) {
			return 0;
		}
		dst[n++] = *b16;
	}
	dst[n] = 0;
	return 1;
}

static void
ascii_of(char *dst, unsigned cap, const char16 *src)
{
	unsigned n = 0;

	for (; *src && n + 1 < cap; src++) {
		dst[n++] = (*src >= 0x20 && *src < 0x7f) ? (char)*src : '?';
	}
	dst[n] = 0;
}

static unsigned
str_cat(char *dst, unsigned cap, unsigned at, const char *s)
{
	while (*s && at + 1 < cap) {
		dst[at++] = *s++;
	}
	dst[at] = 0;
	return at;
}

static int
ends_with_efi(const char16 *name)
{
	unsigned n = len16(name);

	return n > 4 && name[n - 4] == '.' && lower16(name[n - 3]) == 'e' && lower16(name[n - 2]) == 'f' &&
	       lower16(name[n - 1]) == 'i';
}

/* ------------------------------------------------------------------------ */
/* Files                                                                    */

static efi_file *
volume_root(const struct efi_chain_host *h, efi_handle volume)
{
	efi_simple_file_system *fs;
	efi_file *root;

	if (EFI_ERROR(h->st->bs->handle_protocol(volume, &sfs_guid, (void **)&fs)) ||
	    EFI_ERROR(fs->open_volume(fs, &root))) {
		return NULL;
	}
	return root;
}

/* read-only, always (boot_menu_efi_ro=0 changes nothing in L1) */
static efi_file *
file_open(efi_file *root, const char16 *path)
{
	efi_file *f;

	return EFI_ERROR(root->open(root, &f, path, EFI_FILE_MODE_READ, 0)) ? NULL : f;
}

/* its size and attributes; 0: no info */
static int
file_info(efi_file *f, uint64_t *size, uint64_t *attribute)
{
	uint64_t n = sizeof(scratch);
	efi_file_info *info = (efi_file_info *)scratch;

	if (EFI_ERROR(f->get_info(f, &file_info_guid, &n, info))) {
		return 0;
	}
	*size = info->file_size;
	*attribute = info->attribute;
	return 1;
}

/* up to n bytes from where the file is; how many came */
static uint64_t
file_read(efi_file *f, void *buf, uint64_t n)
{
	uint64_t got = 0;

	while (got < n) {
		uint64_t step = n - got;

		if (EFI_ERROR(f->read(f, &step, (uint8_t *)buf + got)) || step == 0) {
			break;
		}
		got += step;
	}
	return got;
}

/* NULL: an AArch64 EFI application; else what it is not */
static const char *
pe_check(const uint8_t *b, uint64_t n)
{
	uint32_t pe;

	if (n < 0x40 || b[0] != 'M' || b[1] != 'Z') {
		return "no MZ header";
	}
	pe = b[0x3c] | (uint32_t)b[0x3d] << 8 | (uint32_t)b[0x3e] << 16 | (uint32_t)b[0x3f] << 24;
	if ((uint64_t)pe + 24 + 70 > n || b[pe] != 'P' || b[pe + 1] != 'E' || b[pe + 2] != 0 || b[pe + 3] != 0) {
		return "no PE header in its first 4 KiB";
	}
	if ((b[pe + 4] | b[pe + 5] << 8) != 0xaa64) {
		return "not an AArch64 image";
	}
	/* the optional header's Subsystem: 10 an application (an EFI-stub kernel, a UKI, bootmgfw are), 11/12 drivers */
	if ((b[pe + 24 + 68] | b[pe + 24 + 69] << 8) != 10) {
		return "not an EFI application (a driver?)";
	}
	return NULL;
}

/* the GPT name of the partition a volume is on, ASCII; "" if it has none */
static void
volume_name_of(const struct efi_chain_host *h, efi_handle volume, char out[37])
{
	efi_partition_info *info;

	out[0] = 0;
	if (EFI_ERROR(h->st->bs->handle_protocol(volume, &partition_info_guid, (void **)&info)) || info->type != 2) {
		return;
	}
	char16 name[37];

	memcpy(name, info->name, sizeof(info->name));
	name[36] = 0;
	ascii_of(out, 37, name);
}

/* the path of the loader's own file on `volume`, if it came from there (QEMU: \EFI\BOOT\BOOTAA64.EFI on the ESP) */
static int
own_path(const struct efi_chain_host *h, efi_handle volume, char16 out[EFI_CHAIN_PATH_MAX])
{
	efi_loaded_image *li;
	unsigned n = 0;

	if (EFI_ERROR(h->st->bs->handle_protocol(h->image, &loaded_image_guid, (void **)&li)) || li->device != volume ||
	    li->file_path == NULL) {
		return 0;
	}
	for (const efi_device_path *dp = li->file_path; dp->type != EFI_DP_TYPE_END;
	     dp = (const efi_device_path *)((const uint8_t *)dp + (dp->length[0] | dp->length[1] << 8))) {
		unsigned len = dp->length[0] | dp->length[1] << 8;

		if (len < 4) {
			break;
		}
		if (dp->type != EFI_DP_TYPE_MEDIA || dp->subtype != EFI_DP_MEDIA_FILE_PATH) {
			continue;
		}
		const char16 *s = (const char16 *)(dp + 1);

		if (n > 0 && out[n - 1] != '\\' && s[0] != '\\' && n < EFI_CHAIN_PATH_MAX - 1) {
			out[n++] = '\\';
		}
		for (unsigned i = 0; i < (len - 4) / 2 && s[i] && n < EFI_CHAIN_PATH_MAX - 1; i++) {
			out[n++] = s[i];
		}
	}
	out[n] = 0;
	return n > 0;
}

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                              */

static int
sig_is(const void *p, const char *sig)
{
	const char *b = p;

	for (; *sig; sig++, b++) {
		if (*b != *sig) {
			return 0;
		}
	}
	return 1;
}

static void
diag_acpi(const struct efi_chain_host *h, const uint8_t *rsdp)
{
	char oem[7], line[4 * 40 + 1];
	unsigned n = 0;

	for (unsigned i = 0; i < 6; i++) {
		oem[i] = (rsdp[9 + i] >= 0x20 && rsdp[9 + i] < 0x7f) ? (char)rsdp[9 + i] : '?';
	}
	oem[6] = 0;
	uint64_t xsdt = 0;

	if (rsdp[15] >= 2) {
		memcpy(&xsdt, rsdp + 24, 8);
	}
	h->print("efi: ACPI RSDP at %x: revision %d, OEM \"%s\", XSDT at %x\n", (uint64_t)(uintptr_t)rsdp,
	    (uint64_t)rsdp[15], oem, xsdt);
	if (xsdt == 0 || !sig_is((const void *)(uintptr_t)xsdt, "XSDT")) {
		h->print("efi: ACPI: no XSDT (Windows needs one)\n");
		return;
	}
	const uint8_t *x = (const uint8_t *)(uintptr_t)xsdt;
	uint32_t len;

	memcpy(&len, x + 4, 4);
	line[0] = 0;
	for (uint32_t off = 36; off + 8 <= len && n < 40; off += 8, n++) {
		uint64_t t;

		memcpy(&t, x + off, 8);
		if (t == 0) {
			continue;
		}
		char sig[6] = { 0, 0, 0, 0, ' ', 0 };

		memcpy(sig, (const void *)(uintptr_t)t, 4);
		for (unsigned i = 0; i < 4; i++) {
			if (sig[i] < 0x20 || sig[i] >= 0x7f) {
				sig[i] = '?';
			}
		}
		str_cat(line, sizeof(line), n * 5, sig);
	}
	h->print("efi: ACPI XSDT: %d tables: %s\n", (uint64_t)n, line);
}

void
efi_chain_diag(const struct efi_chain_host *h, struct efi_chain_diag *d)
{
	static const efi_guid acpi20 = EFI_ACPI_20_TABLE_GUID, acpi10 = EFI_ACPI_10_TABLE_GUID;
	static const efi_guid smbios = EFI_SMBIOS_TABLE_GUID, smbios3 = EFI_SMBIOS3_TABLE_GUID;
	static const efi_guid fdt = EFI_FDT_TABLE_GUID, rtprop = EFI_RT_PROPERTIES_TABLE_GUID;
	static const efi_guid global = EFI_GLOBAL_VARIABLE_GUID;
	const efi_system_table *st = h->st;
	efi_runtime_services *rt = st->rt;
	efi_handle *handles;
	uint64_t count = 0;

	memset(d, 0, sizeof(*d));
	d->uefi_revision = st->hdr.revision;
	h->print("efi: UEFI %d.%d, %d configuration tables\n", (uint64_t)(st->hdr.revision >> 16),
	    (uint64_t)(st->hdr.revision & 0xffff), st->nr_tables);

	d->acpi20 = config_table(st, &acpi20);
	if (d->acpi20 != NULL && !sig_is(d->acpi20, "RSD PTR ")) {
		h->print("efi: the ACPI 2.0 table is not an RSDP: not taken\n");
		d->acpi20 = NULL;
	}
	d->acpi10 = config_table(st, &acpi10);
	if (d->acpi10 != NULL && !sig_is(d->acpi10, "RSD PTR ")) {
		d->acpi10 = NULL;
	}
	if (d->acpi20 != NULL) {
		diag_acpi(h, d->acpi20);
	} else {
		h->print("efi: no ACPI 2.0 table%s (Windows cannot start without one)\n",
		    d->acpi10 != NULL ? " (an ACPI 1.0 one only)" : "");
	}

	d->smbios3 = config_table(st, &smbios3);
	if (d->smbios3 != NULL && !sig_is(d->smbios3, "_SM3_")) {
		d->smbios3 = NULL;
	}
	d->smbios = config_table(st, &smbios);
	if (d->smbios != NULL && !sig_is(d->smbios, "_SM_")) {
		d->smbios = NULL;
	}
	if (d->smbios3 != NULL) {
		h->print("efi: SMBIOS 3 at %x: version %d.%d\n", (uint64_t)(uintptr_t)d->smbios3,
		    (uint64_t)((uint8_t *)d->smbios3)[7], (uint64_t)((uint8_t *)d->smbios3)[8]);
	}
	if (d->smbios != NULL) {
		h->print("efi: SMBIOS at %x: version %d.%d\n", (uint64_t)(uintptr_t)d->smbios,
		    (uint64_t)((uint8_t *)d->smbios)[6], (uint64_t)((uint8_t *)d->smbios)[7]);
	}
	if (d->smbios == NULL && d->smbios3 == NULL) {
		h->print("efi: no SMBIOS table\n");
	}

	d->fdt = config_table(st, &fdt);
	if (d->fdt != NULL && !sig_is(d->fdt, "\xd0\x0d\xfe\xed")) {
		d->fdt = NULL;
	}
	h->print("efi: %s\n", d->fdt != NULL ? "a device tree in the configuration table (Linux prefers it to ACPI)" :
	    "no device tree in the configuration table (Linux needs one from its loader or ACPI)");

	d->rt_properties = config_table(st, &rtprop);
	if (d->rt_properties != NULL) {
		memcpy(&d->rt_supported, (uint8_t *)d->rt_properties + 4, 4);
		h->print("efi: runtime services supported (RT properties table): mask %x\n", (uint64_t)d->rt_supported);
	}

	uint64_t size = 0;

	if (rt->get_variable == NULL) {
		d->get_variable = EFI_UNSUPPORTED;
	} else {
		d->get_variable = rt->get_variable(u"PlatformLang", &global, NULL, &size, NULL);
	}
	h->print("efi: GetVariable(PlatformLang): %x (%s) -- %s\n", d->get_variable, efi_chain_status_name(d->get_variable),
	    (d->get_variable == EFI_BUFFER_TOO_SMALL || d->get_variable == EFI_NOT_FOUND) ? "variables answer" :
	    "VARIABLES DO NOT ANSWER");
	d->query_variable = EFI_UNSUPPORTED;
	if (rt->hdr.revision >= 0x20000 &&
	    rt->hdr.header_size >= offsetof(efi_runtime_services, query_variable_info) + sizeof(void *) &&
	    rt->query_variable_info != NULL) {
		uint64_t max_var = 0;

		d->query_variable = rt->query_variable_info(EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
		    EFI_VARIABLE_RUNTIME_ACCESS, &d->nv_max, &d->nv_remaining, &max_var);
	}
	if (EFI_ERROR(d->query_variable)) {
		h->print("efi: QueryVariableInfo(non-volatile): %x (%s)\n", d->query_variable,
		    efi_chain_status_name(d->query_variable));
	} else {
		h->print("efi: non-volatile variables: %d of %d bytes free\n", d->nv_remaining, d->nv_max);
	}

	if (!EFI_ERROR(st->bs->locate_handle_buffer(ByProtocol, &sfs_guid, NULL, &count, &handles))) {
		st->bs->free_pool(handles);
	}
	d->volumes = (uint32_t)count;
	h->print("efi: %d volumes with a file system (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL)\n", count);
}

void
efi_chain_connect(const struct efi_chain_host *h)
{
	efi_handle *handles;
	uint64_t count, done = 0;

	if (EFI_ERROR(h->st->bs->locate_handle_buffer(ByProtocol, &block_io_guid, NULL, &count, &handles))) {
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		if (!EFI_ERROR(h->st->bs->connect_controller(handles[i], NULL, NULL, 1))) {
			done++;
		}
	}
	h->st->bs->free_pool(handles);
	h->print("efi: boot_efi_connect=1: %d of %d block devices connected\n", done, count);
}

/* ------------------------------------------------------------------------ */
/* The list                                                                 */

struct scan {
	const struct efi_chain_host *h;
	struct efi_chain_entry *list;
	int n, max;
	efi_handle volume;
	efi_file *root;
	char vname[37];
	char16 own[EFI_CHAIN_PATH_MAX];         /* the loader's own file on this volume; "" none */
};

static void
label_of(struct efi_chain_entry *e, const char *title, const char *vname, unsigned vol)
{
	char tmp[EFI_CHAIN_PATH_MAX];
	unsigned n;

	if (title == NULL) {
		ascii_of(tmp, sizeof(tmp), e->path + (e->path[0] == '\\'));
		title = tmp;
	}
	n = str_cat(e->label, sizeof(e->label), 0, title);
	n = str_cat(e->label, sizeof(e->label), n, " (");
	if (vname[0]) {
		n = str_cat(e->label, sizeof(e->label), n, vname);
	} else {
		char num[4] = { 'v', (char)('0' + vol / 10 % 10), (char)('0' + vol % 10), 0 };

		n = str_cat(e->label, sizeof(e->label), n, num);
	}
	str_cat(e->label, sizeof(e->label), n, ")");
}

/* the file at path, if it is an AArch64 EFI application and not yet listed, added */
static void
consider(struct scan *s, const char16 *path, uint32_t kind, const char *title, const char *options, unsigned vol)
{
	char shown[EFI_CHAIN_PATH_MAX];
	uint64_t size, attr, got;
	const char *why;
	efi_file *f;

	if (s->n >= s->max) {
		return;
	}
	for (int i = 0; i < s->n; i++) {
		if (s->list[i].volume == s->volume && path_eq(s->list[i].path, path)) {
			return;
		}
	}
	if ((f = file_open(s->root, path)) == NULL) {
		if (kind == EFI_CHAIN_CONFIG) {
			ascii_of(shown, sizeof(shown), path);
			s->h->print("efi:   entries.cfg: %s: no such file\n", shown);
		}
		return;
	}
	ascii_of(shown, sizeof(shown), path);
	if (s->own[0] && path_eq(path, s->own)) {
		if (kind != EFI_CHAIN_OTHER && kind != EFI_CHAIN_LINUX) {      /* (said once: the tree walk finds it again) */
			s->h->print("efi:   %s: the loader itself: left out\n", shown);
		}
		f->close(f);
		return;
	}
	if (!file_info(f, &size, &attr) || (attr & EFI_FILE_DIRECTORY)) {
		f->close(f);
		return;
	}
	got = file_read(f, scratch, size < sizeof(scratch) ? size : sizeof(scratch));
	f->close(f);
	if ((why = pe_check(scratch, got)) != NULL) {
		s->h->print("efi:   %s: %s: left out\n", shown, why);
		return;
	}
	struct efi_chain_entry *e = &s->list[s->n++];

	memset(e, 0, sizeof(*e));
	e->volume = s->volume;
	path_join(e->path, path, NULL, NULL);
	memcpy(e->volume_name, s->vname, sizeof(e->volume_name));
	e->kind = kind;
	e->size = size;
	if (options != NULL) {
		str_cat(e->options, sizeof(e->options), 0, options);
	}
	label_of(e, title, s->vname, vol);
	s->h->print("efi:   %d: %s -- %s, %d bytes%s%s%s\n", (uint64_t)s->n, e->label, shown, size,
	    e->options[0] ? ", options \"" : "", e->options, e->options[0] ? "\"" : "");
}

/*
 * \EFI\hakios\entries.cfg: one entry a line, "title|path|options" (options may be empty), '#' a comment. The path is
 * on the same volume. Only read, as everything here.
 */
static void
scan_config(struct scan *s, unsigned vol)
{
	efi_file *f = file_open(s->root, u"\\EFI\\hakios\\entries.cfg");
	static char cfg[sizeof(scratch)];
	uint64_t got;

	if (f == NULL) {
		return;
	}
	got = file_read(f, cfg, sizeof(cfg) - 1);
	f->close(f);
	cfg[got] = 0;
	s->h->print("efi:   \\EFI\\hakios\\entries.cfg: %d bytes\n", got);
	for (char *line = cfg; *line;) {
		char *end = line, *title = line, *path = NULL, *options = NULL;

		while (*end && *end != '\n') {
			end++;
		}
		char *next = *end ? end + 1 : end;

		*end = 0;
		if (end > line && end[-1] == '\r') {
			end[-1] = 0;
		}
		for (char *c = line; *c; c++) {
			if (*c == '|') {
				*c = 0;
				if (path == NULL) {
					path = c + 1;
				} else if (options == NULL) {
					options = c + 1;
				}
			}
		}
		if (line[0] != '#' && line[0] != 0) {
			char16 p[EFI_CHAIN_PATH_MAX];

			if (path == NULL || path[0] != '\\' || !path_join(p, NULL, path, NULL)) {
				s->h->print("efi:   entries.cfg: \"%s\": not \"title|\\path|options\": skipped\n", line);
			} else {
				consider(s, p, EFI_CHAIN_CONFIG, title[0] ? title : NULL, options, vol);
			}
		}
		line = next;
	}
}

/* \EFI\<dir>\<name>.efi, one level, in the order the volume keeps them */
static void
scan_tree(struct scan *s, unsigned vol)
{
	static char16 dirs[16][40];
	efi_file *efi = file_open(s->root, u"\\EFI");
	unsigned ndirs = 0;
	uint64_t n;

	if (efi == NULL) {
		return;
	}
	for (unsigned k = 0; k < 256 && ndirs < 16; k++) {
		efi_file_info *info = (efi_file_info *)scratch;

		n = sizeof(scratch);
		if (EFI_ERROR(efi->read(efi, &n, info)) || n == 0) {
			break;
		}
		if ((info->attribute & EFI_FILE_DIRECTORY) && info->name[0] != '.' && len16(info->name) < 40) {
			path_join(dirs[ndirs++], info->name, NULL, NULL);
		}
	}
	efi->close(efi);
	for (unsigned d = 0; d < ndirs; d++) {
		char16 dir[EFI_CHAIN_PATH_MAX], names[16][64];
		unsigned nnames = 0;
		efi_file *df;

		if (!path_join(dir, u"\\EFI\\", NULL, dirs[d]) || (df = file_open(s->root, dir)) == NULL) {
			continue;
		}
		for (unsigned k = 0; k < 256 && nnames < 16; k++) {
			efi_file_info *info = (efi_file_info *)scratch;

			n = sizeof(scratch);
			if (EFI_ERROR(df->read(df, &n, info)) || n == 0) {
				break;
			}
			if (!(info->attribute & EFI_FILE_DIRECTORY) && ends_with_efi(info->name) && len16(info->name) < 64) {
				path_join(names[nnames++], info->name, NULL, NULL);
			}
		}
		df->close(df);
		for (unsigned i = 0; i < nnames; i++) {
			char16 p[EFI_CHAIN_PATH_MAX];

			if (path_join(p, dir, "\\", names[i])) {
				consider(s, p, path_eq(dirs[d], u"Linux") ? EFI_CHAIN_LINUX : EFI_CHAIN_OTHER, NULL, NULL, vol);
			}
		}
	}
}

int
efi_chain_scan(const struct efi_chain_host *h, struct efi_chain_entry *list, int max)
{
	static const struct { const char16 *path; uint32_t kind; const char *title; } known[] = {
		{ u"\\EFI\\systemd\\systemd-bootaa64.efi", EFI_CHAIN_SYSTEMD, "systemd-boot" },
		{ u"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", EFI_CHAIN_WINDOWS, "Windows Boot Manager" },
		{ u"\\EFI\\Shell\\Shell.efi", EFI_CHAIN_SHELL, "EFI Shell" },
		{ u"\\EFI\\BOOT\\BOOTAA64.EFI", EFI_CHAIN_FALLBACK, "EFI default loader" },
	};
	struct scan s = { .h = h, .list = list, .max = max };
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(h->st->bs->locate_handle_buffer(ByProtocol, &sfs_guid, NULL, &count, &handles))) {
		h->print("efi: no volume with a file system: nothing to start\n");
		return 0;
	}
	for (uint64_t v = 0; v < count; v++) {
		s.volume = handles[v];
		if ((s.root = volume_root(h, s.volume)) == NULL) {
			h->print("efi: volume %d does not open\n", v);
			continue;
		}
		volume_name_of(h, s.volume, s.vname);
		if (!own_path(h, s.volume, s.own)) {
			s.own[0] = 0;
		}
		h->print("efi: volume %d%s%s%s\n", v, s.vname[0] ? " (partition \"" : "", s.vname, s.vname[0] ? "\")" : "");
		scan_config(&s, (unsigned)v);
		for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
			consider(&s, known[k].path, known[k].kind, known[k].title, NULL, (unsigned)v);
		}
		scan_tree(&s, (unsigned)v);
		s.root->close(s.root);
	}
	h->st->bs->free_pool(handles);
	h->print("efi: %d EFI applications found on %d volumes (boot_menu_efi_ro=%d)\n", (uint64_t)s.n, count,
	    (uint64_t)h->read_only);
	return s.n;
}

/* ------------------------------------------------------------------------ */
/* The start                                                                */

static void __attribute__((noreturn))
give_up(const struct efi_chain_host *h, const char *what, efi_status s)
{
	h->print("efi: %s: EFI_STATUS %x (%s): the phone resets in 5 s\n", what, s, efi_chain_status_name(s));
	h->st->bs->stall(5000000);
	h->reset();
	for (;;) {
	}
}

/* the volume's device path with the file's after it; NULL: no memory */
static efi_device_path *
full_path(const struct efi_chain_host *h, efi_handle volume, const char16 *path)
{
	const efi_device_path *vol = NULL;
	uint64_t vlen = 0, flen = 4 + 2 * ((uint64_t)len16(path) + 1);
	uint8_t *dp;

	if (!EFI_ERROR(h->st->bs->handle_protocol(volume, &device_path_guid, (void **)&vol)) && vol != NULL) {
		for (const efi_device_path *p = vol; p->type != EFI_DP_TYPE_END;) {
			unsigned len = p->length[0] | p->length[1] << 8;

			if (len < 4) {
				break;
			}
			vlen += len;
			p = (const efi_device_path *)((const uint8_t *)p + len);
		}
	}
	if (EFI_ERROR(h->st->bs->allocate_pool(EfiLoaderData, vlen + flen + 4, (void **)&dp))) {
		return NULL;
	}
	if (vlen) {
		memcpy(dp, vol, vlen);
	}
	efi_device_path *file = (efi_device_path *)(dp + vlen);

	file->type = EFI_DP_TYPE_MEDIA;
	file->subtype = EFI_DP_MEDIA_FILE_PATH;
	file->length[0] = (uint8_t)flen;
	file->length[1] = (uint8_t)(flen >> 8);
	memcpy(file + 1, path, flen - 4);
	efi_device_path *end = (efi_device_path *)(dp + vlen + flen);

	end->type = EFI_DP_TYPE_END;
	end->subtype = EFI_DP_END_ENTIRE;
	end->length[0] = 4;
	end->length[1] = 0;
	return (efi_device_path *)dp;
}

void
efi_chain_start(const struct efi_chain_host *h, const struct efi_chain_entry *e, const char *options)
{
	efi_boot_services *bs = h->st->bs;
	char shown[EFI_CHAIN_PATH_MAX];
	efi_handle child = NULL;
	efi_loaded_image *li;
	efi_device_path *dp;
	efi_file *root, *f;
	uint64_t size, attr, xsize = 0;
	char16 *xdata = NULL;
	uint8_t *buf;
	efi_status s;

	if (options == NULL) {
		options = e->options;
	}
	ascii_of(shown, sizeof(shown), e->path);
	h->print("efi: starting %s: %s, %d bytes\n", e->label, shown, e->size);
	if ((root = volume_root(h, e->volume)) == NULL || (f = file_open(root, e->path)) == NULL) {
		give_up(h, "the file does not open", EFI_NOT_FOUND);
	}
	if (!file_info(f, &size, &attr) || size == 0 || EFI_ERROR(bs->allocate_pool(EfiLoaderData, size, (void **)&buf))) {
		give_up(h, "no room for the file", EFI_ERROR_BIT | 9);
	}
	if (file_read(f, buf, size) != size) {
		give_up(h, "the file does not read", EFI_ERROR_BIT | 7);
	}
	f->close(f);
	root->close(root);
	if ((dp = full_path(h, e->volume, e->path)) == NULL) {
		give_up(h, "no room for its device path", EFI_ERROR_BIT | 9);
	}
	s = bs->load_image(0, h->image, dp, buf, size, &child);
	bs->free_pool(buf);
	if (EFI_ERROR(s)) {
		give_up(h, "LoadImage", s);
	}
	if (!EFI_ERROR(bs->handle_protocol(child, &loaded_image_guid, (void **)&li))) {
		if (li->device == NULL) {
			li->device = e->volume;         /* (a firmware that did not find it from the path) */
		}
		if (options[0]) {
			unsigned n = 0;
			char16 *w;

			while (options[n]) {
				n++;
			}
			if (!EFI_ERROR(bs->allocate_pool(EfiLoaderData, 2 * ((uint64_t)n + 1), (void **)&w))) {
				for (unsigned i = 0; i <= n; i++) {
					w[i] = (char16)(uint8_t)options[i];
				}
				li->load_options = w;
				li->load_options_size = 2 * (n + 1);
			}
		}
		h->print("efi: loaded at %x, %x bytes, from volume %s; load options \"%s\"\n", (uint64_t)(uintptr_t)li->image_base,
		    li->image_size, li->device == e->volume ? "as listed" : "OTHER THAN LISTED", options);
	}
	if (h->watchdog_seconds) {
		bs->set_watchdog_timer(h->watchdog_seconds, 0, 0, NULL);
		h->print("efi: the firmware's watchdog: %d s (boot_efi_watchdog=)\n", h->watchdog_seconds);
	}
	h->print("efi: StartImage\n");
	if (h->before_start != NULL) {
		h->before_start();
	}
	s = bs->start_image(child, &xsize, &xdata);
	bs->set_watchdog_timer(0, 0, 0, NULL);
	if (xdata != NULL && xsize >= 2) {
		char text[96];
		char16 w[48];
		unsigned n = 0;

		for (; n < 47 && n < xsize / 2 && xdata[n]; n++) {
			w[n] = xdata[n];
		}
		w[n] = 0;
		ascii_of(text, sizeof(text), w);
		h->print("efi: its exit data: \"%s\"\n", text);
	}
	h->print("efi: %s came back\n", e->label);
	give_up(h, "StartImage", s);
}
