# Device: POCO X3 Pro (`vayu`)

HakiOS on the **Xiaomi POCO X3 Pro** (`vayu`): Qualcomm Snapdragon 860 (**SM8150-AC**), Adreno 640, 8 cores
(4 Cortex-A55, 4 Cortex-A76), UFS. Apple's open XNU (xnu-8796.121.2, Darwin 22.5.0) with this port's loader and
drivers, and the iOS 16.5 user space from your own IPSW on top: launchd, backboardd, SpringBoard, the stock apps.

It is started with `fastboot boot` and runs from RAM; a reboot brings back the phone's own Android. The boot loader must be
unlocked. It is a research bench with known defects, not a daily system.

The state of each part is also in [`device.conf`](device.conf), in the common repository's words.

## What works

Seen on the phone on the lab's builds k17 to k33-daily (2026-10-26 to 2026-11-06). How sure each line is:

- **owner**: seen working on the phone by its owner, in the interface;
- **test**: a test on the phone passed, from the run's log; nobody confirmed it by eye or ear;
- **partial**: works in part; what is missing is said;
- **no**: does not work, or has never been shown to.

| Part | Chip or block | State | Seen | What was seen, and what is not done |
|---|---|---|---|---|
| Boot: UEFI, loader, XNU | Mu-Silicium `vayuPkg`, `xnuloader`, DWC3 | works | owner | to the lock screen and SpringBoard; no SMC watchdog, so a hung run needs a power-cycle |
| Display | DSI panel, UEFI's framebuffer | works | owner | SpringBoard on the panel; the panel reports a 8.333 ms period at `fb_120=1` and scripted swipes gave 117-122 updates/s (**test**); smoothness under the finger not confirmed |
| Touch | Novatek NT36675 on QUP SPI | partial | owner | multitouch works (`touchscreen=1 nvt_qup=1`); apps get about 30 events/s on a continuous drag while the chip gives 114-217 samples/s; the cause is not proven |
| Buttons | PM8150 PON, PM8150L GPIO | works | test | power and volume |
| CPU | 4 Cortex-A55 + 4 Cortex-A76 | works | test | all 8 cores, OSM frequency tables; at a low charge on a USB data port the kernel holds the clocks down (`vayu_sdp_slow=0` turns that off, and the phone then drains) |
| GPU | Adreno 640 | partial | test | OpenGL ES through Mesa freedreno draws SpringBoard (`gpu=1 vayu_gpu=1 vayu_keep=1`; needs the GPU firmware and the signed zap shader from your phone); no Metal |
| Speaker | TAS2562 / TAS2564, ADSP | partial | test | Music fed 9.9 million frames to the driver in 203 s; nobody confirmed hearing that run; crackle reported earlier |
| Microphone | WCD934x, ADSP | works | test | Voice Memos recorded 28 s of AAC, peak -8.8 dBFS, the waveform on screen; not the call path |
| Headphone jack | WCD934x on SLIMbus | no | | the codec's chip ID is read; no sound |
| Cameras | IMX582 (main), S5K3T2 (front), ultra-wide | partial | owner | main camera live in Camera.app, 17-29 fps to the app, about 30 with `vayu_sdp_slow=0`; stills from a ~2 MP frame; ultra-wide frames dark; front camera not re-checked |
| Wi-Fi, 5 GHz | WCN3990 on the modem's DSP | partial | owner | WPA2 join, DHCP by iOS itself, a page loaded in Safari; one 20-minute run held the link, another dropped after the group-key rekey (`DEAUTH reason 16`) and did not reconnect |
| Wi-Fi, 2.4 GHz and choosing a network | WCN3990 | partial | test | Settings lists networks and joins a WPA2 one; 2.4 GHz itself not tested on the phone |
| Bluetooth | WCN3998 on a QUP UART | partial | test | the switch on, two devices found in Settings (`vayu_bt=1 vayu_bt_dt=1`); no pairing, no audio; the phone's name shows as `(null)` |
| Modem | MPSS, rmtfs | works | test | the modem boots, its remote file system is served |
| Cellular network | QMI NAS/UIM | partial | test | SIM ready, LTE/UMTS registration in several runs; the stock cellular page in Settings through `ctshimd` (our CoreTelephony server) |
| Calls | QMI VOICE | no | | a call start is accepted, then error 13; no conversation |
| SMS | QMI WMS | no | | listing works at the WMS level; Messages opens to a white screen; no message sent or received |
| Mobile data | IPA 4.1 | no | | IPA and GSI reach READY, the modem does not answer `INIT_MODEM_DRIVER`; some attempts hang the SoC |
| NFC | NXP PN553 on I2C | partial | test | the controller answers (`CORE_RESET`, `CORE_INIT`); no tag read |
| GNSS | modem | no | | 10 requests, no fix (historical) |
| Sensors, auto-rotate | SLPI | no | | no accelerometer registry through FastRPC |
| Battery, charging | PM8150B | partial | test | level and charger state in the UI; on a USB data port under load the phone drains (about -0.5 A with `vayu_sdp_slow=0`); charging not shown |
| Haptics | LRA through the PMIC | partial | test | a test pulse; no iOS route |
| Flashlight | PM8150L | partial | | driver in tree; not re-checked on the k30-k33 builds |
| `/private/var` across reboots | UFS, a partition of its own | works | test | saved and found again (`var_store=1 ufs_write=1`; slot 0, 251 MB) |
| Files | | no | | the local storage provider is rejected (`pkd=no`), Browse is empty |
| USB control | DWC3 | partial | test | `xl sh`, touch and swipe, screenshots, file pull with no Wi-Fi; stalls under load |
| IR blaster, USB networking | | no | | built, never confirmed |

