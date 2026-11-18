#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
# the iPhone's identity for XNU, from the device tree of the builder's own IPSW (not in the repository)
[ -f identity.h ] || ./mkidentity.sh

# clang 14 (Ubuntu 22.04) stops at loader.c with "fixup value out of range"; a newer one beside it
# (clang-18 of apt.llvm.org) is taken when the plain names are older. CLANG= and LLD_LINK= say it outright.
if [ -z "${CLANG:-}" ]; then
	CLANG=clang; LLD_LINK=${LLD_LINK:-lld-link}
	v=$(clang -dumpversion 2>/dev/null | cut -d. -f1)
	if [ "${v:-0}" -lt 15 ]; then
		for n in 21 20 19 18 17 16 15; do
			if command -v clang-$n >/dev/null && command -v lld-link-$n >/dev/null; then CLANG=clang-$n; LLD_LINK=lld-link-$n; break; fi
		done
	fi
fi
LLD_LINK=${LLD_LINK:-lld-link}
$CLANG --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
# /Brepro: hash the PE header timestamp, so rebuilding the boot image gives the same bytes.
$LLD_LINK /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /Brepro \
  /out:loader.efi loader.obj
ls -la loader.efi
