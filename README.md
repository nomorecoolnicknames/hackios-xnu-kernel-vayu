# HakiOS for the POCO X3 Pro (`vayu`)

HakiOS on the **Xiaomi POCO X3 Pro** (`vayu`): Qualcomm Snapdragon 860 (**SM8150-AC**), Adreno 640, UFS. The iOS 16.5 user
space (launchd, backboardd, SpringBoard, the stock apps) runs on Apple's open-source XNU kernel (xnu-8796.121.2, Darwin
22.5.0) with this port's boot loader and drivers.

The phone is started with `fastboot boot` and HakiOS runs from RAM: a reboot brings back the phone's own Android. The boot
loader must be unlocked. This is a research bench with known defects, not a daily system.

This repository is a fork of [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) by OOSEX GANG, from before the common
repository was split into device repositories; the POCO X5 Pro (`redwood`) port it started from is still in
`devices/poco-x5-pro/`.

## What works

On the lab's builds k17 to k33-daily (2026-10-26 to 2026-11-06). The last column is two runs of 2026-11-18 from RAM with the
lab's usual arguments: k33-daily rebuilt from the lab's surviving patches (773 s), and the merged kernel of this line before
this branch's last changes (1368 s); a dash is a part neither run looked at.
The full table, with what was seen in which run and how sure each line is, is in
[`devices/poco-x3-pro/README.md`](devices/poco-x3-pro/README.md); [`device.conf`](devices/poco-x3-pro/device.conf) has the
same in the common repository's words.

| Part | State | In short | 2026-11-18 |
|---|---|---|---|
| Boot to SpringBoard | works | lock screen, home screen, the stock apps; no watchdog, a hung run needs a power-cycle | SpringBoard, no panic (both) |
| Display | works | the panel at 120 Hz; smoothness under the finger not confirmed | - |
| Touch | partial | multitouch works; apps get about 30 events/s on a continuous drag | - |
| Buttons | works | power and volume | - |
| GPU | partial | Adreno 640 through Mesa (OpenGL ES); no Metal | - |
| Wi-Fi | partial | 5 GHz WPA2, DHCP, Safari; one run dropped at the group-key rekey; 2.4 GHz not tested | an address by DHCP (both); the merged kernel kept the link 20 min, but the access point did not change the group key in that time |
| Bluetooth | partial | devices found; no pairing | the stack up (k33-daily) |
| Audio | partial | speaker and microphone; the headphone jack is silent | - |
| Cameras | partial | main camera live in Camera.app; stills from a ~2 MP frame | 30 fps (k33-daily) |
| Cellular | partial | the modem registers on LTE; no calls, SMS or mobile data | LTE registration (both) |
| Charging | no | the phone drains on USB under load | 60 % to 56 % in 23 min on USB (merged kernel) |
| Sensors, GNSS, Files, AirDrop | no | | - |

**This branch.** The kernel here is the k33 line: k33-daily rebuilt from the lab's surviving patches, with later work on
top (HFS Plus for `/private/var` on the flash, fixes from a review of the drivers, parts taken from other ports, power and
clock options). Everything new that touches the hardware differently is behind a boot argument and off by default. The
tree builds and boots in QEMU. The merged kernel of the last column is this tree without its last changes (the Metal
plug-in's names, inert without `metal=`, and the network and operator values moved to boot arguments, below), which have
not been run on the phone yet; none of the new boot arguments was turned on in those runs.

Your own network and operator are not in the code. The kernel's own Wi-Fi join wants `vayu_wifi_ssid=` (and, to talk to
one access point only, `vayu_wifi_bssid_tail=XX:XX`) and reads the key from `/redwood/etc/vayu-wifi/wifi.pmk`
(`vayu_wifi_pmk_file=1`; `userland/build.sh` takes it from `fw-wifi/wifi.pmk`, outside git); mobile data takes the APN
from `vayu_cell_apn=`, without it the SIM's default profile.

