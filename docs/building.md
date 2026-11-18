# Building on Linux (POCO X3 Pro)

The port is built on Ubuntu 22.04 (x86-64); the package names below are that release's. Plan on about 40 GB of disk for the
iOS files, the XNU tree and Mu-Silicium. Nothing of Apple's or of the phone's is in the repository: the iOS 16.5 files are fetched from Apple to your
machine, the firmware is pulled from your own phone.

## Host tools

```sh
sudo apt install git curl tar xz-utils lz4 build-essential flex bison m4 perl python3 python3-venv \
    autoconf automake libtool pkg-config lld nasm mono-devel qemu-system-arm qemu-efi-aarch64
```

For Mesa (`GL=1` images, `gpu=1`) also `clang ninja-build libdrm-dev`.

Ubuntu 22.04's own packages are too old for four steps:

| For | Install | Why |
|---|---|---|
| the loader (`xnuloader/build.sh`) | `clang-18`, `lld-18` from apt.llvm.org | clang 14 stops at `loader.c` with "fixup value out of range"; `build.sh` takes `clang-18`/`lld-link-18` by itself when the plain `clang` is older than 15 (or set `CLANG=`, `LLD_LINK=`) |
| the UEFI (`devices/poco-x3-pro/uefi/build.sh`) | `python3.11`, `python3.11-venv` (deadsnakes PPA) | Mu-Silicium's build reads TOML with `tomllib`; `build.sh` makes its `venv/` with the first Python that has it |
| the system image (`sysimage/fetch.sh`) | 7-Zip 23 or newer from 7-zip.org (or the ip7z releases) as `7z` | Ubuntu's p7zip 16.02 lists an APFS image as empty; `fetch.sh` refuses an older `7z` |
| the kernel and user space | theos with its Linux iOS toolchain and `sdks/iPhoneOS16.5.sdk` in `$THEOS` (default `~/theos`) | cctools (`ld64`, `nm`, `otool`, `strip`) and the SDK headers |

The UEFI build uses Mono for NuGet; without Mono it can use the system `nasm`.

## The kernel

```sh
xnu/apply.sh             # Apple's xnu-8796.121.2 and hfs-627.100.6 tags + this port (xnu/hakios.patch, xnu/files/, xnu/hfs/) -> xnu-src/
xnu-deps/setup.sh        # mig, unifdef, the SDK overlay, two libraries
./xnu-build.sh -j8 -k    # -> xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
```

`-k` is required, and the exit status is 2 on a good build: XNU's last step checks export lists this kernel does not have.
Look at the kernel file's date. No spaces in the repository's path. Details: [`xnu/README.md`](../xnu/README.md).

## The loader

```sh
xnuloader/mkidentity.sh  # identity.h from the device tree in Apple's IPSW (one 34 KB file); never committed
xnuloader/build.sh       # clang + lld-link: an EFI application, no EDK2
```

`xnuloader/mkidentity.sh --placeholder` makes an identity with no Apple data, enough for the open kernel in QEMU.

## The system image and user space

```sh
sysimage/fetch.sh                                   # iOS 16.5 (20F66) from Apple's CDN to this machine, unpacked (~8 GB while it runs)
DEVICE=poco-x3-pro REST=1 userland/build.sh         # userland/root.apfs and userland/system.apfs, with this phone's extras
DEVICE=poco-x3-pro GL=1 REST=1 userland/build.sh    # ... with Mesa, for gpu=1
```

## The UEFI

```sh
devices/poco-x3-pro/uefi/build.sh        # Mu-Silicium + the XNU hook -> Mu-Silicium/Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

It clones Mu-Silicium at the commit in `devices/poco-x3-pro/uefi/BASE`, applies the hook and builds `vayuPkg`. It flashes
nothing; `mkxnuimg.py` and `xl` pick the result up with `DEVICE=poco-x3-pro`.

## The phone's firmware

```sh
devices/poco-x3-pro/pull-firmware.sh     # the phone in Android, adb on -> fw-poco-x3-pro/ (git ignores it)
```

## Checking it without the phone

```sh
make qemu-open                           # the open kernel, a placeholder loader, a tiny user space, in QEMU
qemu/run-ios.sh 1200 -display gtk        # the iOS 16.5 user space in QEMU, 20 minutes
python3 tools/provenance-check.py        # nothing in the tree that must not be
```

Running on the phone: [`devices/poco-x3-pro/README.md`](../devices/poco-x3-pro/README.md#building-and-running).
