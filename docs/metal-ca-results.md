# Task 11: CoreAnimation's window server on Metal

*From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (MIT).*

Run `metal-step11`, 2026-11-07. Image on the phone: `boot` 4cb4ccbf (task 9's, boot-arg `metal=1`). Nothing was
flashed. Every Metal run below is backboardd on the plug-in built from this tree, switched at run time (see "How").
Phone log excerpts: `docs/metal-step11-phone.log`. Full logs and every screenshot: ignored `logs/metal-step11/`.
Screenshots here: `docs/metal-step11/`.

## Result

**FACT (screenshots read from the IOSurface the framebuffer shows in layer 0, `tools/metal-ca-shot.sh`).** backboardd
(QuartzCore's render server) draws the whole UI through `HakiOS Adreno 640`, with no `hakios-gl` frame after the
switch. Run `final`, backboardd 2057, default plug-in:

| Screen | Metal | GL (same session) |
|---|---|---|
| Home screen (widget, dock and Search pill blurred) | `f-home.png` | — |
| Settings, a pushed page (General); taken in run try3, on the variant path | `m-settings.png`, `m-general.png` | — |
| App switcher (blurred backdrop, live cards, tap a card to switch) | `f-switcher.png` | `g-switcher2.png` |
| Safari (toolbar material) | `f-safari.png` | `g-safari.png` |
| Control Centre (material modules, sliders, glyphs) | `f-cc.png` | — |

- `metal_test app` (task 9, 24 checks) and `metal_test astc` (72 checks) pass on the same bundle.
- backboardd and SpringBoard kept their PIDs through each session: no crash, no refusals in the log.
- The framebuffer gets 10 frames/s idle (as under GL) and more while animating.
- **GL is one switch away (FACT, run `gl3`):** `tools/metal-ca-run.sh off` restarts backboardd, which is refused the
  device (`metal=1`), and `hakios-gl` frames come back. A reboot does the same, since everything the switch changes is
  in RAM or `/private/var/tmp`.
- The user watched the panel during the runs. They pointed out that Control Centre's modules were still loading when
  the first shot was taken (cold shader compiles). With warm caches it opens complete (`f-cc.png`).

## Two defects found and fixed: CoreAnimation's precompiled variants

The plug-in came from upstream with a rule for an iPhone X image whose QuartzCore had no `default.metallib`. For each
CoreAnimation uber shader it compiled one of 451 precompiled variants from `default.pipelinelib`, chosen by label.
When a pipeline's variant was missing, it used the nearest one as a stand-in. Through that path on vayu:

- **Safari's toolbar material** came out as a grey band reaching about 50 pt above the toolbar (`m-safari.png` against
  `g-safari.png`).
- **The app switcher card from Safari's launch snapshot** came out one solid colour: red in one run, blue in the next
  (`m-switcher6.png`; the snapshot JPEG on disk is white). Diagnosed on the way:
  - The A640 decodes ASTC correctly, void-extent blocks and the 1125x2436 size included (`metal_test astc`, 72/72).
  - Half-precision sampling and CoreAnimation's alpha-forcing `insertelement` compile and run right (same test).
  - The ASTC texture holds exactly the bytes uploaded, and those bytes are already the solid colour. So the picture
    went wrong before the upload, inside CoreAnimation's own handling of the snapshot.

This image's QuartzCore **does** carry `default.metallib`, with the real uber shaders. The plug-in now compiles the
function CoreAnimation asks for, specialised by CoreAnimation's own constants (`hm_bitcode_for`). Both defects are gone
(`f-safari.png`, `f-switcher.png`). Cold cost: 20 stages compiled for 93 pipelines, 15.5 s in all, 1.1 s at most for
one. The persistent disk cache serves them afterwards. `metal_ca_variants=1` brings back upstream's variant path.

Also changed, though it did not cause the bad card: a surface drawn through a copy of its rows (rows not 64-byte
aligned, e.g. 1125-pixel BGRA) is now written back by the command buffer as it completes, before events are signalled
and handlers delivered. It used to be a completed handler, which runs asynchronously on the client's completion queue,
so `waitUntilCompleted` could return before the rows were back.

Upstream (oosexgang/hakios-xnu-kernel) checked 2026-11-07: `main` is still at our pin 63fc2b46; `mtk` drops Metal;
`legacy-hackios` holds only GL-era fixes (`eagl.c`); no open pull request touches Metal.

## How (no reflash)

- `tools/metal-ca-build.sh` (WSL) builds `userland/obj/task11`: the plug-in bundle, `metal_test` and `ca.dylib`
  (`userland/src/metal_ca_try.c`).
- `tools/metal-ca-run.sh on|off NAME [SECONDS]`:
  - `on` uploads them to `/private/var/tmp/task11`, names backboardd in `/private/var/tmp/HakiosMetal.clients` and
    inserts `ca.dylib` into backboardd through `kern.exec_inject` (`metal_test sysctl`; nfcd's group gives way for the
    512-byte limit).
  - The library answers `+[NSBundle bundleWithPath:]` for the system bundle with the uploaded one. A bindfs mount was
    tried first; the kernel allows those to pid 1 only. dyld interposing of IOKit does not reach Metal's calls inside
    the shared cache.
  - It then restarts backboardd and records the console. `off` restores the sysctl and removes the clients file.
- `tools/metal-ca-shot.sh NAME [rwtouch steps]`: does the touches, then takes a screenshot (`metal_test screen ID`).
- Plug-in switches for diagnostics, read from boot-args or, without a reflash, from `/private/var/tmp/HakiosMetal.args`:
  - `metal_trace=1`: each kind of draw once, every CoreAnimation variant chosen, the ASTC blocks a draw samples, and
    each chosen variant's AIR saved to `/private/var/tmp/hakios-air`.
  - `metal_formats=1` (or `/private/var/tmp/HakiosMetal.formats`): each kind of texture, and fills of large ones.

## Open

- **Not yet in an image.** The persistent form is the existing boot-arg `metal=3` (backboardd shown the device; `metal=1`
  is GL) with this plug-in in the ramdisk. That needs a new `boot` image, and deploying it needs the user's OK. A cold
  boot straight into Metal is therefore untested.
- Not measured: frame intervals, GPU time and touch latency on Metal against GL (task 13 instrumentation, Phase E).
- Not exercised: YCbCr video, orientation changes, CoreAnimation filters beyond blur and vibrancy.
- The jasmine CoreAnimation fixes in `NOTES.md` (format-500 YCbCr, SPI texture bind, `CA_DISABLE_DETACHED`): format 500
  is already in the plug-in. Nothing seen here needed the other two.
