# Аппаратный трек камеры и медиа vayu (SM8150: Spectra 380 + Venus IRIS1)

**Вердикт**: Пересмотр отклонения HW-блоков из `camera-master-plan.md:9-23`. Переходим на **гибридный план**: поднимаем HW-блоки по одному с проверяемым гейтом на каждом шаге; CPU NEON (`camera_isp.c`) и GPU GL ES (`Spec N`) остаются живым фоллбэком (`boot-args`) и эталоном сравнения.

## 1. Оценка аппаратных блоков (FACT / INFERENCE / HYPOTHESIS)
1. **Venus / IRIS1 (видеокодек `0x0aa00000`, IRQ SPI 174)** — **ВЫСШИЙ ПРИОРИТЕТ**:
   - **FACT**: В mainline `drivers/media/platform/qcom/venus/core.c:674-824` есть HFI 4xx (`sdm845`) и HFI 6xx (`sm8250`), но SM8150 — это **IRIS1 / HFI 4xx (v4)** (`recovery-live.dts:25210-25296`, `hfi_venus_io.h:18-78`: wrapper `+0xe0000`, CPU CS `+0xd2000`, CPU IC `+0xdf000`). Прошивка `firmware/vayu/modem/venus.{mdt,b00..b04}` (PAS ID `9`, `pil_video_region@0x97600000`, `0x500000`, `VIDEO.IR.1.2-00065-PROD-4`).
   - **FACT**: **Этап M1 уже написан в XNU** (`bsd/dev/vayu_venus.c:1-2071`, `bsd/sys/vayu_venus_hfi.h:1-420`, host-тест `tools/venus-hfi/hfi_test.c` — 84/84 PASS, `V-report.md:230-245`): RPMh `mmcx/mx`, BCM `MM3/SH0/MC0`, `video_cc` PLL0 (`0xab0042c`), GDSCs `mvsc/mvs0/mvs1` (`0xab00814/874/8b4`), SMMU `SID 0x2300/0x60` (IOVA `0x25800000`, 38 МБ IPC: SFR + cmd/msg/dbg очереди), PAS-загрузка через `vayu_pas.c:1300-1490` и `HFI_CMD_SYS_INIT`/`PING`.
   - **INFERENCE**: Для подачи кадров и съёма HEVC/H.264 нужны M2–M3 (`SESSION_INIT 0x10007` domain `ENC=1`, `SESSION_SET_PROPERTY`, `SESSION_SET_BUFFERS`, `SESSION_LOAD_RESOURCES 0x211001`, `SESSION_START 0x211002`, `EMPTY_BUFFER 0x211004` с NV12 IOVA, `FILL_BUFFER 0x211005` → Annex-B NAL в `0x221008`, `VENUS_DESIGN.md:433-470`). В iOS (`userland/src/inject_media.c:406-441`) подключается через `/dev/hakios-venus` и `VTRegisterVideoEncoderWithInfo('hvc1'/'avc1')` в `mediaserverd` — это ускоряет **и запись видео 1080p30, и съёмку фото HEIC (`CMPhoto`)**.
2. **IFE0 Full (PIX path `vfe0@0x0acaf000` + `csid0@0x0acb3000`, IRQ SPI 464/465)**:
   - **FACT**: В `recovery-live.dts:613-627,4116-4132,4412-4455` у `vfe0` (`qcom,vfe175`) свой `ife_0_gdsc` (`0x0ad0a004`, без hw-trigger) и клоки `ife_0_clk_src/clk/axi_clk/cphy_rx_clk` (`VAYU_CAMERA_PHASE0.md:120-121`), но **та же SMMU-группа `msm_cam_smmu_ife` (`SID 0x840..0xe60`, mask `0x620`)**, что уже работает в `vayu_camera.c`. По `camss-vfe-170.c:17-310` шина записи (`0x2000..0x25ff`) и RDI0..3 идентичны `vfe-lite0`.
   - **FACT**: PIX-тракт (`CAMIF 0x478..0x4ac` → цветной конвейер `0x400..0x1fff` → парные WM Y+UV) отдаёт готовый **NV12 в RAM при 0% CPU**. Коэффициенты всех модулей (`bls12`, `lsc34`, `pdpc20`, `bpcbcc50`, `gic30`, `demosaic36`, `cc13`, `gtm10`, `gamma16`, `cs20`) уже извлечены из `com.qti.tuned.vayu_sunny_imx582.bin` (6122 символа, `J-camtuning.md:20-58`).
   - **INFERENCE**: В открытом `cam_vfe` (`vayu-r-oss`) и `camss-vfe-170.c` описаны только top/CAMIF/BUS/IRQ (~150 рег.), а оффсеты модулей `0x400..0x1fff` (~600–900 рег. с LUT) зашиты в `vendor/ex/lib64/hw/camera.qcom.so` (6.49 МБ). Поэтому подъём идёт в 3 шага: **H2a** `vfe0` RDI0 RAW10 → **H2b** PIX с bypass модулей (`MODULE_CFG=0`, только Demosaic+CSC+WM Y/UV) → **H2c** включение модулей по одному со сверкой PSNR против `camera_isp.c`.