**Since 18 November.** The kernel in `xnu/` is now the line the daily images are built from: the third camera, NFC
(PN553), 120 Hz and the boot menu were seen on the phone with it on 19 November. On top of that come the cell work (the
IPA's GSI rings and microcontroller, the modem's QMI requests, WDS), telephony (the kernel's own QMUX services, rmtfs),
the charger's watchdog and limits (`vayu_charge=1`) and the IPA filter answers (`vayu_ipa_flt_ack=1`), each behind its
boot argument and off by default. This merged tree builds and boots in QEMU; as a whole it has not run on the phone.
The boot loader has a boot menu (HakiOS, Android, other EFI applications, a diagnostics entry; or rEFInd with
`boot_menu=refind`) and keeps count of failed starts in misc's boot control block.

**Metal.** The Metal driver for the Adreno 640 (`hakios-metal/`, `METAL=1`) is here without its plug-in: that file was begun
from a repository that states no licence, and it waits for its author's permission. `METAL=1` stops and says so; the
system draws through OpenGL ES as before.

## iOS 27 and macOS 13.4

Two more kernel lines are here, each a patch and new files over the tree it starts from (`xnu/line.sh`, `xnu/lines/`):

- **iOS 27** (`xnu/lines/ios27/`, xnu-12377 on Scion's iOS 27 kernel). On the phone iOS 27's user space starts from a root
  served over the USB cable (NBD), about a hundred processes. A kernel bug that made most daemons kill themselves (iOS
  27's `posix_spawn` attributes read in an older layout) is fixed (`vayu_spawnattr=27`). SpringBoard currently
  crash-loops on SIGABRT, and there is no picture of the system on the panel yet.
- **macOS 13.4** (`xnu/lines/macos13/`, xnu-8796.121.2). On the phone macOS boots with its volumes on NBD disks;
  WindowServer, the Dock and the menu bar run, drawn by the Adreno 640 through HakiosMetal, and the desktop comes up on
  the panel (portrait, HiDPI 2x). The wallpaper's window is on screen but its picture is not drawn yet; touch and the
  cursor do not work yet.

Pictures of the panel come from the kernel's log: `vayu_fbthumb=N` writes a thumbnail every N seconds and
`tools/fbthumb2png.py` turns a log back into PNG files. The screenshots themselves are not in the repository (they show
Apple's artwork). The user spaces of these two lines (how their system images are made) are not in this branch yet.

## What is here

- `xnu/lines/`: the iOS 27 and macOS 13.4 kernel lines, laid over their bases by `xnu/line.sh apply LINE`.
- `xnu/`: the kernel, as a patch to Apple's open-source XNU (`hakios.patch`) and new files (`files/`), laid over Apple's tag
  by `xnu/apply.sh`, which also fetches Apple's HFS Plus and lays `xnu/hfs/hfs.patch` over it. This phone's drivers are `xnu/files/bsd/dev/vayu_*.c`; the head of each says where its numbers come
  from and lists its boot arguments.
- `xnuloader/`: the boot loader, a UEFI application.
- `userland/`: the programs and libraries injected into iOS, the launchd jobs, the APFS image build.
- `devices/poco-x3-pro/`: this phone: its UEFI build (Mu-Silicium's `vayuPkg` with the XNU hook), device tree, firmware
  script, user-space extras, tools.
- `devices/poco-x5-pro/`: the POCO X5 Pro, where HakiOS began.
- `tools/`, `qemu/`, `sysimage/`, `xl`, `mkxnuimg.py`: host tests, QEMU, the scripts that take iOS out of Apple's IPSW on your
  machine, the USB front end.
- `prebuilt/`: a kernel, loader and libraries the HakiOS developers built for the POCO X5 Pro (2026-10-25), kept from the
  common repository; they are not this port's builds, and `xl` uses them only when nothing is built.

## Not here, by design

- No file of iOS: no binary, image, cache, font, picture or screenshot. `sysimage/fetch.sh` fetches the iOS 16.5 IPSW from
  Apple's servers to your machine and unpacks it there; nothing is decrypted or circumvented.
- No firmware of the phone: GPU, Wi-Fi, Bluetooth, audio DSP, modem, cameras and touch are the vendor's. `pull-firmware.sh`
  takes them from your own phone into `fw-*/`, which git ignores.
- No keys, no activation, nothing that makes the phone pass as an iPhone to Apple's services. Apple ID and the App Store
  do not work and are not a goal.

`python3 tools/provenance-check.py` checks this on the tree.

## Building and running

- Building on Linux: [`docs/building.md`](docs/building.md).
- Running on the phone: [`devices/poco-x3-pro/README.md`](devices/poco-x3-pro/README.md#building-and-running).
- Without a phone, in QEMU: `make qemu-open` (nothing of Apple's), or `qemu/run-ios.sh` with the images from
  `userland/build.sh`.

## Other repositories of this port

| Repository | What |
|---|---|
| this one | the port: kernel, loader, user space, tools, this phone's directory |
| [hakios-sm8150](https://github.com/nomorecoolnicknames/hakios-sm8150) | the same phone as a device repository in the common repository's newer layout (`device.conf`, `configs/`, `soc/`), from 2026-10-29; behind this one |
| [hackios-xnu-userland-vayu](https://github.com/nomorecoolnicknames/hackios-xnu-userland-vayu) | early notes; the user space is in `userland/` here |

## Credits

HakiOS, its loader, user space and the POCO X5 Pro port are the work of OOSEX GANG and its contributors. This port also
carries code from other ports, each named with its branch and commit in the file it is in and in [`NOTICE`](NOTICE):
ios-flame (Pixel 4, SM8150; the WCN3990 bring-up), the Redmi K20 Pro port (`hackios_Raphael`; the SCM call lock), the other
POCO X3 Pro port `telegdyd/ios-vayu` (standalone boot, `/private/var` on HFS Plus, the A640 Metal work), Scion, and the
forks of HakiOS by xreactx, janhurn9400, telegdyd, kiwiiiii-eth and adelholzener69. The UEFI is
[Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)'s.

## Licenses

The kernel patch and the files built into XNU are under the Apple Public Source License 2.0 (`LICENSE.APSL`), as a
derivative of Apple's XNU; everything else is under the MIT License (`LICENSE`), except third-party code, which keeps its own
licence (`NOTICE`, `THIRD_PARTY_LICENSES.md`). `LEGAL.md` says what is and is not distributed, `COMPLIANCE.md` how the
licences' terms are met.

HakiOS is an independent research project, not affiliated with or endorsed by Apple Inc. iOS, iPhone, XNU and Apple are
Apple's trademarks, named only to say what the code works with. If you believe a file here infringes your rights, open an
issue and it will be looked at and, where warranted, removed.
