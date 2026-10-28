<!-- Made by tools/docs-gen.py from devices/vayu/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# POCO X3 Pro (`vayu`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `sm8150` |
| Configuration | `make vayu_defconfig` |
| Maintainer | nomorecoolnicknames (README.md) |
| Repository | [`devices/vayu/`](https://github.com/oosexgang/hakios/tree/main/devices/vayu) |
| Firmware | `devices/vayu/pull-firmware.sh` pulls it from your own phone |

## What works

| Part | State | |
|---|---|---|
| display | works | the panel through UEFI's framebuffer, 1080x2400 (vayu_vsync, vayu_wled) |
| touch | works | Novatek NT36675 on QUP SPI (GPI DMA), multitouch (vayu_nvt, vayu_qup_spi, touchscreen=1 nvt_qup=1) |
| buttons | works | power and volume through PM8150 / PM8150L PON and GPIO over SPMI (CONFIG_HAKIOS_BUTTONS) |
| cpu | works | all 8 Kryo 485 cores up to 2.96 GHz, OSM / EPSS LUT (vayu_cpufreq, cpus=8 bootcpu=7 cpufreq=1) |
| gpu | works | Adreno 640 hardware-accelerated rendering via Mesa freedreno FD640 (vayu_adreno, gpu=1 vayu_gpu=1 vayu_keep=1) |
| storage | partly | UFS read through the loader and /private/var R/W work (ufs=2 var_store=1 ufs_write=1); Files.app does not work yet |
| audio | partly | ADSP PIL + TI TAS2562/TAS2564 speaker and microphone work (vayu_audio); 3.5 mm headphone jack (WCD934x, vayu_jack) is silent |
| battery | partly | PM8150B over SPMI; does not charge under XNU and drains quickly, fuel gauge unverified |
| haptic | no | driver in tree (vayu_haptic), not verified on hardware |
| camera | partly | main camera (IMX582) still photos and ~7.6 fps video work (vayu_camera); other sensors not verified on hardware |
| torch | no | driver in tree (hakios.torch), not verified on hardware |
| wifi | no | WCN3990 WLAN DSP crashes at PC=0xb0193960 before FW_READY; WMI/HTT/CE not written (vayu_wifi) |
| bluetooth | no | WCN3998 UART HCI: bluetoothd restarts in a loop on Apple vendor commands (TEST23-BT, vayu_bt) |
| cellular | partly | MPSS PIL + rmtfs + QMI DMS/UIM/NAS: LTE registration works (vayu_cell); mobile data (IPA) hangs the SoC |
| network | partly | USB networking only (DWC3 CDC-ECM/NCM en0, vayu_usbnet); no Wi-Fi or cellular data |
| thermal | no | driver in tree (CONFIG_HAKIOS_THERMAL), not verified on hardware |
| usb | works | Synopsys DWC3 peripheral mode, loader channel and USB networking |
| sensors | no | SLPI SSC and SPI IR blaster drivers in tree (vayu_ssc, vayu_ir), not verified on hardware |
| nfc | no | NXP PN553 on I2C (vayu_nfc), not working / not verified on hardware |

The state is what has been seen on this device, written down in `devices/vayu/device.conf`; a part the table does not name has not been looked at.
