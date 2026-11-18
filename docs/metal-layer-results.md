# Task 10: IOSurface textures and CAMetalLayer on the A640

*From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (MIT).*

2026-11-07, run `metal-step10`. Boot image 4cb4ccbf (task 9's, `metal=1`), **nothing flashed**. GL window server live
throughout (backboardd pid 15). Evidence: `docs/metal-step10-phone.log` (both final runs: the program's output, the
report, the phone's framebuffer and window-server lines while the layer was up), `docs/metal-step10-summary.json`,
`docs/metal-step10-build.log`, `docs/metal-step10-composited.png`.

## What was built

| Piece | File | What it does |
| --- | --- | --- |
| `metal_test layer` | `userland/src/metal_layer.m` | Full-screen `CAMetalLayer` in a remote `CAContext`. Each frame clears to a frame-numbered colour and draws a rotating triangle (`tri_dpos_vs` + `tri_cflat_fs`, the shaders task 7 validated), then `presentDrawable:`/`commit`. Every 15th frame it reads back 5 pixels; it logs present intervals, `nextDrawable` waits and GPU time every 2 s, and reads the window server's composited surfaces. |
| Lookup fallback | `userland/src/metal_layer_lookup.c` → `metal_layer_lookup.dylib` | Interposes `bootstrap_look_up{,2,3}`. A name the system launchd domain refuses is asked of user 501's (`bootstrap_look_up_per_user`). |
| SpringBoard host | `userland/src/metal_layer_host.m` → `MetalLayerHost.dylib` + `MetalLayerHost.plist` | A tweak (`/var/jb/usr/lib/TweakInject`, loaded by `inject_tweaks.c`). On notify `com.hakios.metal_layer.host` it hosts the context ID in `/private/var/tmp/metal_layer.host` in a `CALayerHost` above every window. No hooks. |
| Signer | `tools/macho-sign.py` | Ad hoc code signature with XML entitlements, replacing the linker's (`-Wl,-adhoc_codesign`). There was no ldid; `xnu-deps`' `codesign` is a stub. |
| Build / run / report | `tools/metal-layer-build.sh`, `tools/metal-layer-run.sh`, `tools/metal-layer-report.py`, `tools/metal-layer-png.py` | Build in WSL; one command uploads, logs, runs and judges a run on the phone. |

No plug-in or driver change was needed: `HakiosMetalCA.m` (task 9) already wraps QuartzCore's drawable IOSurfaces
(`-newTextureWithDescriptor:iosurface:plane:` → `hakios_metal_texture_new_iosurface`). It presents a drawable when its
command buffer *completes*, not when it is scheduled, and that ordering is what keeps the display from reading a
half-drawn surface.

## Results

**FACT (`final-full`, 60 s, opaque full-screen layer):**
- 3590 frames in 60.02 s (59.8/s): all completed, 0 command buffers in error, 0 failed checks, exit status 0.
- 239 frames read back from their drawables, 0 wrong (the centre is the frame's triangle colour, the four corners its clear colour).
- Present interval: median 16.65 ms, p95 18.01, p99 19.15, max 130.66 (one hitch).
- `-nextDrawable` blocks about 14–15 ms a frame: QuartzCore paces the program at the panel's rate.
- GPU time per frame: median 2.0–3.0 ms, p95 ≤ 3.25 ms in every 2 s period.
- The drawables cycle through 3 IOSurfaces. QuartzCore gives the drawable itself to the framebuffer (direct scanout): the kernel's swap lines name the program's surfaces in framebuffer layer 0, and the window server composites nothing.
- Panel: median 60.9 frames/s (min 54.9). Frame-to-frame: 95% under 21 ms in every second; 2 seconds of 58 had a gap over 26 ms (longest 69 ms).
- Framebuffer counters while hosted: **0** surfaces given to the GPU again while being copied to the panel, **0** locked for drawing while being copied, **0** swaps copied before the GPU finished. Any of these would be the mechanism for tearing.
- 35 °C, 80% afterwards.

**FACT (`final-plain`, 20 s, `LAYER_PLAIN=1`):** a magenta `CALayer` with the Metal layer as its lower half, so the
GL window server (`eagl.c`, Mesa FD640) has to composite the drawables.
- 1187 frames (59.3/s), 79 read back, 0 wrong.
- Present interval: median 16.66 ms, p95 20.53, p99 22.10.
- Three reads of the window server's own output surfaces (IOSurfaces 3 and 4): the program's background on 146–151 of 400 grid points (the lower half minus the triangle), **every point from one frame number**, 0–3 frames behind the newest. `docs/metal-step10-composited.png` is the last one.
- Same zero counters; panel at 60.9/s.

**FACT (kernel surface ABI, `HakiosGPU` selector `0x7e`, 40-byte `HakiosSurface`):** each drawable is imported once
("IOSurfaces taken as buffers", about 0.5 ms each) and then reused across frames. Layout: linear BGRA8, 1125x2436,
rows of 4544 bytes (4500 rounded up to 64), which the driver's 64-byte row rule accepts. Task 5 listed this selector as
not validated; it is now exercised on hardware.

**FACT (the user, watching the panel through all the runs):** the animation was on the panel with nothing wrong
(no tearing or corruption reported). The kernel's scanout lines and counters and the composited read-back agree.

## What it took to get a context on screen (all FACT, phone logs)

1. **launchd domain.** backboardd's services (`com.apple.CARenderServer` and the rest) live in the `user/501` domain.
   A child of sweetd (a system job) is answered from `system`: "failed lookup ... No such process", 1102. launchd
   picks the domain by the job a process descends from: `setuid(501)` changed nothing. libxpc reads the bootstrap
   port once, at process start. So `metal_test layer` takes SpringBoard's bootstrap port (`task_for_pid`, which works
   for an ad hoc signed, i.e. platform, binary here) and runs itself again: special ports survive `exec`. The lookup
   dylib covers the program's own lookups; interposing doesn't reach calls inside the shared cache.
2. **Visibility.** A displayable remote context (display 1, level 1e6, `orderAbove:`) is created and committed to,
   but the window server draws none of it, not even a plain `CALayer`. Hosted by SpringBoard in a `CALayerHost`, it
   shows at once. INFERENCE: on iOS 16 the window server shows the system app's contexts and what they host, which
   is how SpringBoard shows other processes.
3. **Entitlements are not the gate here.** This kernel's AMFI stand-in never gives user space anyone's entitlements:
   `OSEntitlements_get_xml` is a stub, so `csops` returns none for every process, SpringBoard included. The ones
   signed into `metal_test` are inert. `CARenderServerRenderDisplay` returns an all-zero surface.
4. **Signatures.**
   - A DER entitlements blob makes the kernel reject the whole signature (process unsigned, flags 0); XML only is accepted (flags `0x26000001`: valid, platform).
   - A signed file overwritten in place keeps its old cached signature and then counts as unsigned. Remove first, then upload; `metal-layer-run.sh` does.
5. **Test cost.** Reading back a whole 11 MB drawable cost about 22 ms of GPU time. That delayed the window server's frame past the framebuffer's 20 ms fence wait ("the frame is copied as it is"), so the read-back is now 5 pixels.

## Running it again

```bash
wsl -d Debian -- bash -lc 'bash "/mnt/c/Users/teleg/Documents/Code/Poco X3 Pro/ios-vayu/tools/metal-layer-build.sh"'
```

Install the host once: `MetalLayerHost.dylib` and `.plist` from `userland/obj/task10` go into
`/private/var/jb/usr/lib/TweakInject` (make the directories with `sweet-sh.py mkdir`), then restart SpringBoard
(`sweet-sh.py kill -15 <SpringBoard pid>`; launchd starts it again). These files are on `/var` and survive a reboot.
To remove them, delete them; the directories are inert (`init.c` only acts on `/var/jb` when the image has a package
prefix, and this one has none).

```bash
tools/metal-layer-run.sh final-full 60
```

```bash
tools/metal-layer-run.sh final-plain 20 LAYER_PLAIN=1
```

## Open

- `presentedTime`/`addPresentedHandler:` never fire: 0 frames reported. Frame intervals come from present calls and the kernel's panel statistics instead.
- `GPUStartTime`/`GPUEndTime` are non-zero and plausible (2–3 ms). Validating the timer belongs to task 13.
- Only BGRA8, single plane, linear. YCbCr and other formats/planes are task 11's (CoreAnimation's surfaces).
- The host is a test fixture. A real app gets its context hosted by FrontBoard/UIKit scenes; that path belongs to tasks 11 and 12.