**Since then.** On 2026-11-18 k33-daily, rebuilt from the lab's surviving patches, ran on the phone from RAM for 773 s with
the lab's usual arguments: no panic, SpringBoard, LTE registration, the Bluetooth stack up, an address by DHCP on Wi-Fi, the
cameras at 30 fps, the USB control node there. The lab's own k30 and the k30 rebuilt from the patches gave the same that day
(with UMTS or LTE registration and the speaker test). The merged kernel of this line (the k33 line with the later work
below, before this branch's last changes) then ran for 1368 s with the same arguments: no panic, SpringBoard, LTE
registration, an address by DHCP; the Wi-Fi link held for the 20 minutes (in the earlier run that dropped, the access point
ended it at 589 s on its group-key change), but the access point did not change its group key this time, so the new
handling of that change is still untried. The battery went from 60 % to 56 % on USB and the CPU reached 73 °C. Nothing else
in the table was looked at in those runs.

**This branch.** The kernel here is the k33 line: k33-daily rebuilt from those patches, with later work on top (HFS Plus for
`/private/var` on the flash, fixes from a review of the drivers, parts of other ports, power and clock options), each new
behaviour behind a boot argument that is off by default. It builds and boots in QEMU. What ran on the phone is the merged
kernel above; this branch's last changes (the Metal plug-in's names, inert without `metal=`, and the network and operator
values moved to boot arguments) have not, and none of the new boot arguments has been turned on there yet.

## iOS 27 and macOS 13.4 on this phone

Runs of 20-21 November 2026, with the kernels of `xnu/lines/` (see the main README):

| System | Kernel | Seen on the phone | Not yet |
|---|---|---|---|
| iOS 27 | xnu-12377 on Scion's iOS 27 kernel (`xnu/lines/ios27/`) | the user space starts from a root over the USB cable (NBD), about a hundred processes; the daemons' self-SIGKILL fixed (`vayu_spawnattr=27`) | SpringBoard crash-loops on SIGABRT; no picture of the system on the panel (the boot console and the logo, then black) |
| macOS 13.4 | xnu-8796.121.2 (`xnu/lines/macos13/`) | the volumes on NBD disks; WindowServer, the Dock and the menu bar, drawn by the Adreno 640 (HakiosMetal); the desktop on the panel, portrait, HiDPI 2x | the wallpaper's picture (its window is on screen); touch and the cursor |

The panel's pictures in those runs came from the kernel's log (`vayu_fbthumb=N`, `tools/fbthumb2png.py`).

## What is here

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

The kernel's drivers for this phone are the files `xnu/files/bsd/dev/vayu_*.c` (headers in `xnu/files/bsd/sys/vayu_*.h`); the
head of each says where its numbers come from and lists its boot arguments. Where the POCO X5 Pro's drivers could be shared they
were changed in small ways (`hakios.adreno.c`, `hakios.battery.c`, `hakios.buttons.c`, `hakios.torch.c`, `hakios.ufs.c`,
`hakios.input.c` and others): a `vayu_*` boot argument selects the vayu path, and without it the phone is untouched at redwood's
addresses.

## Not here, by design

No file of iOS, no firmware of the phone, no screenshot. The modem, GPU, DSP, Wi-Fi and Bluetooth firmware, the touch
controller's firmware, the camera sensor tables and the signed zap shader are the vendor's: they come from your own phone and
stay out of git. The Wi-Fi password and its PMK are never in a boot argument or a log: the PMK is a file inside your own root
image, `/redwood/etc/vayu-wifi/wifi.pmk` (`vayu_wifi_pmk_file=1`; `userland/build.sh` takes it from `fw-wifi/wifi.pmk`, 64 hex
digits as `wpa_passphrase` prints them; `SHARE=1` leaves it out).

Nor is anything of the network the port was made on. What is yours is given at run time:

- the kernel's own Wi-Fi join (`vayu_wifi_join=1`): `vayu_wifi_ssid=NAME`; `vayu_wifi_bssid_tail=XX:XX` to talk only to the
  access point whose BSSID ends so (without it, any of that name on 5 GHz); `vayu_wifi_pmk_name=FILE` for a key file of
  another name in the same directory. Without `vayu_wifi_ssid` there is no boot join; joins from Settings work as before;
- mobile data: `vayu_cell_apn=APN`; without it the SIM's default profile;
- the reverse SSH tunnel to your own machine (`userland/src/tunnel.c`): `tunnel_host=` and `tunnel_user=`, or
  `ssh/hakios-tunnel.conf` at the top of the checkout, beside the tunnel's key and outside git; without a host it does
  nothing;
