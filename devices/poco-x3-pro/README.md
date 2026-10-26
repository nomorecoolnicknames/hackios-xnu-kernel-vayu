# Device: POCO X3 Pro (vayu)

Everything in HakiOS that belongs to this phone is here, or in the `vayu_*` files of the kernel. This is
the second phone of the project, after the POCO X5 Pro (`../poco-x5-pro/`), and it shows what a port to an
older SoC costs: nearly every driver is its own, because the Snapdragon 860 is not the Snapdragon 778G.

| What | Where |
|---|---|
| SoC | Qualcomm Snapdragon 860 (SM8150-AC), Adreno 640, 8 cores (4 Cortex-A55, 4 Cortex-A76) |
| UEFI for the phone | `uefi/`: Mu-Silicium's own `vayuPkg` with the same XNU hook as the POCO X5 Pro's (`../poco-x5-pro/uefi/mu-silicium.patch`); `uefi/build.sh` clones, patches and builds |
| What `mkxnuimg.py` and `xl` read | `boot.conf`: the Mu-Silicium platform, where the UEFI image sits, the loader's name for the phone |
| Device tree | `prebuilt/device-tree.dtb`: Mu-Silicium's `Resources/DTBs/vayu.dtb` unchanged (SHA-256 `1c71a972e92b4ea3e21966145bd745346f760079d0e4f04203179ff9dead0db2`, Mu-Silicium `e51cd87`) |
| SoC addresses | `soc.args`: what `tools/soc-args.py` reads from that device tree (see below) |
| The phone's firmware | **not in the repository**: `pull-firmware.sh` pulls it off your phone over `adb` into `fw-poco-x3-pro/` at the top; `userland/build.sh` copies it into the image's `fw/` |
| Extra user-space programs | `userland/`: `build.sh` (sourced by `userland/build.sh`) and `src/` |
| Tools | `tools/camtab.py` (the camera sensor tables), `tools/gpt-plan.py` (a preview of the partition-table change that makes room for the port's own partition; writes nothing to a device) |

The kernel's drivers for this phone are the files `xnu/files/bsd/dev/vayu_*.c` (headers in
`xnu/files/bsd/sys/vayu_*.h`); the head of each says where its numbers come from. Where the POCO X5 Pro's
drivers could be shared they were changed in small ways (`hakios.adreno.c`, `hakios.battery.c`,
`hakios.buttons.c`, `hakios.torch.c`, `hakios.ufs.c`, `hakios.input.c` and others): a `vayu_*` boot-arg
selects the vayu path, and without it the phone is untouched at redwood's addresses.

## What works

Labels: **hardware** = ran on this phone and the result was seen; **built** = compiled and linked, no hardware
result for it; **not working** = ran, and does not work. "Hardware" results were seen on builds made
before this tree was rebuilt on the current upstream; this tree's kernel (`xnu-build.sh`), loader and the
user-space sources compile and link, and it has not been booted since. Run it before trusting any line.

| Component | Chip or block | Driver | State |
|---|---|---|---|
| Boot: UEFI, loader, XNU, USB link | Mu-Silicium `vayuPkg`, `xnuloader`, DWC3 | `xnuloader/loader.c` (`platform vayu`), `vayu_pas.c` | hardware: the loader starts, `xl` talks to it, the kernel boots to `launchd`; the SMC watchdog is not available, so a hung run needs a power-cycle |
| Display | DSI panel, framebuffer by UEFI | `MiniIOMobileFramebuffer.cpp`, `vayu_vsync.c`, `vayu_mdp.c` | hardware: SpringBoard on screen; the controller is only read (no zero-copy present yet) |
| Touch | Novatek NT36675 on QUP SPI (GPI DMA) | `vayu_nvt.c`, `vayu_qup_spi.c` | hardware: works (`touchscreen=1 nvt_qup=1`); the chip is flashless and XNU downloads its firmware at every start |
| CPU frequency | Cortex-A55/A76 clusters | `vayu_cpufreq.c` | hardware |
| Storage | UFS | `hakios.ufs.c` | hardware, read-only survey and reads |
| Battery, charger, buttons | PM8150B, PM8150 keys | `hakios.battery.c`, `hakios.buttons.c` | hardware: charger state and volume keys follow the phone; the fuel gauge's numbers are not confirmed |
| Backlight, screen off on lock | PM8150L WLED | `vayu_wled.c` | hardware: the screen turns off on lock; the brightness slider is not confirmed |
| Flashlight | PM8150L | `hakios.torch.c` | hardware |
| 3D | Adreno 640 | `vayu_adreno.c`, with `hakios.gpu.cpp` and Mesa/freedreno in user space | hardware: SpringBoard drawn on the GPU (`gpu=1 vayu_gpu=1 vayu_keep=1`, needs the GPU firmware and the signed zap shader from your own phone) |
| Haptics | LRA through the PMIC | `vayu_haptic.c` | hardware: a test pulse is felt; no iOS route |
| Audio | WCD934x codec, TAS256x amplifiers, ADSP | `vayu_audio.c`, `vayu_pas.c`, `hakios.audio.c`, `userland/src/audio_plugin.c` | hardware: sound plays; known defects, speaker crackle among them |
| Headphone jack | WCD934x on SLIMbus | `vayu_jack.c` | built, the codec's chip ID was read on hardware; no jack audio claimed |
| Cameras | IMX582, S5K3T2 | `vayu_camera.c`, `userland/src/camera_*.c` | hardware: frames reach the camera app; colour, speed and lag are not right yet |
| Sensors | SLPI (SSC) accelerometer and others | `vayu_slpi.c`, `vayu_ssc.c`, `vayu_fastrpc.c`, `userland/src/motion.c` | built; a stream for iOS auto-rotate exists, not confirmed on hardware |
| Modem | MPSS through the remote file system | `vayu_modem.c`, `vayu_rmtfs.c`, `vayu_pas.c` | hardware: the modem boots, its remote file system is served, WLFW is announced |
| Cellular | QMI DMS/UIM/NAS, WDS | `vayu_cell.c` | hardware: SIM ready, registration on LTE seen. **Data is not working**: the modem asserts on the first data session because the IPA initialisation message is not sent yet |
| Wi-Fi | WCN399x on the modem's DSP | `vayu_wifi.c` | not working: the firmware accepts the board data, then crashes; no association |
| Bluetooth | Qualcomm WCN3998 on a QUP UART | `vayu_bt.c`, `userland/src` extras: `bluetool_stub.c` | built; the controller initialises and `bluetoothd` reports its stack up on hardware, then stops at a vendor-specific command that times out (a workaround for it is written and has not been seen to work); no pairing |
| NFC | NXP PN553 on I2C | `vayu_nfc.c` | built; the controller answers `CORE_RESET` and `CORE_INIT` on hardware, discovery is not reached |
| IR blaster | SPI engine | `vayu_ir.c`, `vayu_irtest.c` | built, not confirmed |
| USB networking | DWC3 gadget, CDC-ECM | `vayu_usbnet.c`, `vayu_usbnet_ecm.c` | built, not confirmed |

