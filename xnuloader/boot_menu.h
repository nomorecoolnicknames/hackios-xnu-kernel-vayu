/*
 * The boot menu of a standalone start (boot_menu=1): what it needs from UEFI and from the other parts of the
 * loader. boot_menu.c is compiled as part of loader.c (included by it, like bcb.h), after everything it calls.
 *
 * What the other entries' parts provide, defined before boot_menu.c is included:
 *
 *   EFI applications (efi_chainload.h), with boot_menu_efi=1: efi_chain_host_init(),
 *     efi_chain_diag(), efi_chain_scan() once as the menu comes up, efi_chain_start() for the one chosen (never
 *     returns: a cold reset whatever happens)
 *   misc's boot control block (loader.c): bcb_menu_android() (never returns),
 *     bcb_menu_default_name() / bcb_menu_default_next() with boot_menu_bcb=1, bcb_menu_read() for the
 *     diagnostics, standalone_by_hand (set when HakiOS is chosen with a key: counted, never sent elsewhere)
 */
#ifndef XNULOADER_BOOT_MENU_H
#define XNULOADER_BOOT_MENU_H

/* EFI_SIMPLE_TEXT_INPUT_PROTOCOL: ST->con_in. On vayu the firmware's ButtonsDxe is behind it. */
struct menu_input_key {
	uint16_t scan;
	char16 ch;
};

typedef struct menu_text_input {
	efi_status (EFIAPI *reset)(struct menu_text_input *self, uint8_t extended);
	efi_status (EFIAPI *read_key_stroke)(struct menu_text_input *self, struct menu_input_key *key);
	void *wait_for_key;
} menu_text_input;

/* UEFI scan codes (UEFI 2.x, 12.3: EFI_INPUT_KEY) */
#define MENU_SCAN_UP     0x01
#define MENU_SCAN_DOWN   0x02
#define MENU_SCAN_HOME   0x05
#define MENU_SCAN_DELETE 0x08
#define MENU_SCAN_ESC    0x17

#define BOOT_MENU_EFI_MAX 4

/* what a loader's LoadOptions ask for (refind_menu.c: rEFInd's entries start the loader with these) */
#define MENU_OPT_OFF     1      /* menu=off: no menu, HakiOS */
#define MENU_OPT_ANDROID 2      /* menu=android: bcb_menu_android() */
#define MENU_OPT_DIAG    3      /* menu=diag: the text menu, on its diagnostics page */

#endif /* XNULOADER_BOOT_MENU_H */
