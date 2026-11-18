# A640 freedreno reference captures — Metal task 4

*From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (MIT).*

**FACT (2026-11-07, run `metal-step4`):** five off-screen jobs passed on the phone's
`FD640`, `OpenGL ES 3.2 Mesa 25.0.7`. All ten captured submissions decode on the PC
with Mesa 25.0.7 `cffdump`, exit 0, chip ID `0x06040001`, empty stderr. Readback
results are recorded in `metal-step4-validation.log`. File sizes and SHA256 hashes
are in `metal-step4-manifest.json`; decoder hashes and packet counts are in
`metal-step4-decode-summary.json`.

## Where the artifacts are

Raw RD files and full text decodes are **outside git**, under this Windows checkout's
`logs/metal-step4/` (`C:\Users\teleg\Documents\Code\Poco X3 Pro\ios-vayu\logs\metal-step4`).
Each `.rd` is gzip-compressed, despite its extension. Its matching `.decode.txt`
contains the full cffdump output, including indirect buffers, draw state and shader
disassembly. `.decode.stderr` is empty for every capture. Preserve this directory:
the tracked manifest identifies files but does not replace their contents.
The ten compressed files total 145,015 bytes; full decoded text totals 500,728 bytes.

The phone also retains the dumps in `/private/var/tmp/` until reboot; this is not a
durable backup. Older exploratory files are there too. Only the directory delta
from the indexed run was retrieved.

## Test and submission index

All graphics jobs use 32×32 RGBA8 framebuffer attachments, one sample, no depth or
stencil, and compare **every pixel** with the listed result (tolerance one byte).
The triangle uses three generated vertices and covers the full target.

| Case | Primary submission (`metal4_… .rd`) | Verified result | Additional submissions |
| --- | --- | --- | --- |
| Clear | `metal4_clear_gl_reference_04155.rd` | Blue `[0,0,255,255]`; no draw packet | `04156` readback copy |
| Textured triangle | `metal4_texture_gl_reference_04157.rd` | 1×1 nearest RGBA8 texture `[51,102,153,255]`; one draw | `04158` readback copy |
| MRT | `metal4_mrt_gl_reference_04159.rd` | Attachment 0 red, attachment 1 green; one draw, `SP_FS_OUTPUT_CNTL1.MRT=2` | `04160`, `04161` readback copies |
| Blending | `metal4_blend_gl_reference_04162.rd` | Red alpha 0.5 over blue → `[128,0,128,255]`; separate RGB/alpha factors, one draw | `04163` readback copy |
| Compute | `metal4_compute_gl_reference_04164.rd` | 64 SSBO uints equal `i*3+7`; local size 8×1×1, dispatch 8×1×1 | None |

Additional filenames use the same case prefix as their primary. Decoded readback
submissions contain a blit and no draw/dispatch. The clear primary has no draw,
the three triangle primaries each have one `CP_DRAW_INDX_OFFSET`, and compute has
one `CP_EXEC_CS`. Compute decode lines 390–397 show the NDRANGE, lines 531–535 the
group counts. These provide concrete anchors for comparison with hakios-metal.

**FACT (decodes):** graphics primary submissions select `BUFFERS_IN_GMEM` with a
32×32 bin, emit GMEM render/resolve markers, and do not emit `CP_SET_BIN_DATA5`.
**INFERENCE:** these tiny jobs do not exercise hardware visibility binning. They
are references for basic correctness, not benchmarks or complete A640 coverage.
No forced sysmem path, UBWC, MSAA, LRZ, framebuffer fetch or AIR shaders were
validated here. Kernel-internal setup/submissions are not part of Mesa's RD dump.

## Reproduce

`userland/src/gl_reference.c` sets `FD_RD_DUMP=enable,full` and
`FD_RD_DUMP_TESTNAME=metal4_CASE` before creating its OSMesa ES 3.1 context.
Mesa's existing `freedreno_rd_output` writes `/tmp/metal4_CASE_gl_reference_FENCE.rd`.
On this phone `/tmp` resolves to `/private/var/tmp`. No DRM hook or driver patch
was needed. The job refuses a renderer without `640` and exits nonzero on shader,
framebuffer, readback or GL errors. It does not present a frame.

Build on WSL Debian from the Windows checkout (build outputs use WSL `~/vayu`):

```sh
# Native decoder dependencies (installed for this run):
sudo apt-get install liblua5.4-dev libarchive-dev libdrm-dev libexpat1-dev libzstd-dev
bash tools/metal-reference-build.sh
```

The script cross-builds `userland/obj/gl_reference` against the existing iOS Mesa
library and builds native `cffdump` from a separate, unpatched extraction of the
pinned Mesa 25.0.7 archive (SHA256 verified). It uses Mesa's existing Python venv.
Decoder executable:
`~/vayu/xnu-deps/build/mesa/metal4-native-build/src/freedreno/decode/cffdump`.
Native source and build remain under `xnu-deps/build/mesa/metal4-native-*` in WSL.
The normal Mesa/iOS build is not reconfigured.

On Windows PowerShell, from the repository root with iOS and sweetd up:

```powershell
python devices/poco-x3-pro/tools/sweet-sh.py --put userland/obj/gl_reference /private/var/tmp/gl_reference 755
python tools/metal-reference-capture.py --output logs/metal-step4-new
```

Then in WSL, from that same Windows checkout:

```sh
python3 tools/metal-reference-decode.py logs/metal-step4-new
# Individual full decode:
~/vayu/xnu-deps/build/mesa/metal4-native-build/src/freedreno/decode/cffdump \
    --no-color --no-pager logs/metal-step4/metal4_compute_gl_reference_04164.rd
```

The collector validates remote byte counts and gzip CRCs, and records hashes;
the decoder checks hashes and chip identity before recording packet counts.
Use a new output directory to preserve earlier evidence. Fence numbers and
addresses will differ. Mesa dumps are taken before submission: the job's checked
readback establishes that the associated work also completed correctly.

## Device and build scope

**FACT:** used the running iOS session, with `gpu=1 vayu_gpu=1 vayu_keep=1` and
SpringBoard's GL environment left running. No restart, boot-image replacement,
partition command or firmware change. The only phone writes were the temporary
test executable and ordinary filesystem capture files. After capture, sweetd and
the command port answered; battery sysctls reported 34°C and 90%.

**FACT:** WSL `git pull win vayu-dev` refused unrelated local changes; preserved
them. Built the new job from this Windows checkout and native decoder from the
verified archive. The phone used its already-installed Mesa dylib, reporting the
version above; no claim of byte identity with the local cross-build library.
`os_same_file_description` printed its existing Darwin fd-comparison fallback
warning for each job; all result checks passed nevertheless.

Task 5 (hakios-metal transport probe on the phone) is next. These captures prove
the freedreno reference path, not hakios-metal hardware execution.