3. **CDM (`cpas-cdm0@0x0ac48000`, IRQ SPI 461, SMMU `SID 0x2000`)**:
   - **FACT**: Аппаратный Command DMA без прошивки (`recovery-live.dts:4209-4225,4377-4403`, клиенты: `vfe0/1`, `jpegenc`, `lrme`). Читает из IOVA буферы команд (`REG_CONT`, `REG_RANDOM`, `GEN_IRQ`) и атомарно пишет регистры IFE по SOF/EOF.
   - **INFERENCE**: Нужен только на этапе **H2c**, когда в `vfe0` обновляются сотни регистров LUT (`lsc34`/`gamma16`) на лету; до этого проще прямой MMIO.
4. **JPEG HW Encoder (`jpegenc@0x0ac4e000`, IRQ SPI 474, SMMU `SID 0x2180/0x21a0`, mask `0x20`)**:
   - **FACT**: Чисто регистровый блок **без прошивки и МК** (`recovery-live.dts:4134-4150,4666-4689`), питается от `titan_top_gdsc` (`0xad0c1bc`), клоки `cam_cc_jpeg_clk` (600 МГц), ~50 регистров (FE PLN0=Y / PLN1=UV, WE bitstream, DQT 2×64, Huffman, IRQ).
   - **INFERENCE**: На вход требует **уже готовый NV12** (не RAW10!), а `Camera.app` в iOS 16.5 сохраняет фото в **HEIC через HEVC** (`inject_media.c:406-413`). Поэтому ценность ниже Venus; полезен для быстрых JPEG-снапшотов в `camera_test` / `AVCapturePhotoOutput`.
5. **LRME (`lrme@0x0ac6b000`, IRQ SPI 476, SMMU `SID 0x20e0/0x2160`)**:
   - **FACT**: Регистровый блок оценки движения для EIS (`recovery-live.dts:4251-4275,4741-4765`, настройки `mod_lrme10` в `.bin`). **INFERENCE**: В `inject_media.c:498-501` стабилизация выключена; поднимать только после стабильного 1080p30 через Venus + IFE0.
6. **ICP (Cortex-A5 `0x0ac00000`, IRQ SPI 463) + BPS (`0x0ac6f000`) / IPE0/1 (`0x0ac87000/0x0ac91000`)**:
   - **FACT**: Прошивка — `vendor/ex/firmware/CAMERA_ICP.elf` (3.6 МБ ELF32 ARM, НЕ TZ PAS `.mdt`), грузится в `camera_region@0x8b700000` с маппингом в **IOVA `0x0..0x500000`** на `SID 0x2222` (`recovery-live.dts:4152-4172,4582-4664`).
   - **REJECTED (до закрытия P4/P5)**: A5 ждёт закрытые stripe-пакеты CDM+UBWC (сотни КБ тайловых дескрипторов `anr10`/`tf10`/`asf30`/`ltm13` на кадр), которые строит `camera.qcom.so`. Реверс stripe-калькулятора нерентабелен при наличии IFE0 PIX + CPU/GPU post-ISP.

