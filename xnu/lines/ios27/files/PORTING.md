# Porting the vayu drivers to xnu-12377 (branch `vayu/xnu-12377`)

This tree is `Scion-Project/xnu@c2f63b275f83406995066d6e6e29e4ce7a8ba56d` (xnu-12377.121.6 + Scion's iOS 27 layer) with a
VAYU board on top. The 8796 line it takes the drivers from is `the iOS 16.5 line (xnu/ of the repository)` (`6a36d992`, called **B** below);
`S:` is this tree. Plan: `plans/ios27-macos/IOS27_MACOS_PLAN.md` (K0..K8), recon: `rep-A-R1.md`, `T3-xnu12377.md`.

## What K0 fixed (the contract)

- **Board.** `MACHINE_CONFIGS=VAYU`: `-DARM64_BOARD_CONFIG_SWEET -DARM64_BOARD_CONFIG_VAYU` (makedefs/MakeInc.def), the way
  Scion's FRANKEL is built. `SWEET` is defined, so the generic machine layer is Scion's: `nos_arm_asm`, `nos_arm_pmap`,
  `sweet_secondary_entry` (start.s), GICv3 Group 1 in `pexpert/arm/pe_fiq.c`, PSCI `CPU_ON` in
  `iokit/Kernel/arm/SweetCPU.cpp`, `sweet_a64e.c`, `SweetPlatformExpert`/`SweetFramebuffer`/`SweetSurface`/
  `SweetKeyStore`/`SweetHID`, built-in pthread, HFS, AMFI. vayu's facts: `pexpert/pexpert/arm64/VAYU.h` (16K pages,
  CNTV INTID 19, GICR stride 0x20000, PSCI by SMC, cores in Aff1). Option sets: `config/MASTER.arm64.vayu` (= FRANKEL =
  STARLTE: iOS 27 target, jetsam/freeze, routefs, no CSR), `scion_vayu` instead of `scion_frankel`.
- **No SM7150 addresses.** SWEET's own early-framebuffer writes/mappings at `0x9c000000` are compiled for
  `SWEET && !A5XELTE && !FRANKEL && !VAYU` (start.s:586,775; arm_vm_init.c:2091,2533,2548; sweet_early.c:136). VAYU
  gets FRANKEL's no-op early screen. **Any new `#if defined(SWEET)` code that names an address must exclude VAYU.**
- **No Exynos drivers.** The S9 layer's Samsung/Exynos/Broadcom drivers (47 `bsd/dev` files, 5 IOKit files) are under
  the new option `scion_exynos`; every other generic board's MASTER names it, VAYU's does not. What the generic files
  ask of them is answered by `bsd/dev/vayu_stubs.c` (from `frankel_stubs.c`, no hardware access).
- **Page size: 16K** (decision K0; A-R1 "4K/16K"): `__ARM_16K_PG__` in VAYU.h, as SWEET/FRANKEL. iOS 27's 16K shared
  cache and CodeDirectories need no 16K-on-4K paging. `ARM64_LARGE_MEMORY=0` (iPhoneOS default; compact kernel VA)
  — K3 checks the physical aperture against vayu's 6/8 GiB.
- **Where drivers start** (`bsd/sys/vayu_12377_abi.h`, `bsd/dev/vayu_board.c`): no port edits `bsd_init.c`. A driver
  writes, at file scope, `VAYU_HOOK(VAYU_PHASE_AFTER_DEVFS, nvt, vayu_nvt_start);` (or `VAYU_PHASE_BEFORE_ROOT` for
  block devices a root is on). `bsd_init.c` runs the hooks of each phase at one fixed point, **only with `vayu27=1`**,
  and each hook still checks its own 8796 boot argument (`vayu_arg("touchscreen")`). Earlier than that: XNU's own
  `STARTUP()` (machine layer). Root on UFS: `rd=ufsroot0` calls `sweet_ufs_rootdev()` (IOKitBSDInit.cpp:971), which the
  UFS port defines (stub in vayu_stubs.c until then).
