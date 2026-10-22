# UEFI for vayu (POCO X3 Pro)

> Status: scaffold. The profile and loader platform are in place; the patched
> Mu-Silicium UEFI for vayu is not built yet.

## What upstream already gives us

Mu-Silicium has `Platforms/Xiaomi/vayuPkg` (state: Inactive). Its UEFI status
marks Display, Internal Storage, Mass Storage, Windows Boot and Linux Boot as
working; USB Host and USB PD are not. We use the upstream `vayuPkg` as-is and
do **not** copy `uefi/files/Platforms/Xiaomi/redwoodPkg` or
`uefi/files/Binaries/redwood`.

## What HackiOS adds

Only the generic XNUP hook:

- `uefi/mu-silicium.patch` — `BootShim/BootShim.S` copies the XNUP container
  after the UEFI FD to `PAYLOAD_BASE`; `BootManagerLib` starts `XnuLoader`
  instead of the boot menu;
- `uefi/files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c`
  and `XnuLoader.h` — the loader entry point (generic; `XNU_PAYLOAD_BASE`
  is `0xF0000000`, so the profile uses the same `PAYLOAD_BASE`).

## Quick build

```sh
./uefi/build-vayu.sh
```

It clones Mu-Silicium at `uefi/BASE` if needed, applies the XNUP patch, copies
`XnuLoader.{c,h}`, creates/reuses `../venv`, and runs `build_uefi.py -d vayu -r
RELEASE`. Re-running is idempotent. The long form below is what the script does.

## Build plan

```sh
# 1. Mu-Silicium at the commit HackiOS was built on
cd work/kernel-vayu
git init Mu-Silicium
cd Mu-Silicium
git remote add origin https://github.com/Project-Silicium/Mu-Silicium.git
git fetch --depth 1 origin "$(cat ../uefi/BASE)"
git checkout FETCH_HEAD
git submodule update --init --recursive --depth 1

# 2. Apply the generic XNUP hook
git apply --whitespace=nowarn ../uefi/mu-silicium.patch
mkdir -p Silicon/Silicium/SiliciumPkg/Library/BootManagerLib
cp ../uefi/files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.{c,h} \
   Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/

# 3. Build the vayu UEFI
python3 -m venv ../venv
../venv/bin/pip install -r pip-requirements.txt
source ../venv/bin/activate
python3 build_uefi.py -d vayu -r RELEASE
```

Expected output:

```
Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

Then point the profile at it and build the boot image:

```sh
UEFI_FD=$PWD/Mu-Silicium/Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
  common/build.sh vayu
```

## Addresses (from `vayuPkg/Library/MemoryMapLib`)

| Item | Value | Source |
|------|-------|--------|
| UEFI FD base | `0x9FC00000` | `Resources/Configs/vayu.toml` + `vayuPkg` "UEFI FD" |
| UEFI FD size | `0x300000` | `vayu.toml` `uefi_fd.size`; still compare with the built FD |
| Payload base | `0xF0000000` | `XnuLoader.h`; free RAM above the UEFI map |
| Framebuffer | `0x9C000000` (inference) | `vayu.dts` `cont_splash_region@9c000000` (36 MB) |

The framebuffer base is an inference from the DTB and the UEFI memory map, not a
hardware measurement. The loader prints it as `display <w> x <h> at <base>`;
after the first UEFI session, update `.fb_base` in `xnuloader/loader.c` and
`FB_BASE` in `common/devices/vayu.conf` to the printed value.

## BootShim

`prebuilt/uefi/BootShim.bin` is built for redwood's `FD_BASE`/`PAYLOAD_BASE`.
For vayu, either:

- build Mu-Silicium (it rebuilds the shim with the profile's addresses), or
- build the shim standalone from `Mu-Silicium/BootShim`:

```sh
make -C Mu-Silicium/BootShim REQUIRES_KERNEL_HEADER=1 \
  FD_BASE=0x9FC00000 FD_SIZE=0x300000 \
  CFLAGS='--target=aarch64-linux-gnu -DPAYLOAD_BASE=0xF0000000'
```

## Known unknowns

- exact GOP framebuffer base (the `0x9C000000` hypothesis);
- whether the built `SILICIUM_UEFI.fd` is exactly `0x300000` bytes;
- whether `0xF0000000` is free in vayu's UEFI memory map;
- touch controller and panel (vayu may be FocalTech/Novatek, not Goodix GT9916);
- vayu's stock UEFI drivers: upstream `vayuPkg` already has a working set, but
  the usual Silicium binary patches may differ from redwood's.
