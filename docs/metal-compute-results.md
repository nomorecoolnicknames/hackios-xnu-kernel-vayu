# A640 compute — task 6

*From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (MIT).*

**FACT (2026-11-07, run `metal-step6`):** the complete no-Mesa compute
probe passes seven times: once initially, three times with SpringBoard and
backboardd stopped, three times with the original UI processes running. Each
run passes 70 compute checks and eight CP transport checks. A separate fixture
checks `tex_size_h` at two sizes, comparing all 4,096 output pixels each time.
All eight final process-lifetime audits pass: no outstanding GEM handles,
mappings or queues, no transport errors, successful service close. Phone UI
PIDs 9 and 15 resumed; final battery 27 C / 89%. No reboot or partition write.

Evidence: `metal-step6-phone.log`, `metal-step6-summary.json`, and
`metal-step6-build.log`. Full intermediate logs, raw captures and full decoder
output are preserved under ignored `logs/metal-step6/`. This is a core-driver
hardware result; the system UI continues to use its existing GL fallback.

## Coverage

The committed fixtures executed on A640 are:

| Fixture | CPU comparison / controls |
| --- | --- |
| `end_only` | Fence completes, all 1,024 canary words unchanged; one and 16 groups |
| `fill_id`, `fill_id_air` | Every uint `gid*K+c`; changed constants, half grids and untouched tails; indirect counts changed after encoding; grids through 262,144 words |
| `chain160_air`, `chain220_air`, `long_chain_air` | Every uint against the CPU rotate/xor/multiply chain, and a one-step-short negative reference; partial preload and fetch beyond instruction cache |
| `pvt_fill_air` | All 4,096 private-memory outputs and 64 tail words |
| `tg_arg_air` | All 1,024 neighboring static/dynamic shared-memory pairs and 64 tail words |
| `ptr_phi_air` | All 256 branch-selected loads, 256 loop sums and 64 tail words |
| `tex_fill_air` | R32/RGBA32Uint, nontrivial row pitches, partial grids, changed constants, mip/slice views, untouched padding |
| `tex_fill_h_air` | All 2,048 RGBA16Float pixels, exact half bits; excludes interpreting those bits as integers |
| `tex_copy_air` | Every lane of all covered pixels, partial grids/padded rows; six additional integer formats, absent channels and alpha defaults |
| `tex_sample_gid_air` | Nearest/linear pixel centers and quarter positions; CPU bilinear reference and distinct nearest/linear controls |
| `tex_fill_arr_air`, `tex_fill_3d_air` | Every covered texel, untouched final layer/depth slice and inter-layer padding |
| `tex_sample_wide_air` | Seven address/border modes; 540 CPU-reference samples each; checks cases where other modes differ |
| `tex_size_h_air` | Half sample multiplied by queried texture width; 4x4 and 8x8 patterned inputs to 64x64 output; all four lanes of every pixel |

The half-query fixture was present upstream but unused by its main probe;
`tools/texture-query-probe.cpp` now exercises it explicitly.

**FACT / remaining compiler limitation:** `argbuf_res_air.hpp` remains an A635
fixture and is deliberately not submitted. Regeneration for A640 fails with
`interface: input air.indirect_buffer` in the existing frontend. The main
probe's argument-buffer case requires live `compileAir` reflection, unavailable
in this no-compiler build. Carry this diagnosed case into task 9's AIR API and
argument-encoder work. The bitcode-only clz/bytes/uniform/pack/undef-phi/width/
ballot cases likewise require that compiler-enabled configuration. These are
not hardware passes. No `MESA=1` or device-side AIR compilation is claimed.

## Fixes found by these fixtures

The eleven additional fixtures were regenerated from their committed LLVM IR
with Mesa 25.0.7 ir3, explicitly targeting `0x06040001`. Three initial generation
failures and one hardware mismatch exposed older frontend behavior:

- **FACT:** a pointer phi became scalar SSA, so dereference failed. Pointer
  phis now keep a static memory class/buffer binding and carry the byte offset
  through the phi register. Branches and loop back edges pass on hardware;
  a regression also verifies that merging different buffer bindings is refused.
- **FACT:** half image stores were typed from their void return type as uint.
  Stores now take their scalar class and width from the stored value.
- **FACT:** the 2D array-store layer was consumed as the value, then the value
  was required to be a constant mip level. The separate layer is now appended
  to the spatial coordinate before reading the store value.
- **FACT:** the shared-memory fixture completed but only 992/1,024 pairs
  matched. The two wrong lanes per group correspond to aliases of `s[0]` and
  `s[32]`: both static array and dynamic argument began at offset zero.
  Dynamic shared arguments now begin after the aligned static allocation.
  The fixed shader passes 1,024/1,024 and preserves its tail.

These changes are in the existing `tools/mesa/air` frontend. The installed iOS
Mesa library was not rebuilt or replaced; GL stays on its working compiler.
Only the precompiled A640 fixtures carry these fixes onto the phone here.

## Host and compiler checks