- the scripts that talk to the phone over adb or fastboot want its `SERIAL=` and assume none.

The Metal plug-in (`userland/src/HakiosMetalCA.m`, MIT; begun from vcvkk's plug-in, whose author allowed MIT on
2026-11-21) is built with `METAL=1 userland/build.sh`.

## Building and running

Building on Linux: [`docs/building.md`](../../docs/building.md). With the kernel, the loader and the images built:

```sh
devices/poco-x3-pro/uefi/build.sh                     # the UEFI with the XNU hook -> Mu-Silicium/Build/vayuPkg/...
devices/poco-x3-pro/pull-firmware.sh                  # the phone in Android, on adb -> fw-poco-x3-pro/
DEVICE=poco-x3-pro REST=1 userland/build.sh           # root.apfs and system.apfs with this phone's extras
export DEVICE=poco-x3-pro
./xl enter                                            # fastboot-boots the UEFI, which starts the loader
./xl ramdisk userland/system.apfs                     # the image into the phone's RAM
XL_CMDLINE="bootcpu=7 cpus=8 fb_threads=6 vtimer_intid=19 touchscreen=1 nvt_qup=1 gpu=1 vayu_gpu=1 vayu_keep=1" ./xl run
./xl reboot                                           # back to Android
```

`xl` and `mkxnuimg.py` read `boot.conf` for the platform, the UEFI target and the UEFI image's address, so
`DEVICE=poco-x3-pro` is all they need. Nothing is flashed: the phone is booted with `fastboot boot`.

`tools/darwin-install.sh` and `tools/standalone.sh` are the beginning of an installation that boots by itself: the iOS volume
written into the phone's partition `darwin` from the recovery, and an image in `boot`. They write to the phone, so each write
wants `DARWIN_WRITE=yes` or `FLASH_BOOT=yes` and the phone's `SERIAL=`; `tools/install-test.sh` runs them against a phone made
of files. On the phone only their first steps have been done (2026-11-18: `boot` saved, the layout block written into
`darwin`); the iOS volume and the standalone image have not been written or started.

Boot arguments that matter on this phone:

- `vtimer_intid=19`: the SM8150 wires the arch timer's virtual PPI to INTID 19 (the kernel's default, 27, is the POCO X5 Pro's
  and QEMU's); without it no timer interrupt arrives and the kernel stops at the first wait. `bootcpu=7 cpus=8`: boot on the
  last core, use all eight. `fb_threads=6`: six processors share the frame copy onto the panel.
- Each `vayu_*` driver has a switch of its own (`vayu_audio=1`, `vayu_camera=...`, `vayu_modem=1`, `vayu_rmtfs=2`,
  `vayu_cell=2`, `vayu_wifi=1`, `vayu_bt=1`, `vayu_nfc=1`, `vayu_ir=1`, ...), and the head of its file lists the rest. Anything
  that writes to a pin controller outside what the loader prepared is behind an explicit opt-in (`..._xnu_tlmm=1`).
- The cellular bring-up is opt-in step by step: `vayu_cell_online=1`, `vayu_cell_provision=1`, `vayu_cell_register=1`,
  `vayu_cell_data=1`. Mobile data is known to hang the SoC; leave `vayu_cell_data` off.
- `vayu_sdp_slow=0` lets the CPU run at full speed on a USB data port at a low charge; the battery then drains.

## The SoC addresses

`soc.args` is what `tools/soc-args.py` prints for `prebuilt/device-tree.dtb` (the same procedure as in
`../poco-x5-pro/README.md`). The `vayu_*` drivers carry vayu's addresses in their own source, with the device-tree node each
came from, and do not read `soc_*` arguments; the file is kept as the worked example and for the shared modem and IPA code,
which does read them.

## Partition table

`tools/gpt-plan.py` previews, from dumps of the first and last blocks of the phone's UFS unit, the change that makes room for
the port's own partition for the iOS volume (`show`, `compare`, `plan`); it writes no device. The loader refuses any block write
except into that partition, past a block named in `xnuloader/loader.c` (`darwin_vayu`), and only after checking the partition's
whole GPT entry; and, only when the image it was started from says `flash_stage=1`, a boot image staged there by iOS into
`boot` (`boot_vayu`, by its GPT entry), keeping the old one to put back if a piece does not read back.

## Credits

HakiOS and its POCO X5 Pro port are the work of OOSEX GANG and contributors
([oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)); this port is built on them. The UEFI platform is
[Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)'s `vayuPkg`. Parts of the WCN3990 bring-up are ported from
ios-flame (Pixel 4, SM8150; MIT, Copyright (c) 2026 dipa-studio), and the SCM call lock from the Redmi K20 Pro port
(`hackios_Raphael`). The standalone boot, `/private/var` on HFS Plus and the A640 Metal work follow the other POCO X3 Pro
port, `telegdyd/ios-vayu` (`vayu-dev`); smaller parts come from Scion and the forks by xreactx, janhurn9400, telegdyd,
kiwiiiii-eth and adelholzener69. Each file says where, with the branch and the commit. Third-party code and its licences: [`NOTICE`](../../NOTICE),
[`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md).
