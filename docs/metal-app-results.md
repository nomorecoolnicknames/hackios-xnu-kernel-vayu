# Task 9: a real Metal program on the A640

*From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (MIT).*

Date: 2026-11-07. Roadmap: `docs/metal-roadmap.md`, task 9. Evidence: `docs/metal-step9-phone.log` (phone),
`docs/metal-step9-build.log` (builds, host suites, fixture reconciliation). Raw per-run logs: `logs/metal-step9-*.txt`
(not in git).

Labels as in the roadmap: **FACT** (seen, with the log), **INFERENCE**, **HYPOTHESIS**.

## What was done

1. **One AIR frontend, reconciled.** `tools/mesa/air` is now the pinned upstream revision's frontend
   (`63fc2b46`, vcvkk's lineage, which `hakios-metal/src/compiler.cpp` is written against), with upstream's
   `air-ir3.patch` and its two ir3 fixes `ir3-shift-split.patch` and `ir3-ssbo-subdword.patch`. Our older copy's
   task-6 fixes (pointer phis, typed half stores, array-store layers, static/dynamic shared memory) are not
   re-applied on top: **FACT** (`docs/metal-step9-build.log`, `tools/metal-fixtures-reconcile.sh`) the reconciled
   frontend reproduces 58 of the 59 committed A640 fixtures byte for byte, task 6's compute ones included, so upstream
   already lowers those cases to the same words. The 59th, `argbuf_res`, was still the A635 fixture our old frontend
   refused (`air.indirect_buffer`); it now compiles for A640 and is committed.
2. **Compiler-enabled builds.** Host Mesa (`tools/air/mk-host.sh`, two build fixes: meson in its venv, and a glibc
   2.41 guard) and the iOS cross Mesa (`tools/mk-mesa.sh`, then `ninja ... src/compiler/air/libair.a`) both from this
   tree's `tools/mesa`. **FACT:** host suite with the compiler: 263 tests, 0 failed checks; without it: 262 tests,
   0 failed checks; fake probe stages 1–4: 0 failures; `make ios MESA=1` cross-builds the archive and probe. One
   test expectation was A635's register allocation (`l[r1.z+256]`; A640 has `r2.x`), now chip-independent.
3. **The plug-in.** `userland/src/HakiosMetalCA.m` is upstream's full Objective-C bridge over the same core
   (textures, samplers, depth/stencil, render and compute pipelines and encoders, blits, argument encoders, heaps,
   events, IOSurface textures; measured by upstream on iOS 16.5's Metal 306.6.4), vendored in place of the bundled
   `hakios-metal/plugin/HakiosMetal.m`, whose objects were placeholders. vayu changes: task 8's process gate
   (`metal=1` metal_test only, `2` all but backboardd, `3` all, `HakiosMetal.clients`), the name "HakiOS Adreno 640"
   and task 8's limits/families, a `pipeline cache:` line per pipeline (time, stages from the cache, stages
   compiled), and function-constant specialisation done in the plug-in (below). `userland/build.sh` builds it by
   default; `METAL_PLUGIN=basic` builds the old one.
4. **Persistent compute cache.** `compileAir` now reads and writes the same disk cache as `compileStage`
   (`HAKIOS_METAL_CACHE`, read-only `HAKIOS_METAL_CACHE_SEED`) under its own `.kernel` names; a damaged file is a
   miss. `hakios_metal_cache_counts()` reports hits and misses (host test
   `a_compiled_kernel_comes_back_from_the_disk_cache`).
5. **The program.** `metal_test app LIBRARY [BUNDLE [CACHE_DIR]]` (`userland/src/metal_app.m`, linked into
   metal_test because `metal=1` shows the device to metal_test alone). After the device, everything is public Metal
   API. The library is built on the PC: `tools/metal-mklib.py` packs the hand-written AIR of
   `hakios-metal/tools/kernels` (llvm-as) into the MTLB container that iOS's Metal parses itself, with `CNST` public
   metadata for function constants (format read off the image's `AV1SW.metallib`). `tools/metal-app-build.sh`
   builds everything.

## Results on the phone

Boot image 1252556b (task 8, `metal=1`), nothing flashed: the bundle, metal_test and the library were uploaded to
`/private/var/tmp/task9` and the device was made from that bundle by hand (`metal_test`'s `bundle` mechanism, as
Metal makes it: `-initWithAcceleratorPort:` on the IOAcceleratorES service). GL window server live throughout.

**FACT (final pair, `docs/metal-step9-phone.log`):** 24 checks, 0 failed, cold and warm:

| Check | Result |
| --- | --- |
| `-newLibraryWithURL:` of the PC-built `.metallib`, `-newFunctionWithName:` with types | 7 functions |
| compute `fill_id`, 64 threadgroups of 64, `setBytes` constant | 4096 of 4096 words right |
| compute into a texture, `tex_fill`, 64x32 RGBA32Uint, `setTexture` | 2048 of 2048 texels right |
| blit `fillBuffer` 64 KiB + `copyFromBuffer` 16 KiB | 0 wrong bytes |
| render `tri_color_vs+tri_color_fs`, 64x64 RGBA8, clear + store | 4032 of 4032 pixels within one unit of the barycentric rule (max deviation 0) |
| render `tri_color_vs+tri_tex_fs`, 4x4 texture by `replaceRegion:`, nearest clamp sampler | 3840 of 3840 judged pixels their texel exactly |
| function constants 0=7, 1=true | 4096 of 4096 pixels `07 ff 40 ff` |
| function constant 1=true alone (guarded 0 absent) | 4096 of 4096 pixels `00 00 40 ff` |
| required constant 1 left out | refused with an error naming `fc_opaque` |
| each render target read by `-getBytes:` and by a blit `copyFromTexture:toBuffer:` | identical |
| completion handlers, `waitUntilCompleted`, status Completed | 7 of 7 handlers |

**FACT (cache):** cold run on an empty cache: 0 hits, every stage compiled (fill_id 91 ms including the
compiler's first use in the process, render pipelines 4–10 ms), 8 cache files written. Warm run, next process:
every pipeline from the cache, 10 hits / 0 compiled (fill_id 0.44 ms, render pipelines 0.5–0.9 ms), no new files.
The plug-in's own persistent cache `/private/var/db/hakios-metal-cache` (kept across boots, `var_hfs=1`) behaved the
same in runs 6–7 (written in run 1, warm after).

**FACT (device report, new bundle):** name "HakiOS Adreno 640", registry ID 4294967593 (the IORegistry entry, task
8's open item), 256 MiB buffers, 1024 threads, 32 KiB threadgroup memory, samples 1/2/4, families Common1/Apple1/
Apple2 only, iOS feature sets 1 and 2; a 4 MB public-API blit fill: 0 wrong bytes.

**FACT (compiler-enabled probe, full stages, `--transport-audit`):** 279 checks pass, 1 fails; 699 draw passes (491
GMEM, 7 binned). Newly covered by a compiler on the device: 9q/9q2/9f (stages compiled and linked on the phone, the
same words as the host's, and drawn), and 6w: `argbuf_res` compiled on the phone, both argument-buffer dispatches
right (task 6's carried item). The transport audit's one error is the deliberate missing-IOSurface control
(request 126, as in task 7).

**The one probe failure, diagnosed:** "compile tex_fill.ll on the phone: 96 words, DIFFERENT from the host's,
layout the same". **FACT:** the host compiler compiles the probe's embedded bitcode to exactly the fixture, and the
other seven on-phone compiles match the host's word for word; the Metal program's phone-compiled `tex_fill` writes
2048 of 2048 texels right. So the phone's compile of this one kernel is different but correct code.
**HYPOTHESIS:** the iOS and Linux builds of ir3 order something differently (the libc's `qsort_r` is BSD's on iOS,
GNU's on Linux, and differs on equal keys). Not chased further; the check stays strict.

## Fixes found on the way

- Function constants: Metal's own `-newFunctionWithName:constantValues:error:` asks a compiler service this device
  has not got: **FACT** run 2 waited ~30 s, then "Compiler encountered an internal error". The plug-in now
  specialises functions of its own libraries itself: the plain function with the values attached
  (`_baseFunctionConstantValues`), after Metal's rule that a constant read without `is_function_constant_defined`
  is required. The core compiles them in at pipeline creation, keyed in the cache by the values.
- A library without `CNST` public metadata has its constants silently dropped (run 1: "nothing supplied").
- Build: ld64 needs `-Wl,-U,_native_bridge_metal_*` for the plug-in's weak metal2vulkan symbols; `llvm-nm` (not
  binutils `nm`) reads the Mach-O archive.

## Deployed: the system path (image 4cb4ccbf)

With the user's OK the plug-in went into the image (`docs/metal-step9-deploy.log`): 4cb4ccbf = task 8's 1252556b
with only the ramdisk rebuilt (d2186bb7: `bin/metal_test`, the plug-in, `libOSMesa` and `motd` differ from step 8's
tree; same 21 firmware files, same kernel, loader, resident image and command line). Deployed from iOS; `boot`
holds it. Backups: `../backup-vayu/xnu-standalone-metal9-4cb4ccbf.img`, `root-flash-fcb7ecfa.apfs`; rollback 1252556b.

- **FACT:** `/redwood/bin/metal_test app task9.metallib` on `MTLCreateSystemDefaultDevice()`: 24 of 24, twice.
  The first run after the reboot took every stage of the earlier runs from `/private/var/db/hakios-metal-cache`
  (the cache survives a reboot); only `tex_fill`, never cached there before, compiled. Second run: all from the cache.
- **FACT:** `metal_test device`: the system device "HakiOS Adreno 640", registry ID 4294967593, task 8's limits and
  families, blit PASS.
- **FACT (boot console):** the window server is refused the device (metal=1) and makes its OpenGL ES 3.2 context
  on FD640 with the rebuilt Mesa (the two new ir3 patches in); SpringBoard 9 / backboardd 15 stay up. Not looked
  at beyond that: GL rendering correctness under the new Mesa was not checked frame by frame.

## Not covered (still open)

- Vertex descriptors/attributes through Metal, depth/stencil states, MSAA, MRT and blending through the public API
  were not in this program (the core passed them in task 7 through its own interface). Argument encoders and heaps
  were not exercised through Metal.
- Plug-in GPU times: `GPUStartTime/GPUEndTime` now return values (0.15–0.5 ms here); not validated against the
  timer (task 13).
- `THIRD_PARTY_LICENSES.md`: the plug-in's origin licence is "to verify", as upstream records it; the user
  judged that no issue for this project's purpose (2026-11-07).
