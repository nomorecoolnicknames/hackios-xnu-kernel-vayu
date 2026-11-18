# Vendored hakios-metal for vayu

## Provenance

Upstream: https://github.com/oosexgang/hakios-xnu-kernel, directory `hakios-metal/`.
Pinned revision: `63fc2b4633bac3af8e57dadab9f5a8d4cb940438` (main observed 2026-11-07;
commit dated 2026-11-06, merge PR #53). This is the current integrated driver, rather
than the retired standalone repository. Pinning the whole upstream revision makes
the source and its build fragments an identifiable snapshot. No upstream Git
metadata or generated build output is vendored. MIT license and Mesa notices are
preserved in `LICENSE` and `NOTICE`.

## Local changes and patch ledger

Apply the patches from the repository root with `git apply --directory=hakios-metal`.
The original patches remain in `docs/vayu-patches/` as historical inputs.

- `0040-hakios-metal-a640-gen2-port.patch`: all hunks applied except the fourth
  `src/compiler.cpp` hunk (old line 1259). Upstream moved the surrounding
  `compileStage` cache/locking code. Manually replaced its remaining hard-coded
  `0x06030500` diagnostic with `chipName()`, exactly as intended by the rejected
  hunk. No hunk was discarded. Includes the fake transport for the staged probe.
- `0043-hakios-metal-a640-fixtures.patch`: all hunks applied without conflicts.
- `tools/kernels.mk`: default fixture chip is now `0x06040001`. Regeneration of
  assembly/AIR fixtures requires the explicit `fixtures`/`air-fixtures` goals;
  `Makefile` similarly gates bitcode headers behind `bc-fixtures`. A normal build
  consumes committed fixtures and cannot silently replace them according to file
  timestamps or require a host Mesa build to recreate them.
- `Makefile`: `vayu-check` builds and runs the host suite and staged fake probe,
  and explicitly cross-builds both the iOS archive and hardware probe. Upstream's
  `ios` goal can otherwise silently omit the probe when host Mesa is absent.
- Host warning fixes: initialize every `Bindings` member in the probe, bind the
  small dimension pairs by reference, and use `hakios_metal_chip_known` directly
  instead of an unused-on-Linux helper in `src/c_api.cpp`.
- Update the newer upstream chip-acceptance assertion to A640 and explicitly
  reject A635; 0040 predates that assertion. Pass the numeric stop stage (`4`)
  to the fake probe so it cannot fall through to real-shader readback tests.
- `a6xx.hpp`: Clang's C++17 `__builtin_signbit` is not constexpr; use its constexpr
  bit-cast builtin for the sign bit, preserving negative zero. GCC keeps its
  existing implementation. Existing float/viewport tests cover this path.
- (k33 line, taken from vayu-dev `061e5a82`) `tools/compiler.mk`: `IOS_MESA_ROOT`
  defaults to `xnu-deps/build/mesa-metal` when it exists (`METAL=1 tools/mk-mesa.sh`:
  the iOS Mesa with `tools/mesa/metal/*.patch`, kept apart from the OpenGL ES
  library's `xnu-deps/build/mesa`), else to `xnu-deps/build/mesa` as before.
- (k33 line) `tools/kernels.mk`: `LLVM_AS` defaults to the newest `llvm-as-NN` (20 down to 15)
  found in PATH, else `llvm-as`: the kernels' `.ll` use opaque pointers, which Ubuntu
  22.04's `llvm-as` (LLVM 14) rejects (`expected metadata operand`); `llvm-as-18` is there.
- (k33 line) `Makefile`, `tools/compiler.mk`: the Linux `IOS_CXX` adds `-stdlib=libc++`; Ubuntu
  22.04's clang links C++ for Darwin with `-lstdc++`, which the SDK lacks, so the probe did not link.

## Build and scope

Task 3 audited these choices against Mesa 25.0.7; see the comparison table in
`../docs/metal-driver-inventory.md`. Further local fixes: A640's nonzero GMEM
base in framebuffer-fetch descriptors and fake validation; firmware WFM before
indirect draws; A640 defaults in `ir3-kernel`/`compiler-smoke`; shared-memory limit
uses the chip constant. Header annotations/declarations and the register checker
now allow checking the whole header against Mesa's generated output. Evidence:
`../docs/metal-step3-register-check.log` and `../docs/metal-step3-build.log`.

From a configured WSL `~/vayu` checkout, including the cross tools installed by
`xnu-deps/setup.sh` and `~/theos/sdks/iPhoneOS16.5.sdk`, one command builds the
library, both probes and tests without touching a phone:

```sh
make -C ~/vayu/hakios-metal -j3 vayu-check CXX=clang++
```

For a clean driver build, first run `make -C ~/vayu/hakios-metal clean`.
Host dependencies: Clang with C++17, GNU make, pthread; cross dependencies:
`xnu-deps/host/bin/xnu-clang++`, cctools ld64, `llvm-ar`, and the iOS SDK.
SDK and cross-compiler paths remain overridable through the upstream variables.

Outputs (ignored): `out/host/tests`, `out/host/probe-fake`,
`out/ios/libhakios-metal.a`, `out/ios/hakios-metal-probe`.
The iOS artifacts are built, not run. `probe-fake --stages 4` executes CP buffer and
submission operations in the simulator, not Adreno shader instructions.

## Transport hardware follow-up

Task-5 hardware follow-up: `../docs/metal-transport-abi.md` and
`../docs/metal-step5-phone.log` record real A640 stages 1–4, with UI processes
paused and resumed. This boot exposes `HakiosGPU`; `RedwoodGPU` remains an older
fallback. `probe --transport-audit` enables opt-in IOKit lifetime/error accounting
and logs the result after the user client is closed; `--service NAME` overrides
discovery for diagnosis. These options are Darwin-only. Audit failures are logs
and must be checked in addition to the probe exit status. No shader execution or
live AIR compilation is implied by those transport results.

## AIR frontend decision

**FACT (source inspection):** upstream does not carry a second AIR-to-NIR frontend
inside `hakios-metal`. `src/compiler.cpp` includes `nir_air.h`, calls
`air_to_nir`/`air_module_function_constants`, then Mesa ir3. Its
`tools/compiler.mk` links Mesa's `src/compiler/air/libair.a`; its comments identify
the source as the parent tree's `tools/mesa/air`, copied by `tools/mk-mesa.sh`.

**Decision:** use this repository's existing `tools/mesa/air` as the single AIR
frontend, with Mesa 25.0.7 and our patches. The driver's C++ compiler code is the
runtime adapter around that frontend and ir3, not an alternative frontend.

**FACT (header comparison against the pin):** our frontend is older than the
one in the pinned parent tree. Upstream `nir_air.h` adds decoded sampler state,
vertex-attribute and framebuffer-fetch metadata, threadgroup-argument metadata,
and stitching interfaces, among other changes. The current driver consumes newer
fields. Thus simply turning on `MESA=1` against our existing frontend is not a
supported build yet: reconcile/backport those APIs in `tools/mesa/air` before
enabling it. Keep one frontend in that location, not a second copy in the driver.

The baseline acceptance command deliberately has **no `MESA=1`**: it uses
precompiled fixtures and tests the named compiler refusal. It does not validate
live AIR compilation. The upstream `MESA=1`, `MESA_SRC`, `MESA_BUILD`,
`IOS_MESA_SRC`, and `IOS_MESA_BUILD` integration remains available for builds
with both host and iOS Mesa compiler archives. This task makes no claim that
those compiler-enabled builds or newer upstream fixtures have been validated
for A640. The 21 fixtures covered by 0043 are ported; additional upstream
fixtures must be checked/regenerated before hardware use in later tasks.

Acceptance evidence: `../docs/metal-step1-build.log` and the task-1 result in
`../docs/metal-roadmap.md`. No hardware result follows from a fake-GPU pass.

**Task 9 (reconciliation done):** `tools/mesa/air` is now the pinned upstream
frontend itself, with upstream's `air-ir3.patch`, `ir3-shift-split.patch` and
`ir3-ssbo-subdword.patch`. It reproduces 58 of 59 committed A640 fixtures byte
for byte (`../tools/metal-fixtures-reconcile.sh`; task 6's compute fixtures
included, so our older copy's lowering fixes are not needed on top); the 59th,
`argbuf_res`, is regenerated for A640 and runs on the phone (probe 6w).
`make test MESA=1` (host Mesa from `../tools/air/mk-host.sh`, found by default
in `../build/mesa-host`) passes, as do `vayu-check MESA=1` and `make ios MESA=1`
(`../tools/metal-app-build.sh`). One test needle was A635 register allocation.
Local driver change: `compileAir` uses the disk cache (`.kernel` files beside
the `.stage` ones); `hakios_metal_cache_counts()` reports hits and misses. The
plug-in in use is `../userland/src/HakiosMetalCA.m`, not `plugin/HakiosMetal.m`.
Evidence: `../docs/metal-app-results.md`, `../docs/metal-step9-*.log`.

## Compute hardware follow-up

Task 6: `../docs/metal-compute-results.md` records seventeen precompiled
compute fixtures on A640, including eleven regenerated additions. Seven full
main-probe runs pass (70 compute checks each), plus two half-query checks;
original GL UI stopped/resumed/live. The existing AIR frontend now fixes
pointer-phi offsets, typed half stores, array-store layer operands and static/
dynamic shared-memory overlap. These are offline fixture-generation fixes;
no MESA=1 build or replacement of the installed iOS Mesa library is implied.
`argbuf_res_air` still needs newer indirect-buffer metadata (task 9).

Use `probe --compute-only --transport-audit` for the full compute suite.
`--compute-core-only` restricts it to the original ported fixtures; no render
or deliberate recovery stage runs in either compute-only mode. Opt-in
`--dump-threadgroup FILE` captures that dispatch in Mesa RD format, using
`HAKIOS_METAL_RD` in `Gpu::submit`; ordinary submissions do not capture data.
The independent `texture-query-probe.cpp` exercises `tex_size_h`, unused by
upstream's probe. Newer graphics fixtures still require their A640 port/test.

## Render hardware follow-up

Task 7: `../docs/metal-render-results.md`. The full probe (stages 1–9) passes
on A640 in the default mode (GMEM for every pass that qualifies) and with
`--sysmem`: 253 checks, 0 failures, four runs, UI live. The 28 render fixtures
that were still A635 were regenerated for `0x06040001` by the pinned upstream
revision's own AIR frontend (a separate native build; see
`../tools/metal-render-fixtures.sh`), not by `tools/mesa/air`. That frontend
cannot yet compile ten of them and lowers some others differently. At A635 the
same generator reproduces all 28 committed fixtures byte for byte
(`../tools/metal-render-fixtures-control.sh`). This does not change the AIR
frontend decision above: these fixtures are what the reconciled frontend must
produce (task 9).

Probe options added: `--sysmem` (`CommandQueue::setGmemDefault(false)`),
`--render-stop SECTION`, and a closing line of draw passes by mode
(`CommandQueue::passTotals`). With `--transport-audit`, each failed IOKit
request is printed by number. One A640 difference is deliberate: a bicubic
sampler is refused (`kHasTexFilterCubic` false) and the probe checks that refusal.