- **DMA/MMIO declarations**: `bsd/sys/vayu_dma_compat.h` (kmem_alloc_contig now in `vm/vm_kern_xnu.h`, ml_io_map,
  ml_io_map_wcomb, kvtophys, flush_dcache). Scion's `bsd/sys/hakios.xnu.h` declares the same plus `loader_log_line()`,
  `loader_return()`, `FlushPoC_DcacheRegion()`.
- **Build lists**: `bsd/conf/files`, `iokit/conf/files.arm64`, `osfmk/conf/files.arm64` have one block per port
  (`# A-Kn ...:` ... `# end A-Kn`). A port adds its lines **inside its own block only** (`optional scion_vayu`), so
  eight branches merge without conflicts. Every other shared file below has exactly one owner.

## Build

- Mac (native, the build host now): the working tree is copied to the Mac (one shared object tree, one build at a
  time, by content) and `tools/vayu/build-mac.sh` runs there (Scion's make line with Xcode 27.2 and the KDK 26.5). Kernel back in
  `TREE/BUILD/mach.development.vayu`. A clean build ~25-40 min at -j4, incremental a few minutes.
- QEMU: `tools/vayu/qemu-vayu.sh TREE/BUILD/mach.development.vayu [SECS] [CMDLINE]` boots it on a Linux host (QEMU 9.2 virt,
  GICv3, cortex-a76, 1 CPU) with **Scion's** xnuloader (scion-kernel 1950dc78, built in `w0/xnu12377-tools/sk-loader`;
  ours of the 8796 line has no xnu-12377 device tree) and `adt_layer=s9` (its 12377 device tree: 256-byte random-seed,
  the uart phandle; without it the kernel panics "Expected 256 seed bytes from bootloader, but got 64"). PASS =
  "Hello from user space on Darwin!" (hello is pid 1 and exits, so "initproc failed to start" follows, as on 8796).
  K0's kernel (f955895a) passes: banner `xnu-12377.121.6-vayu`, SweetPlatformExpert, AppleKeyStore, IOSurfaceRoot,
  SweetHID, AMFI/Sandbox stand-ins, HFS, `BSD root: md0`, both VAYU hook points, launchd = hello.
- `Couldn't alloc class "StarbiteAccelerator"` in the log: something still asks for the S9's Metal accelerator class
  (scion_exynos, not built). Harmless; K8 replaces it with HakiosGPU's.
- Linux (fallback, not finished): a fakeroot build (fakeroot with
  availability 157.2, firehose headers 1542.100.32, mig; macOS 26.1 SDK Kernel.framework for iig; clang 18). Stops at
  `__builtin_ctzg`/`__builtin_clzg` (clang >= 19 needed) and the KDK's `TrustCache/API.h`, `CoreEntitlements/V2`.