## 2. Ранжирование «Ценность / Сложность» и порядок гибридного трека
| Ранг | Блок | Ценность | Сложность | Роль в гибридном конвейере (фоллбэк = CPU NEON / GPU GL ES) |
| :--- | :--- | :--- | :--- | :--- |
| **1** | **Venus / IRIS1** (`0x0aa00000`) | Максимальная (HEVC видео + HEIC фото, ~0% CPU) | Низкая-средняя (M1 уже в дереве) | Заменяет x264/SW-HEVC в `VideoToolbox`; фоллбэк — SW кодер `mediaserverd` |
| **2** | **IFE0 Full PIX** (`0x0acaf000`) | Высокая (30 fps RAW10→NV12 без CPU/GPU) | Средняя (H2a/H2b) → Высокая (H2c) | Заменяет `camera_isp.c` / GL ES шейдеры на превью/видео; фоллбэк — `ife-lite0` |
| **3** | **CDM0** (`0x0ac48000`) | Средняя (атомарный апдейт LUT IFE0 по SOF) | Низкая (~25 рег., без FW) | Вспомогательный DMA для этапа H2c (`vfe0`); фоллбэк — прямой MMIO |
| **4** | **JPEG ENC** (`0x0ac4e000`) | Умеренная (NV12→JPEG за <5 мс без CPU) | Низкая-средняя (~50 рег., без FW) | Ускоряет JPEG stills в `camera_test` / `AVCapturePhotoOutput`; фоллбэк — ImageIO |
| **5** | **LRME** (`0x0ac6b000`) | Нишевая (векторы движения для EIS) | Средняя (FE/WE DMA + матрицы) | Опциональная стабилизация видео после 1080p30 |
| **6** | **ICP A5 + BPS/IPE** | Низкая (дублирует IFE0 PIX + CPU/GPU NR) | Экстремальная (реверс CamX UBWC) | **Отклонено** (оставить `camera_isp.c` Spec L для 12 МП оффлайн-фото) |

**Рекомендуемый порядок**: `1. Venus M1 (железо) → M2/M3 (1 кадр HEVC) → M4 (VideoToolbox)` $\rightarrow$ `2a. IFE0 RDI0` $\rightarrow$ `2b. IFE0 PIX Bypass (NV12)` $\rightarrow$ `2c. IFE0 PIX модули + 3. CDM0` $\rightarrow$ `4. JPEG ENC` $\rightarrow$ `5. LRME`.

## 3. ТЗ для исполнителя (DeepSeek) по первым двум блокам

### ТЗ Блок 1 (`DS-HW-VENUS`): Venus / IRIS1 — от M1-пинга до кодирования NV12 $\rightarrow$ HEVC/H.264
- **Входные файлы**: `bsd/dev/vayu_venus.c`, `bsd/sys/vayu_venus_hfi.h`, `work/hkd-ws20/tools/venus-hfi/hfi_test.c`, `userland/src/inject_media.c`, референс Linux `drivers/media/platform/qcom/venus/{hfi_cmds.c,hfi_msgs.c,venc.c}` и `VENUS_DESIGN.md:433-470`.
- **Задачи**:
  1. **Шаг V1 (Проверка M1 на железе)**: Положить `venus.{mdt,b00..b04}` в `/fw` рамдиска, загрузить ядро с `vayu_venus=1` (1 прогон) и снять `/dev/klog`.
  2. **Шаг V2 (M2/M3 — кодирование 1 кадра в ядре, `vayu_venus=2`)**: Включить `mvs0` GDSC (`vayu_venus_gdsc=3`), реализовать в `vayu_venus.c` открытие сессии `HFI_CMD_SYS_SESSION_INIT` (`0x10007`, `ENC=1`, `HEVC=0x2000` / `H264=0x2`), установку свойств (`FRAME_SIZE` 1920×1080, `FRAME_RATE`, `BITRATE`, `COLOR_FORMAT=NV12`), запрос `BUFFER_REQUIREMENTS` (`0x202001`), аллокацию IOVA в пуле `0x27e00000+` (`SID 0x2300`), `SESSION_SET_BUFFERS`, `SESSION_LOAD_RESOURCES` (`0x211001`), `SESSION_START` (`0x211002`), подачу тестового NV12 (`EMPTY_BUFFER 0x211004`) и забор битового потока (`FILL_BUFFER 0x211005` $\rightarrow$ `MSG_FILL_BUFFER_DONE 0x221008`). Сначала — поллингом `WRAPPER_INTR_STATUS` (`+0xe000c`), затем — IRQ SPI 174.
  3. **Шаг V3 (M4 — `/dev/hakios-venus` + VideoToolbox)**: Экспортировать `/dev/hakios-venus` (`ioctl` config + write NV12 / read Annex-B) и подключить в `inject_media.c:415-441` через `VTRegisterVideoEncoderWithInfo`.
