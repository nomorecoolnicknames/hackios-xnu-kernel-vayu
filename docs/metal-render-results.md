# A640 rendering — task 7

*From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (MIT).*

**FACT (2026-11-07, run `metal-step7`):** the whole hardware probe, stages 1–9,
passes four times in a row on the phone with the GL UI live: twice in the default
mode, where every pass that qualifies is drawn in GMEM tiles, and twice with
`--sysmem`, where only the probe's own GMEM-versus-sysmem comparisons tile. Each
run: exit 0, **253 checks passed, 0 failures**. 175 of the checks are in stage 9
(rendering), and the other 78 are the transport and compute stages of tasks 5–6,
rerun as regression checks. Probe `userland/obj/metal-probe` sha256 `5819ce1b…5d4b`.

| Run | Mode | Draw passes | In GMEM (binned, tiles) | In sysmem | Checks | Failures |
| --- | --- | --- | --- | --- | --- | --- |
| f1 | default | 683 | 488 (7, 1048) | 195 | 253 | 0 |
| f2 | `--sysmem` | 683 | 14 (1, 130) | 669 | 253 | 0 |
| f3 | default | 683 | 488 (7, 1048) | 195 | 253 | 0 |
| f4 | `--sysmem` | 683 | 14 (1, 130) | 669 | 253 | 0 |

