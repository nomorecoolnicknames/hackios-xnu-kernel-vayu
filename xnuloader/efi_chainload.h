/*
 * Other UEFI applications started from this loader (L1 of plans/VAYU_DUALBOOT_PLAN_v2.md): Linux (systemd-boot,
 * GRUB, a kernel with its EFI stub or a UKI), Windows' boot manager, an EFI shell. Found on the FAT volumes the
 * firmware's Fat driver mounted, started with the firmware's own LoadImage/StartImage. Whatever happens after
 * the start -- the application returns, or LoadImage/StartImage fail -- ends in a cold reset, never back in Mu's
 * boot manager (which powers the phone off).
 *
 * Its own translation unit (efi_chainload.c): what it needs of the loader comes in struct efi_chain_host.
 * Interface for the boot menu.
 *
 * Boot-args (read by loader.c, efi_chain_standalone):
 *   boot_menu_efi=1     look for EFI applications at all (default 0: nothing here runs)
 *   boot_menu_efi_ro=0  (default 1) nothing in L1 writes; 0 is kept for the day something may (entries.cfg
 *                       edits), and is only logged
 *   boot_efi_run=N      (with boot_menu_efi=1) start entry N of the list without a menu: the L1 probe before the
 *                       menu exists. No such entry: said, and the start goes on as it would have
 *   boot_efi_watchdog=S (default 300, the UEFI boot manager's 5 minutes; 0: off) the firmware's watchdog armed
 *                       before StartImage: an application stuck in boot services resets the phone. Every OS
 *                       loader turns it off itself (ExitBootServices does too)
 *   boot_efi_connect=1  ConnectController on every block device first, so that the Fat driver binds volumes
 *                       nobody connected (default 0)
 */
#ifndef XNULOADER_EFI_CHAINLOAD_H
#define XNULOADER_EFI_CHAINLOAD_H

#include "efi.h"

#define EFI_CHAIN_MAX           24      /* entries in one list */
#define EFI_CHAIN_PATH_MAX      128     /* char16, NUL included */
#define EFI_CHAIN_OPTIONS_MAX   256     /* ASCII, NUL included */

enum efi_chain_kind {
	EFI_CHAIN_OTHER = 0,            /* \EFI\<any>\<any>.efi */
	EFI_CHAIN_FALLBACK,             /* \EFI\BOOT\BOOTAA64.EFI: the removable-media default, whatever it is */
	EFI_CHAIN_SYSTEMD,              /* \EFI\systemd\systemd-bootaa64.efi */
	EFI_CHAIN_WINDOWS,              /* \EFI\Microsoft\Boot\bootmgfw.efi */
	EFI_CHAIN_SHELL,                /* \EFI\Shell\Shell.efi */
	EFI_CHAIN_LINUX,                /* \EFI\Linux\*.efi: a UKI or a kernel with its EFI stub */
	EFI_CHAIN_CONFIG,               /* a line of \EFI\hakios\entries.cfg */
};

struct efi_chain_entry {
	efi_handle volume;                      /* the handle with EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
	char16 path[EFI_CHAIN_PATH_MAX];        /* on that volume: \EFI\BOOT\BOOTAA64.EFI */
	char label[64];                         /* ASCII, for the menu: "Windows Boot Manager (win)" */
	char volume_name[37];                   /* the GPT partition's name, ASCII; "" if none (a USB stick, ...) */
	char options[EFI_CHAIN_OPTIONS_MAX];    /* LoadOptions (entries.cfg), ASCII; "" none */
	uint32_t kind;                          /* enum efi_chain_kind */
	uint64_t size;                          /* of the file, bytes */
};

/* What the firmware offers Windows and Linux (efi_chain_diag) */
struct efi_chain_diag {
	uint32_t uefi_revision;                 /* SystemTable's: 0x00020046 = 2.70 */
	void *acpi20, *acpi10;                  /* RSDP ("RSD PTR " checked), NULL: none */
	void *smbios, *smbios3;                 /* entry points ("_SM_" / "_SM3_" checked) */
	void *fdt;                              /* a device tree in the configuration table (Linux takes it over ACPI) */
	void *rt_properties;                    /* EFI_RT_PROPERTIES_TABLE; NULL: all runtime services claimed */
	uint32_t rt_supported;                  /* its RuntimeServicesSupported mask (valid with rt_properties) */
	efi_status get_variable;                /* GetVariable(PlatformLang) without a buffer: BUFFER_TOO_SMALL or
	                                           NOT_FOUND = the service works, UNSUPPORTED = it does not */
	efi_status query_variable;              /* QueryVariableInfo(NV|BS|RT) */
	uint64_t nv_max, nv_remaining;          /* its storage, bytes */
	uint32_t volumes;                       /* handles with EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
};

struct efi_chain_host {
	efi_system_table *st;
	efi_handle image;                       /* the loader's: the parent of what it starts; its own file is skipped */
	void (*print)(const char *fmt, ...);    /* the loader's print: %s %x %d, every number passed as uint64_t */
	void (*before_start)(void);             /* the loader's last word before control leaves (its log flushed, its
	                                           watchdog off); NULL: none */
	void (*reset)(void);                    /* cold reset (the loader's reboot()); does not return */
	int read_only;                          /* boot_menu_efi_ro */
	uint64_t watchdog_seconds;              /* boot_efi_watchdog */
};

/* The firmware's tables and variable services, printed and returned. */
void efi_chain_diag(const struct efi_chain_host *h, struct efi_chain_diag *out);

/* ConnectController on every block device (boot_efi_connect=1). */
void efi_chain_connect(const struct efi_chain_host *h);

/*
 * The EFI applications on every FAT volume: entries.cfg's lines first, then the known names, then any
 * \EFI\<dir>\<name>.efi, each an AArch64 PE (checked by its header), the loader's own file left out. Each entry is
 * printed. Returns how many were put in list (at most max).
 */
int efi_chain_scan(const struct efi_chain_host *h, struct efi_chain_entry *list, int max);

/*
 * Start one: the file read, LoadImage with the volume's device path and the file's (so LoadedImage's DeviceHandle
 * is the volume), LoadOptions = options (NULL: the entry's own; "": none), StartImage. Never returns: whatever
 * comes back, its EFI_STATUS stays on the screen 5 s, then the cold reset.
 */
void efi_chain_start(const struct efi_chain_host *h, const struct efi_chain_entry *e, const char *options)
    __attribute__((noreturn));

/* A short name of an EFI_STATUS ("not found", "aborted", ...); "" for one it does not know. */
const char *efi_chain_status_name(efi_status s);

#endif /* XNULOADER_EFI_CHAINLOAD_H */