**FACT:** driver suite 261 tests / zero failed checks; fake CP stages pass;
iOS library, main probe and both focused probes cross-build. Native fixture
writer generates all eleven new A640 fixtures. Pointer-phi branch/loop and
cross-binding rejection regression passes.

**FACT / existing software failure:** the full `tools/air/check.sh -q` returns
1. Reader: all 456 modules match LLVM. Translation/ir3: 321/456, unchanged
from the recorded baseline. All 255 comparable LLVM/softpipe shader results
match; 201 cannot be compared by that harness. Arithmetic constant folding:
71 exact operations agree over 45,697 cases. Atomics/barrier check and all 35
software texture calls pass. The software execution subset has ten operation
mismatches among its 612 cases: clamp/fmin/fmax variants. Running the same
subset with the pre-task frontend reproduces the same ten failing operations
and counts. These are baseline failures, not a clean full-CI pass or Adreno
failures; their underlying softpipe/reference semantics remain open. The
current frontend was restored and its pointer regression rerun afterward.
Full output and comparison: `metal-step6-build.log`.

## Submission comparison

Both the failed and fixed actual shared-memory submissions were captured before
submission with their referenced buffers. Opt-in `HAKIOS_METAL_RD` in
`Gpu::submit` writes Mesa-compatible RD sections; it is disabled normally.
`--dump-threadgroup FILE` selects just that dispatch. Capture errors fail that
submission. Raw captures are 360,596 bytes each; SHA256 and decoder hashes are
in `metal-step6-summary.json`. The task-4 native `cffdump` decodes both as
`0x06040001`, exit zero, empty stderr.

Compared with task 4's `metal4_compute_gl_reference_04164`:

| Decoded state | Freedreno reference | Failing/fixed hakios-metal shared test |
| --- | --- | --- |
| Dispatch | `CP_EXEC_CS`, groups 8x1x1 | `CP_EXEC_CS`, groups 16x1x1 |
| Local/global X | 8 / 64 | 64 / 1,024 |
| IBO count | 1 output SSBO, bindless | 1 output SSBO, direct |
| Shared-size encoding | 1 (no shader shared data) | 8 (8,448 requested bytes rounded to KiB, encoded minus one) |
| Completion | GPU fence + correct 64 words | Completed fence, clean transport; only shared aliases wrong before fix |

The task-4 shader does not use shared memory, so it cannot prove shared-address
correctness. It does provide the known-good compute dispatch/descriptor state.
The before/after shared captures have the same dispatch, descriptor and shared
allocation state. The decisive disassembly difference is:

```text
before: stl.u32 l[r2.x],     r1.w, 1
        ldl.u32 r3.x, l[r0.x],     1
after:  stl.u32 l[r2.x+256], r1.w, 1
        ldl.u32 r3.x, l[r0.x+256], 1
```

**INFERENCE, confirmed by fixed CPU readback:** the mismatch came from shader
shared-address lowering, rather than transport, launch size or shared capacity.
No PM4 or kernel change was needed. As in task 5, `FAULTS 0` is a stub on vayu
and is not evidence of recovery/reset counts. Recovery of a hung core is not
ported; these tests completed without requiring it.

## Reproduction

From the Windows checkout, using WSL Debian:

```sh
# Attempt git -C ~/vayu pull win vayu-dev first; preserve unrelated dirty work.
bash tools/metal-compute-host.sh
bash tools/metal-compute-fixtures.sh
python3 tools/metal-compute-frontend-check.py
bash tools/metal-compute-build.sh
bash tools/metal-compute-query-build.sh
```

The host wrapper uses the existing `tools/air/mk-host.sh` setup in
`~/vayu/build/mesa-host`, with a local libc compatibility correction to Mesa's
`once_flag` guard. It builds native fixture tools separately from iOS Mesa.
The fixture writer omits the newer frontend's null-texture metadata loop;
none of these compute shaders declares a null texture. The runtime AIR adapter
still needs the broader API reconciliation in task 9.

Upload `userland/obj/metal-probe`, `metal_transport_run` and `metal-query-probe`
with `devices/poco-x3-pro/tools/sweet-sh.py --put` to `/private/var/tmp/`.
Run the probe as `metal-probe-compute --compute-only --transport-audit`.
The optional `--compute-core-only` restricts it to the original task-1 A640
fixtures; the default compute-only mode includes every regenerated fixture in
the main probe. Rendering, deliberate recovery tests and metallib sweeps are
beyond its stop point.

`metal_transport_run paused|live PROBE 3 compute` repeats that suite with UI
state verification and a 20-second independent resume watchdog. The equivalent
job list is `userland/rc.jobs.metal-compute`; this run used sweetd directly so
stdout, exit status and audits could be collected without the full console.
Run `metal-query-probe` separately for the two additional query checks.
`python tools/metal-compute-report.py` verifies and extracts the evidence.

Next: task 7, rendering fixtures. Do not run the unrestricted probe blindly:
newer graphics fixtures still target A635 and require their own A640 audit.