- **Артефакты**: патч `vayu_venus.c`, дамп `/tmp/venus_out.h265`, проверка `ffprobe /tmp/venus_out.h265` на хосте.
- **Критерий успеха (строки в логе ядра `/dev/klog`)**:
  - Для V1: `vayu-venus: 1: HFI: SYS_INIT done: error 0`, `vayu-venus: 1: HFI: FIRMWARE VERSION: "VIDEO.IR.1.2-00065-PROD-4..."`, `vayu-venus: 1: HFI: ping ack: cookie 0xbeef`, `vayu-venus: 1: PASS`.
  - Для V2: `vayu-venus: 2: HFI: SESSION_INIT done`, `vayu-venus: 2: HFI: FILL_BUFFER_DONE flags=0x... filled_len=... (nal: VPS/SPS/PPS/IDR), smmu_faults=0`.

### ТЗ Блок 2 (`DS-HW-IFE0-PIX`): Полный IFE0 (`csid0` + `vfe0`) — от RDI0 к PIX NV12
- **Входные файлы**: `bsd/dev/vayu_camera.c`, `camss-vfe-170.c`, `recovery-live.dts:4412-4455`, `com.qti.tuned.vayu_sunny_imx582.bin` (`wJ/ref/qti131.py`), дизассемблер `vendor/ex/lib64/hw/camera.qcom.so`.
- **Задачи**:
  1. **Шаг H2a (`vayu_cam_ife=0`, RDI0 на полном `vfe0`)**: Включить `ife_0_gdsc` (`0x0ad0a004`), клоки `ife_0_clk_src/clk/axi_clk/cphy_rx_clk/csid_clk_src`, направить `csIPHY1` $\rightarrow$ `csid0` (`0x0acb3000`) $\rightarrow$ `vfe0` (`0x0acaf000`) RDI0 (SMMU `SID 0x840` уже настроен!). Доказать 100% паритет с `ife-lite0` на RAW10.
  2. **Шаг H2b (`vayu_cam_ife=2`, PIX Bypass $\rightarrow$ NV12)**: Включить в `csid0` порт **IPP** (`CSI2_RX_CFG1` / IPP path), в `vfe0` настроить `CAMIF_CFG` (`0x47c`) + размеры окна (`0x494..0x4a4`), оставить все цветные субмодули в bypass (`MODULE_CFG=0`), кроме базового Demosaic + CSC (`RGB->YCbCr`), и настроить два Bus WM (WM0 = Y plane, WM1 = UV interleaved plane, `MODE_QCOM_PLAIN`) с выводом готового NV12 1920×1080 в RAM при 0% CPU.
  3. **Шаг H2c (Модули по одному)**: Извлечь из `camera.qcom.so` оффсеты субмодулей внутри `vfe175` (`0x400..0x1fff`) и включать по одному: `bls12` (чёрный) $\rightarrow$ WB/Gain $\rightarrow$ `cc13` (матрица 3×3 из `.bin`) $\rightarrow$ `gamma16` (LUT 64×3) $\rightarrow$ `lsc34` (сетка 17×13), на каждом шаге сверяя кадр с эталоном `camera_isp.c`.
- **Артефакты**: патч `vayu_camera.c`, таблица оффсетов `docs/research/VAYU_VFE175_PIX_REGS.md`, дампы `/tmp/ife0_rdi.raw10` и `/tmp/ife0_pix.nv12`.
- **Критерий успеха (строки в логе ядра `/dev/klog`)**:
  - Для H2a: `vayu-camera: ife0: hw_version=0x10070050 ife_0_gdsc=ON`, `vayu-camera: ife0-rdi0: 1920x1080 RAW10 frame_done=10 smmu_faults=0`.
  - Для H2b/c: `vayu-camera: ife0-pix: CAMIF 1920x1080 -> NV12 (WM0 Y + WM1 UV) 29.9 fps, cpu_isp=0%, psnr_vs_cpu=37.4dB, smmu_faults=0`.