The rest of the stock iOS user space comes from your own IPSW exactly as on the POCO X5 Pro.

## Building and running

Everything starts from the top of the repository. Get the iOS files and build the kernel and loader as in
the main README, then:

```sh
devices/poco-x3-pro/uefi/build.sh                     # the UEFI with the XNU hook -> Mu-Silicium/Build/vayuPkg/...
devices/poco-x3-pro/pull-firmware.sh                  # the phone in Android, on adb -> fw-poco-x3-pro/
DEVICE=poco-x3-pro REST=1 userland/build.sh           # root.apfs and system.apfs with this phone's extras
export DEVICE=poco-x3-pro
./xl enter                                            # fastboot-boots the UEFI, which starts the loader
./xl ramdisk userland/system.apfs                     # the image into the phone's RAM
XL_CMDLINE="bootcpu=7 cpus=8 fb_threads=6 vtimer_intid=19 touchscreen=1 nvt_qup=1 gpu=1 vayu_gpu=1 vayu_keep=1" ./xl run
```

`xl` and `mkxnuimg.py` read `boot.conf` for the platform, the UEFI target and the UEFI image's address, so
`DEVICE=poco-x3-pro` is all they need. Nothing is flashed: the phone is booted with `fastboot boot` and
a reboot brings it back to its own Android. The boot loader must be unlocked.

Boot-args that matter on this phone:

- `vtimer_intid=19`: the SM8150 wires the arch timer's virtual PPI to INTID 19 (the kernel's default, 27,
  is the POCO X5 Pro's and QEMU's); without it no timer interrupt arrives and the kernel stops at the first
  wait. `bootcpu=7 cpus=8`: boot on the last core, use all eight. `fb_threads=6`: six processors share the
  frame copy onto the panel.
- Each `vayu_*` driver has a switch of its own (`vayu_audio=1`, `vayu_camera=...`, `vayu_modem=1`,
  `vayu_rmtfs=2`, `vayu_cell=2`, `vayu_wifi=1`, `vayu_bt=1`, `vayu_nfc=1`, `vayu_ir=1`, ...), and the head
  of its file lists the rest. Anything that writes to a pin controller outside what the loader prepared is
  behind an explicit opt-in (`..._xnu_tlmm=1`).
- The cellular bring-up is opt-in step by step: `vayu_cell_online=1`, `vayu_cell_provision=1`,
  `vayu_cell_register=1`, `vayu_cell_data=1`.

The modem, GPU and DSP firmware, the touch controller's firmware, the sensor tables and the signed zap
shader are the vendor's; they come from your phone and stay out of git.

## The SoC addresses

`soc.args` is what `tools/soc-args.py` prints for `prebuilt/device-tree.dtb` (the same procedure as in
`../poco-x5-pro/README.md`). The `vayu_*` drivers carry vayu's addresses in their own source, with the
device-tree node each came from, and do not read `soc_*` arguments; the file is kept as the worked example
and for the shared modem and IPA code, which does read them.

## Partition table

`tools/gpt-plan.py` previews, from dumps of the first and last blocks of the phone's UFS unit, the change
that makes room for the port's own partition for the iOS volume (`show`, `compare`, `plan`); it writes no
device. The loader refuses any block write except into that partition, past a block named in
`xnuloader/loader.c` (`darwin_vayu`), and only after checking the partition's whole GPT entry.