In `--sysmem` mode, the 14 GMEM passes are the tests that call for GMEM by name
(GMEM-versus-sysmem comparisons, framebuffer-fetch GMEM kinds, Clear in tiles).
**INFERENCE (from `gmemPlan`/`gmemDrawOk`'s rules, not counted per pass):**
the 195 sysmem passes of the default mode are passes the planner refuses for
GMEM: depth/stencil, MSAA, non-window-sized targets and the other exclusions
that task 14 keeps. So each fixture's pixels are judged in both modes wherever
the driver can tile it.

Evidence: `metal-step7-phone.log` (the four runs, verbatim),
`metal-step7-fixtures.log`, `metal-step7-build.log`, `metal-step7-summary.json`.
Stepwise and earlier runs are in ignored `logs/metal-step7/`. The phone stayed
at 26 °C. SpringBoard (pid 9) and backboardd (pid 15) ran throughout and were
still running afterward. Only temporary uploads to `/private/var/tmp`: no
reboot, no partition command. The system UI stays on its GL fallback.

**Transport audit.** Each run's audit shows `handles=0 mappings=0 queues=0` and
exactly one failed call. That call is request 126 (`kHakiosSurface`, 0x7e),
which answers `kIOReturnNotFound` for the deliberate control "a texture over an
IOSurface ID nobody made". The audit now names every failed request, and that
line prints directly before the control's `ok` line. No other transport error
occurred. The audit's summary word is therefore `FAIL` by its own strict rule,
for an expected refusal; there was no leak and no unexpected error.

## Coverage, in the order task 7 lists it

| Task 7 item | Probe section (checks a run) | What is judged |
| --- | --- | --- |
| clear and read back | 9i (9) | 2D-engine Clear over six formats and a sub-rectangle, then before a draw; refusals by name |
| flat triangle | 9a–9c (7) | the lower-left half to the byte, Metal's orientation, one fill rule on the diagonal, half viewport, `vertexStart` |
| attributes | 9d, 9h, 9h2, 9d2, 9n, 9o, 9o2, 9y (28) | barycentric colours, interleaved `[[stage_in]]`, other vertex formats, half varyings, indexed/restart/instanced, vertex pulling via UBO, base vertex/instance |
| textured triangle | 9k, 9j2, 9k2, 9w, 9s (37) | FS prefetch, ASTC LDR/HDR, 1D, mip levels and `generateMipmaps`, null texture 2D/3D; cube, cube array, 2D/1D array, 3D, shadow compare |
| blending | 9j (6) | six equations, every pixel against the formula |
| depth | 9l (14) | six test setups, Depth32Float; MSAA depth resolves |
| stencil | 9x (3) | Depth32Float_Stencil8: write 1, then draw where it equals 1 |
| MRT | 9t (4) | `[[color(0)]]`/`[[color(1)]]` into their own attachments, one also cleared |
| framebuffer fetch | 9u (8) | once and twice in a pass, raster order and barrier, GMEM and sysmem |
| a direct-to-memory and a GMEM pass of the same scene | inside 9w (3g): 10 | 1125x2436 BGRA8: five blended triangles across tile edges, **0 pixels differ** GMEM against sysmem, and the no-load twin differs (2,740,432); hardware binning (VSC) 0 differ; framebuffer fetch across 12 tiles and in a 6-tile corner, 0 differ; Clear in tiles (BGRA8, RGBA16Float) equal to the 2D fill |
| (beyond the list) | 9g, 9r, 9u2, 9v, 9z, 9z2, 9q2, 9l2, 9m (61) | constant buffers, 200 draws in one pass, derivatives, A8/R16F/RG16F/RG32F targets, private memory, a FS beyond the instruction cache, occlusion queries, lines, culling, an IOSurface target and a '420f' YCbCr sample |

Performance lines are measurements, not judgments; they go to task 13/14. With
GMEM, eight full-screen blended 1125x2436 draws took 2,640 µs, against 5,137 µs
in sysmem (1.95x). A two-screen pass with a Clear and 1 or 140 tiny draws took
2,140 / 3,631 µs in GMEM, against 663 / 722 µs in sysmem.

**Submissions against task 4.** No fixture disagreed with its CPU rule on the
phone, so no stream needed diffing against the freedreno captures. Task 7 asks
for that comparison only where results disagree.

## Not covered here (FACT, why)

- **Compiled on the phone:** 9q (constexpr sampler compiled on the device), 9f
  (the four stages compiled on the phone) and the compiler-dependent
  measurements print "no compiler in this build". This is the no-`MESA=1`
  baseline; task 9.
- **Fixtures no probe section draws:** `tri_dbuf_fs`, `tri_fc_fs`,
  `tri_nullfb_fs`, `tri_oldcb_vs`, `tri_olddb_vs`, `null_fs`. They are used by
  the host suite only. They were still regenerated for A640 where they were A635.
- **Bicubic filtering** is not a render feature of this chip. A640 (a6xx_gen2)
  has no cubic filter in its TP. On this chip the probe now checks that the
  driver refuses a bicubic sampler by name and still makes the linear one.
- **9e** (`--bisect`) and the deliberate hang/recovery stage did not run.
  Recovery is not ported (task 5/6).
- UI paused/stopped runs were not repeated for rendering: all four runs had
  the compositor drawing alongside the probe, the harder case of the two.

## What changed to get here

1. **Fixtures.** 28 render fixtures in the pinned tree were still A635
   (`0x6030500`): `chain_fs` and the `tri_*` stages listed in
   `metal-step7-fixtures.log`. Our older `tools/mesa/air` frontend could not
   build 10 of them: `air.render_target` (framebuffer fetch), array-texture and
   cube coordinates, compare sampling, address-space-0 buffers, a pointer
   argument, and the null-texture metadata of two more. Some of the others it
   builds differently: `tri_arr_vs`'s constant table goes to private memory, and
   a host test then fails.
   So the fixtures were regenerated with the AIR frontend of the pinned upstream
   revision itself: a detached worktree `~/hakios-pin` at
   `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`, native Mesa 25.0.7 built by its
   own `tools/air/mk-host.sh` (plus task 6's glibc `once_flag` correction),
   and only the chip id changed to `0x06040001`.
   **Control (FACT):** the same generator at `0x6030500` reproduces all 28
   committed A635 fixtures byte for byte. So every difference in the A640
   fixtures comes from the chip, not the compiler. Scripts:
   `tools/metal-render-fixtures.sh`, `tools/metal-render-fixtures-control.sh`.
   **Not a frontend decision:** task 1's choice of `tools/mesa/air` as the only
   frontend stands. The pinned frontend serves here as the fixture generator
   only, and reconciling ours with it remains task 9 (see `hakios-metal/VAYU.md`).
2. **Probe** (`hakios-metal/tools/probe.cpp`): `--sysmem` (the queue's new
   GMEM default off), `--render-stop SECTION` (end as a section of stage 9
   begins, used for the stepwise first runs), a `probe: section` marker
   before each, and a closing line of draw passes by mode. Three expectations
   were written for upstream's A635 and are corrected here:
   - bicubic: a refusal is the right answer on A640;
   - full-screen GMEM tile count: hard-coded 24 (A635), now the driver's own
     planner (12 on A640);
   - five GMEM-labelled tests relied on the queue default instead of asking:
     they now `setGmem` explicitly.
   No pixel rule was loosened.
3. **Driver** (`command.hpp`/`command.cpp`): `CommandQueue::setGmemDefault`
   (what a new command buffer's `setGmem` starts as; on, as before) and
   `passTotals()` (draw passes, GMEM, binned, tiles, summed at submission).
   `iokit_transport.cpp`: the audit names each failed request.
   Host suite: 261 tests, 0 failed checks. Fake probe stages 1–4 pass, and
   the fake runs stage 9 to its end without a crash.

**No driver rendering fix was needed on A640:** every stream that ported
fixtures produce drew correctly the first time. The only failures were the
three A635 expectations above and the five tests that relied on the GMEM
default.

## Reproduction

From the Windows checkout, using WSL Debian:

```sh
# once: git -C ~/hakios worktree add --detach ~/hakios-pin 63fc2b4633bac3af8e57dadab9f5a8d4cb940438
#       then tools/air/mk-host.sh there (Mesa 25.0.7), with the once_flag correction of metal-compute-host.sh
bash tools/metal-render-fixtures.sh [NAME...]
bash tools/metal-render-fixtures-control.sh
bash tools/metal-render-build.sh
```

Upload with `devices/poco-x3-pro/tools/sweet-sh.py --put
userland/obj/metal-probe /private/var/tmp/metal-probe-render`, then run
`metal-probe-render --transport-audit` and `metal-probe-render --sysmem
--transport-audit` (about two minutes each, `--timeout 580`). Read one
section at a time with `--render-stop 9x`.

Next: task 8 (register the device with iOS). Task 9 then reconciles the
frontend, because a pipeline compiled on the device must produce what these
fixtures hold.
