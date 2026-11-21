/*
 * hello_ret.efi: the EFI application qemu/run-standalone-test.sh starts from the loader (efi_chainload.c). It says
 * what it was given -- its load options, and whether its own file opens from LoadedImage->DeviceHandle, as an OS
 * loader's would -- and goes back with Exit(EFI_ABORTED) and exit data, which the loader must answer with a reset.
 *   clang --target=aarch64-unknown-windows -ffreestanding -fshort-wchar -I xnuloader -c hello_ret.c
 *   lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /out:hello_ret.efi hello_ret.obj
 */
#include "efi.h"

static efi_system_table *ST;

static void
say(const char16 *s)
{
	ST->con_out->output_string(ST->con_out, s);
}

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid li_guid = EFI_LOADED_IMAGE_GUID, fs_guid = EFI_SIMPLE_FS_GUID;
	efi_loaded_image *li;
	efi_simple_file_system *fs;
	efi_file *root, *self;
	char16 *data;

	ST = st;
	say(u"\r\nhello_ret: started\r\n");
	if (EFI_ERROR(st->bs->handle_protocol(image, &li_guid, (void **)&li))) {
		say(u"hello_ret: no loaded image protocol\r\n");
	} else {
		say(u"hello_ret: load options [");
		if (li->load_options != NULL && li->load_options_size >= 2) {
			say((const char16 *)li->load_options);
		}
		say(u"]\r\n");
		const efi_device_path *fp = li->file_path;

		if (li->device == NULL || fp == NULL || fp->type != EFI_DP_TYPE_MEDIA || fp->subtype != EFI_DP_MEDIA_FILE_PATH) {
			say(u"hello_ret: no device handle or no file path\r\n");
		} else if (EFI_ERROR(st->bs->handle_protocol(li->device, &fs_guid, (void **)&fs)) ||
		    EFI_ERROR(fs->open_volume(fs, &root)) ||
		    EFI_ERROR(root->open(root, &self, (const char16 *)(fp + 1), EFI_FILE_MODE_READ, 0))) {
			say(u"hello_ret: its own file does NOT open from DeviceHandle\r\n");
		} else {
			say(u"hello_ret: its own file opens from DeviceHandle: ");
			say((const char16 *)(fp + 1));
			say(u"\r\n");
			self->close(self);
			root->close(root);
		}
	}
	static const char16 why[] = u"hello_ret says goodbye";

	if (EFI_ERROR(st->bs->allocate_pool(EfiLoaderData, sizeof(why), (void **)&data))) {
		return EFI_ABORTED;
	}
	for (unsigned i = 0; i < sizeof(why) / 2; i++) {
		data[i] = why[i];
	}
	say(u"hello_ret: Exit(EFI_ABORTED)\r\n");
	return st->bs->exit(image, EFI_ABORTED, sizeof(why), data);
}
