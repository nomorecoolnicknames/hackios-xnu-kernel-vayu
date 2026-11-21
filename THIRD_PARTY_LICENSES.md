# Third-party licenses

Every third-party component HakiOS redistributes or fetches at build time, with its origin,
license, and status. Where "origin uncertain" or "to verify" is written, the item is **not**
asserted to be cleared for redistribution; treat it as pending review (see `COMPLIANCE.md`).

This supersedes the summary in `NOTICE`, which stays as the short, canonical list.

## Redistributed in this repository

| Component | Path | Origin | License | Notes |
|---|---|---|---|---|
| XNU modifications | `xnu/hakios.patch`, `xnu/files/` | derivative of Apple XNU | APSL 2.0 (`LICENSE.APSL`) | base tag `xnu-8796.121.2`, fetched by `xnu/apply.sh` |
| libdeflate | `xnu/files/bsd/miscfs/apfs/libdeflate/` | Eric Biggers | MIT | see its `COPYING` |
| libzbitmap | `xnu/files/bsd/miscfs/apfs/libzbitmap/`, `xnu-deps/build/lzbitmap/` | Corellium LLC (port of eafer/libzbitmap) | GPL-2.0+ OR MIT | used under MIT |
| LinuxKPI / Linux headers | `wifi/lx/include/linux/`, `wifi/lx/include/net/` | Linux / FreeBSD LinuxKPI | GPL-2.0 (Linux) / BSD-2-Clause (FreeBSD) | small compatibility headers; see each file's notice |
| Linux DRM shim | `tools/mesa/hakios-drm/`, `xf86drm.h` | Linux / X.Org | MIT | build-time shim for Mesa |
| AIR / Metal-library tooling | `tools/mesa/air/` | HakiOS (Darwin on redwood contributors); since vayu-dev's Metal task 9 the copy at `oosexgang/hakios-xnu-kernel` `63fc2b46` (vcvkk's AIR front end, as upstream took it) | MIT | LLVM bitcode layout is a published format |
| The Metal plug-in | `userland/src/HakiosMetalCA.m` | vcvkk's `userland/src/HakiosMetal.m` (github.com/vcvkk/darwin-redwood, branch hakios-workspace, at eb9fdf5); then `oosexgang/hakios-metal` `30f9c95` (`window-server/userland/src/HakiosMetalCA.m`, the same first version, blob `a1d0d2f5`) and `oosexgang/hakios-xnu-kernel` `63fc2b46`; vayu changes marked "(vayu" | MIT | the base's author granted permission to publish under MIT on 2026-11-21; the licence text below (hakios-metal's) |
| ir3 fixes for Metal | `tools/mesa/metal/ir3-shift-split.patch`, `tools/mesa/metal/ir3-ssbo-subdword.patch`, `tools/mesa/metal/air-ir3-library-wrapper.patch` | `oosexgang/hakios-xnu-kernel` `63fc2b46` | MIT (Mesa's) | patches to Mesa 25.0.7, applied to the Metal driver's builds only |
| hakios-metal direct Metal driver | `hakios-metal/` | `oosexgang/hakios-xnu-kernel`, revision `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`, `hakios-metal/`, with vayu-dev's A640 port | MIT (`hakios-metal/LICENSE`, Mesa notice in `hakios-metal/NOTICE`) | provenance, patch ledger and AIR choice in `hakios-metal/VAYU.md` |
| mkbootimg.py | `devices/poco-x5-pro/prebuilt/mkbootimg.py` | Android Open Source Project | Apache-2.0 | |
| Mu-Silicium / Project Mu derived files | `devices/poco-x5-pro/uefi/` | Project Silicium, Microsoft, contributors | BSD-2-Clause-Patent; some GPL-2.0+ OR MIT | derived parts keep upstream license |
| Qualcomm/Xiaomi UEFI driver binaries | `devices/poco-x5-pro/uefi/files/Binaries/` | Qualcomm / Xiaomi (from device `xbl.img`) | vendor's — **not redistributed** | not tracked: each user extracts them from their own phone with UEFIReader; only `README.md` (the patches) is kept |
| FocalTech touch firmware header | `xnu/files/bsd/dev/hakios.focaltech_fw.h` | FocalTech (device firmware blob) | vendor's — **to verify** | candidate to move to pull-from-device |
| Apple host headers (build only) | `xnu-deps/hostinc/mach-o/`, `.../mach/` | Apple (open-source cctools/xnu headers) | APSL 2.0 | host-side Mach-O headers for the cross build |
| Pre-built kernel & user space | `prebuilt/` | built from this repo | APSL 2.0 / MIT (as source) | reproducible from source here |
| ios-flame Wi-Fi bring-up (ported) | `xnu/files/bsd/dev/vayu_wifi_{htc_connect,scan,sta,wmi}.c` and their headers | github.com/NWylynko/ios-flame (fork of dipa-studio/ios-flame), `phone/wifi/radio.c` at `745c4341` | MIT, Copyright (c) 2026 dipa-studio | the notice is in each file |
| Other HakiOS ports: kernel parts | `xnu/files/bsd/dev/vayu_bt.c`, `vayu_gnss.c`, `hakios.apple80211.c`, `hakios.ufs.c`, `hakios.virtio_blk.c`, `bsd/hfs/hfs_builtin.c`, `osfmk/arm64/user_exception_trace.c` and their headers; the HFS, exitlog and Metal hunks of `xnu/hakios.patch` | `xreactx/hakios-xnu-kernel` `oneplus-6t-upstream` `0e5e7bd6a05c`, `12d310ec3b2e`; `janhurn9400/hackios-xnu-kernel` `renoir` `7287928f164b`; `telegdyd/hakios-xnu-kernel` `negroni-dev` `b4c39cc65b12`; `kiwiiiii-eth/hakios-xnu-kernel` `fajita` `d82116fb9050`, `68594340fc87`; `Scion-Project/scion-kernel` `main` `a40a96524a51`, `8042def0c06d`; `oosexgang/hakios-xnu-kernel` `main` `83aceb83817c`, `7ef54e774d32`; `telegdyd/ios-vayu` `vayu-dev` `74cfdfa7d258`, `ec38881a572c`, `274f9827f52f`, `86984b16b016`, `bd98a330786f` | APSL 2.0 (their kernels' licence) | the port, branch and commit are in each file's head |
| Other HakiOS ports: loader, user space, tools | `xnuloader/loader.c` (2.5), `userland/src/{var_hfs.h,flash_stage.*,hfs_test.c,gnssd.m,inject_loc.c,metal_*.m,metal_layer_lookup.c,init.c}`, `userland/build.sh` (`LOCATIOND`, `BT_AUDIO`, `METAL`), `devices/poco-x3-pro/tools/{standalone.sh,darwin-install.sh,var-volume.sh}`, `qemu/run-hfs-test.sh`, `tools/{metal-mklib.py,macho-sign.py,air/mk-host.sh}`, `docs/metal-*.md` | `telegdyd/ios-vayu` `vayu-dev` `546ac05ab957`, `8054280162b9`, `86984b16b016`, `56f0a3adbd41`, `274f9827f52f`, `ec38881a572c`, `74cfdfa7d258`, `061e5a82d023`; `oosexgang/hakios-xnu-kernel` `main` `83aceb83817c`; `xreactx/hakios-xnu-kernel` `oneplus-6t-upstream` `12d310ec3b2e`, `e86c96b9ada8`; `janhurn9400/hackios-xnu-kernel` `renoir` `7287928f164b`; `adelholzener69/hakios-xnu-kernel-raphael` `raphael` `3233e8371422` | MIT (their user space's licence) | the port, branch and commit are in each file's head |
| HFS Plus changes | `xnu/hfs/hfs.patch`, `xnu/hfs/hfs.sh`, `core.list`, `encodings.list` | `oosexgang/hakios-xnu-kernel` `main` `7ef54e774d32` (the layout), the changes as `telegdyd/ios-vayu` `vayu-dev` has them | APSL 2.0 | Apple's hfs-627.100.6 itself is fetched, not kept |
| Other HakiOS ports: the snatch branch | `xnu/files/bsd/dev/vayu_ipa_qmi.c` (`vayu_ipa_flt_ack=`), `xnu/files/bsd/dev/hakios.battery.c` and `hakios.buttons.c` (`vayu_charge=`) | this repository's `snatch` branch `640c7df`, `e777381` (after `kiwiiiii-eth/hakios-xnu-kernel` `b47c5df2`, `Akhdaa/hakios-xiaomi-munch` `15014aa2`) | APSL 2.0 | rewritten for this tree; named in the driver's comments |
| The iOS 27 kernel line | `xnu/lines/ios27/` | a patch and files over `Scion-Project/xnu` `c2f63b27` (xnu-12377.121.6 + Scion's iOS 27 layer); the SWEET board files from `Scion-Project/scion-kernel` | APSL 2.0 | the base is fetched by `xnu/line.sh`, not kept |
| The macOS 13.4 kernel line | `xnu/lines/macos13/` | a patch and files over Apple's `xnu-8796.121.2`, HFS Plus as `xnu/hfs/` | APSL 2.0 | the base is fetched by `xnu/line.sh`, not kept |
| Terminus Font glyphs | `xnuloader/boot_menu_font.h` (generated by `xnuloader/mkfont.py`) | Terminus Font, Dimitar Toshkov Zhekov | SIL OFL 1.1 (`xnuloader/font-OFL.txt`) | the boot menu's 16x32 ASCII glyphs |
| rEFInd build and patches | `xnuloader/refind/` | rEFInd v0.14.2 (rodsbooks.com/refind), fetched by `xnuloader/refind/build.sh` | GPL-3.0 | our patches to it are GPL-3.0; rEFInd's sources are not kept |
| POCO X3 Pro device tree | `devices/poco-x3-pro/prebuilt/device-tree.dtb` | Mu-Silicium `Resources/DTBs/vayu.dtb` at `e51cd87`, compiled from the phone's Linux sources | GPL-2.0 (Linux DTS) — **to verify** | `pending` in `tools/provenance.allow` |

## Licence texts

### hakios-metal (MIT): `hakios-metal/`, `userland/src/HakiosMetalCA.m`

    MIT License

    Copyright (c) 2025-2026 the HakiOS authors

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Fetched at build time, never committed

| Component | Fetched by | Origin | License |
|---|---|---|---|
| iOS 16.5 IPSW (user space of the system image) | `sysimage/fetch.sh` | Apple | Apple's — user supplies, user's own copy |
| iPhoneOS 16.5 SDK | `xnu-deps/setup.sh` (github.com/theos/sdks) | Apple (headers) via theos | Apple's / theos |
| Apple open sources (AvailabilityVersions, libdispatch, libpthread, cctools, bootstrap_cmds) | `xnu-deps/setup.sh` | Apple | APSL 2.0 |
| HFS Plus, hfs-627.100.6 (the kernel's data volume) | `xnu/hfs/hfs.sh`, from `xnu/apply.sh` | Apple, github.com/apple-oss-distributions/hfs | APSL 2.0 |
| MacOSX13.3 SDK (Kernel.framework DriverKit headers only) | `xnu-deps/setup.sh` | Apple via joseluisq/macosx-sdks | Apple's |
| lzfse | `xnuloader/mkidentity.sh` (when no decoder present) | Apple, github.com/lzfse/lzfse | BSD-3-Clause |
| ath11k (Wi-Fi driver) | `tools/mk-ath11k.sh` | Linux | BSD-3-Clause-Clear |
| Mesa (GL=1 images) | `tools/mk-mesa.sh` | Mesa project | MIT and others |
| Procursus / Sileo packages | `tools/mk-jb.sh` | Procursus team | their own licenses |
| Phone firmware (GPU, Wi-Fi, DSP, modem, haptics, cameras) | `devices/poco-x5-pro/pull-firmware.sh` | Qualcomm / Xiaomi | vendor's — user's own device |

## How to rebuild the inventory

- Tracked non-text/binary files: `git ls-files -z | xargs -0 file --mime-type | grep -v text/`
- License mentions in tracked files: `git grep -li "SPDX-License-Identifier\|GPL\|BSD\|MIT\|APSL\|copyright"`

If a component's origin is uncertain, it is marked **to verify** above and must be treated as not
yet cleared for redistribution until confirmed with its owner.
