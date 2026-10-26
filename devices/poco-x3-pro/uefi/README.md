# UEFI for vayu

Mu-Silicium has its own port for this phone (`Platforms/Xiaomi/vayuPkg`); HakiOS uses it as it is. What
HakiOS adds is the same generic hook as for the POCO X5 Pro, kept in `../../poco-x5-pro/uefi/`:

- `mu-silicium.patch`: `BootShim/BootShim.S` copies the "XNUP" container behind the UEFI image to
  `PAYLOAD_BASE` (0xF0000000), and `BootManagerLib` starts `XnuLoader` instead of the boot menu;
- `files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.[ch]`: the loader's entry point.

Here:

- `BASE`: the Mu-Silicium commit the port is made on (the same as the POCO X5 Pro's);
- `no-recursive-submodules.patch`: Mu-Silicium's `RequiredSubmodule` second argument is `recursive`, not
  `required`; for vayu it is set to `False` for `Mu_Basecore` and the OpenSSL tree, so `stuart --update`
  does not fetch OpenSSL's test-only trees, which the build does not need;
- `build.sh`: clones Mu-Silicium at `BASE`, applies both patches, copies `XnuLoader.[ch]`, makes a venv and
  runs `build_uefi.py -d vayu -r RELEASE`. Run it again and it keeps the clone and skips patches already
  applied. It flashes nothing;
- `verify-bootimg.py`: checks an image before it is booted: the Android header, that the decompressed
  kernel is `BootShim + SILICIUM_UEFI.fd + XNUP payload`, the payload's `platform=vayu` entry and the appended
  device tree.

## Build

Packages as in Mu-Silicium's `setup_env.sh` (see `../../poco-x5-pro/uefi/README.md`).

```sh
devices/poco-x3-pro/uefi/build.sh      # -> Mu-Silicium/Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
export DEVICE=poco-x3-pro
./xl enter                             # makes the boot image (mkxnuimg.py) and boots it with fastboot
```

Verify an image the same way, without booting it:

```sh
devices/poco-x3-pro/uefi/verify-bootimg.py xnu-usb.img \
    --fd Mu-Silicium/Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
    --dtb devices/poco-x3-pro/prebuilt/device-tree.dtb --platform vayu
```

The output is reproducible: `xnuloader/build.sh` links the loader with `/Brepro` and `mkxnuimg.py` writes the
gzip header with `mtime=0`, so two builds from the same inputs give the same bytes.

## Addresses

| Item | Value | Source |
|---|---|---|
| UEFI image base | `0x9FC00000` | Mu-Silicium `Resources/Configs/vayu.toml` and `vayuPkg` |
| UEFI image size | `0x300000` | `vayu.toml` (`uefi_fd.size`) |
| Payload base | `0xF0000000` | `XnuLoader.h`; free RAM above the UEFI memory map |
| Framebuffer | `0x9C000000` | the loader's own log on the phone: `display 1080 x 2400 at 9c000000` |

`mkxnuimg.py` builds the boot shim from `Mu-Silicium/BootShim` with these addresses. The prebuilt
`BootShim.bin` of the POCO X5 Pro is for its own addresses and is not used for this phone.