- Loader ABI: `struct boot_args` has a 1024-byte CommandLine in S, 608 in both loaders (ours and Scion's): the kernel
  reads `bootFlags`/`memSizeActual` past the loader's struct (zeros today). K1 makes the loader write S's layout.

## Rules for every port (K1..K8)

1. Own worktree of this branch (`git worktree add ../a-kN-12377 -b vayu/a-kN vayu/xnu-12377`), own files only (table).
   A file not in your row and not new: ask the integrator (K0) instead of editing it.
2. New code: `#if defined(ARM64_BOARD_CONFIG_VAYU)` (or `VAYU` in machine-layer headers); new behaviour behind
   `vayu27=1` plus its own boot argument, off by default. Nothing that writes a PMIC, pin or flash without its argument.
3. Bring B's file, then adapt to 12377 APIs (below). Header in each file: origin (`the iOS 16.5 line (tree 6a36d992) <path>`),
   license (APSL-2.0 as the tree; Scion's files: APSL-2.0 per their headers).
4. Six B headers collide with Scion's (`bsd/sys/hakios.{audio,camera,input,lx,net,sta}.h`, all different): Scion's
   copies are used by the S9 drivers (`scion_exynos`). Do not overwrite them: merge so both compile, or give vayu's
   its own name.
5. Do not re-add 8796 code that Scion already has in another form (pmap `ptoa(pp)` fix, kalloc_type, the extra_ram
   path, IOSurface/Framebuffer user clients): adapt to Scion's.

## Ownership and file lists

| Port | Owns in this tree (S) | Brings from B | Notes (12377 / T3 / A-R1) |
|---|---|---|---|
| **K1** boot, loader, PE | `osfmk/arm64/start.s`, `osfmk/arm/arm_init.c`, `osfmk/arm64/sweet_early.c` (VAYU block), `pexpert/arm/pe_bootargs.c`, `pe_kprintf.c`, `pe_serial.c`, `pe_init.c`, `osfmk/kern/debug.c`, `osfmk/console/serial_console.c`, new `osfmk/arm64/loader_return.c`, `user_exception_trace.c`, `generic_arm64_support.c` | `osfmk/arm64/loader_return.c`, `user_exception_trace.c`, `generic_arm64_support.c`, start.s hunks (`XNULDRST` slot, early vectors B:start.s:260-339, `LOADER_CHECKPOINT` :615-657,803,936-943), arm_init.c checkpoints (B:341-381), `pe_bootargs.c` (4096-byte boot-args), b4-repo `xnuloader/` (ADT, boot_args, `mkadt_vayu.py` from n104ap.adt + SM8150 map + 1080x2400 canvas) | Scion has no `XNULDRST`/loader ring: port them with the loader ABI, do not paste B's start.s. `hakios.xnu.h` already names `loader_log_line()`/`loader_return()`. Gate: QEMU prints the banner `VAYU` and checkpoints to `bsd_init`. |
| **K2** CPU, GIC, timer, PSCI | `pexpert/arm/pe_fiq.c`, `iokit/Kernel/arm/SweetCPU.cpp`, `osfmk/arm64/machine_routines.c` (cpus=/bootcpu=), `osfmk/arm64/cpu.c`, `osfmk/arm64/sweet_c2.*` (off for VAYU), `iokit/Kernel/arm/AppleARMSMP.cpp` | `osfmk/arm64/gic_v3_irq.c` (SPI registration B:226-264, stop-SGI), `psci_smp.c/.h` (SMC/HVC, stop all CPUs before return to UEFI) | ONE owner of IAR/EOI: Scion's sleh.c:3168-3225 reads `ICC_IAR1_EL1` itself; B's gic_v3_irq.c also acks. Choose one, keep SPI attach/mask. vayu boots on **core 7** (`bootcpu=7` on the 8796 line; MPIDR Aff1=7 expected, check). sleh.c IRQ part: request edits from K4 (owner). Gate: QEMU cpus=1 then cpus=8, timer, IPI. |
| **K3** VM, pmap, DMA | `osfmk/arm/pmap/pmap.c`, `pmap.h`, `osfmk/arm64/arm_vm_init.c` (not the VAYU guards), `osfmk/arm/io_map.c`, `osfmk/arm/model_dep.c`, `bsd/sys/vayu_dma_compat.h` | B pmap/io_map hunks only where Scion lacks them | Keep Scion's `pmap.c` (`ptoa(pp)` S:7311-7320). **`pmap_wcomb_normal()` S:1921-1933 is true for every address on SWEET boards — VAYU inherits it**: decide. `VM_WIMG_WCOMB` on managed RAM >4 GiB (macOS-ForSheng 0063). 16K granule vs 4K MMIO neighbours; `vayu_cmd_db=0x85f20000` instead of reading 0xc3f0000. |
| **K4** PAC / arm64e | `osfmk/arm64/sleh.c`, `osfmk/arm64/sweet_a64e.c`, `bsd/dev/arm/kern_machdep.c`, `bsd/kern/kern_exec.c`, `osfmk/vm/vm_shared_region_pager.c`, `bsd/kern/sweet_amfi.c` | (B has no arm64e emulation; Bergamota `0131`, `sweet_pacemu.h` ideas) | VAYU path **without** Exynos M3 pinning/LSE/FP16 (sleh.c:1331-1387 STARLTE-only parts). PACGA policy; `vayu_pacemu=1`. Gate: A-T1 in QEMU. |
| **K5** IOKit GUI, KeyStore, display | `iokit/Kernel/arm/SweetPlatformExpert.cpp`, `SweetFramebuffer.cpp`, `SweetSurface.cpp/.h`, `SweetKeyStore.cpp`, `SweetEntitlements.cpp`, `SweetWatchdog.cpp`, `SweetBattery.cpp`, `bsd/sweet_hfs/sweet_hfs_pclass.c`, new display drivers | `bsd/dev/vayu_mdp.c`, `vayu_vsync.c`, `vayu_panel.c`, `vayu_wled.c`, `vayu_display_power.c` + their `bsd/sys/*.h`; only ideas from `iokit/Kernel/GenericARMPE.cpp`, `MiniIOMobileFramebuffer.cpp`, `MiniIOSurface.cpp/.h`, `MiniAppleKeyStore.cpp`, `MiniDrivers.h` | One class per service: do not add Mini* beside Sweet* (`IOMobileFramebuffer`, `IOSurfaceRoot`, `AppleKeyStore`). UEFI framebuffer first (no MDSS writes); `vayu_mdp=2` separately. Gate: services registered, surf/keystore tests in QEMU. |
| **K6** storage, USB-NCM | `iokit/bsddev/IOKitBSDInit.cpp` (root), `bsd/sweet_hfs/*` (except pclass), new storage/USB drivers, `sweet_ufs_rootdev` (remove the stub in vayu_stubs.c) | `bsd/dev/hakios.ufs.c`, `hakios.ufshc.c/.h`, `bsd/sys/hakios.ufs_area.h`, `vayu_usbnet.c`, `vayu_usbnet_ecm.c`, `vayu_ctl.c` (+ `.h`), `vayu_nbd.c` (I5), `hakios.image.c`, `hakios.sparse.c`, `hakios.fat.c`, `hakios.apfs_check.c`, `bsd/miscfs/apfs/*` (B's APFS: root_apfs=ufs_darwin), `bsd/miscfs/ramfs/ramfs.c` | Scion's `hakios.ufs.c`/`hakios.ufshc.c` (FRANKEL, copied from scion-kernel, not in this fork) are the same lineage: same path, pick one. Root: early UFS via `rd=ufsroot0` (fixes T3's "UFS registered after vfs_mountroot"). UFS read-only first (`ufs_write=0`), USB handover behind `usb_handover=1`. `kmem_alloc_contig` via vayu_dma_compat.h. |
| **K7** touch, buttons, haptics | `iokit/Kernel/arm/hid/*` (SweetHID), new input drivers | `bsd/dev/vayu_nvt.c`, `vayu_qup_spi.c`, `hakios.buttons.c`, `hakios.input.c`, `vayu_haptic.c`, `hakios.haptic.c` (+ headers; `hakios.input.h` collides) | Events into `SweetHID.cpp` natively; `/dev/hakios-touch` only as diagnostics. `nvt_qup=1 touchscreen=1`, no pinmux change unverified. |
| **K8** Adreno 640 | new GPU drivers | `bsd/dev/vayu_adreno.c`, `hakios.adreno.c`, `iokit/Kernel/hakios.gpu.cpp` | `kmem_alloc_contig`/`vm_kern_xnu.h`, `dc ivac`, WC zero-fill (0008); HotMetal2 v2 transport + hakios-metal. `starlte_gpu_user_*` stubs in vayu_stubs.c stay unless HakiosGPU answers them. `gpu=1 vayu_gpu=1 vayu_cmd_db=0x85f20000`. |
| **K0** integrator | `makedefs/*`, `config/*`, `*/conf/files*` (outside the blocks), `bsd/kern/bsd_init.c`, `pexpert/pexpert/arm64/{VAYU.h,board_config.h}`, `bsd/sys/vayu_12377_abi.h`, `bsd/dev/vayu_board.c`, `bsd/dev/vayu_stubs.c` (ports delete their own sections), `tools/vayu/*`, this file | | Merges K1..8. |

Later domains (A-D1..D8, after SpringBoard; same rules, own blocks to be added): Wi-Fi (`vayu_wifi*.c`,
`hakios.apple80211.c`, `hakios.wifi.c`, `hakios.wlfw.c`, `hakios.lx.c`, `hakios.sta.c`, `hakios.net.c`,
`iokit/Kernel/hakios.wlan.cpp`, `MiniIONetworkStack.cpp`), BT (`vayu_bt.c`, `iokit/Kernel/vayu.bt.dt.cpp`), audio
(`vayu_audio.c`, `vayu_jack.c`, `hakios.adsp.c`, `hakios.audio.c`, `hakios.speaker.c`, `hakios.mic.c`, `hakios.sound.c`,
`hakios.guard.c`), modem/LTE/SMS/GNSS (`vayu_modem.c`, `vayu_rmtfs.c`, `vayu_pas.c`, `vayu_scm.c`, `vayu_cell*.c`,
`vayu_qmux*.c`, `vayu_ipa*.c`, `vayu_sms.c`, `vayu_voice*.c`, `vayu_gnss.c`, `hakios.qmi.c`, `hakios.nas.c`,
`hakios.cell.c`, `hakios.ipa.c`, `hakios.mpss.c`, `iokit/Kernel/VayuBaseband.cpp`, `MiniAppleStockholm.cpp`), camera
(`vayu_camera.c`, `vayu_venus.c`, `hakios.camera.c`, `hakios.torch.c`), NFC/sensors/power (`vayu_nfc.c`, `vayu_ir.c`,
`vayu_slpi.c`, `vayu_ssc.c`, `vayu_fastrpc.c`, `vayu_cpufreq.c`, `hakios.cpufreq.c`, `hakios.battery.c`,
`iokit/Kernel/hakios.battery.cpp`). Not needed: `hakios.virtio_*`, `hakios.goodix.c` (QEMU/other phones),
`pthread_kext_static.c`, `libkern/amfi/miniamfi*`, `osfmk/minicrypto` (Scion has `sweet_pthread`, `sweet_amfi.c`,
`sweet_bearssl`), `bsd/hfs` (Scion has `bsd/sweet_hfs`).

## 8796 -> 12377 API changes the ports meet (T3)

- `kmem_alloc_contig()` (8 args, same form): `vm/vm_kern_xnu.h` (S:osfmk/vm/vm_kern_xnu.h:591-610), not
  `vm/vm_kern.h`. B callers: `vayu_mdp.c:42,242`, `vayu_usbnet.c:41,417`, `hakios.ufshc.c:64,1255`, `vayu_adreno.c`.
  Include `<sys/vayu_dma_compat.h>`.
- `kalloc_type`/`kfree_type`/`kalloc_data` exist on both (B:osfmk/kern/kalloc.h:635,656; S:732,753); keep the
  `__kalloc_type` summary in C (SilentS13 9b58eb837: else `kt_count=0`). Contiguous DMA stays `kmem_alloc_contig`.
- `ml_io_map` = `VM_WIMG_IO`, `ml_io_map_wcomb` = `VM_WIMG_WCOMB`; on SWEET boards `pmap_wcomb_normal()` is true for
  any PA (K3). With 16K pages a mapping covers the whole 16K granule.
- pmap: `ptoa(pp)` before shifting (S:pmap.c:7311); no 32-bit `pp << PAGE_SHIFT`.
- Interrupts: Scion's `sleh.c` handles IAR/EOI for SWEET boards (S:3168-3225) and calls the board for SPIs
  (`starlte_gic_spi_handle` on STARLTE); B's drivers register SPIs with `gic_v3_irq.c` (B:226-264). K2 provides the
  VAYU SPI registration with B's interface so other ports keep their calls.
- IOKit: `externalMethod`/`IOUserClient` unchanged in form, but the private selector ABIs are iOS 27's: compare
  `MiniIOMobileFramebuffer.cpp:131` (swap struct 0x46c) with `SweetFramebuffer.cpp:12-18,1604-1635`, `SweetSurface.cpp:8-24`
  (layout 0xa68). One provider per service.
- bsd_init order: S:bsd_init.c:796-810 registers HFS and AMFI before the root; B's `hakios_input_init` chain
  (B:bsd_init.c:985-995, hakios.input.c:382-406) is replaced by VAYU_HOOK records.
- Boot args: `BOOT_LINE_LENGTH` is 1024 in S (pexpert/pexpert/arm64/boot.h:18), 608 in B, and B's `pe_bootargs.c`
  reads a longer line (the plan's 4096 bytes) from the loader: `struct boot_args`' layout is the loader ABI (K1).
- PAC: none on SM8150; user space is arm64e: `sweet_a64e.c` emulates/strips (K4).
