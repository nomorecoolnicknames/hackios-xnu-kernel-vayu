# Pinned Metal driver inventory

*From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (MIT).*

Audit date: 2026-11-07. Source baseline: local commit `bc19b43`, which vendors
upstream `63fc2b4633bac3af8e57dadab9f5a8d4cb940438` plus the A640 port and build fixes.
Paths and line numbers below are relative to `hakios-metal/` at that local commit.
This inventory comes from implementation bodies and selector registration, not the README.

**FACT (source)** means code exists with the stated limits. **Implemented** means
the described operation has a concrete implementation; **partial** means only a
subset or an incomplete bridge; **missing** includes objects that merely return
zero or exist as placeholders. None of these labels means A640 hardware success.

**FACT (build):** `metal-step1-build.log` records 261 host tests, fake-probe stages
1–4, and an arm64 library/probe build without `MESA=1`. It does not validate live
AIR compilation, the Objective-C plugin, display integration or GPU shader results.
The fake checks packet/state/memory behavior, not arbitrary Adreno shader execution.
The existing Mesa AIR API still needs reconciliation (`../hakios-metal/VAYU.md`).

## Metal API coverage

**Update (task 9, `docs/metal-app-results.md`):** the plug-in vayu builds is now
`userland/src/HakiosMetalCA.m` (upstream's full bridge over the same core), not
the bundled `plugin/HakiosMetal.m` the right-hand column below describes. Through
it, textures, samplers, render/compute pipelines and encoders, function constants,
blits and completion handlers ran on the A640 via the public API. The column is
kept as the audit of the bundled plug-in; re-audit the CA plug-in where a later
task depends on a row (IOSurface textures, drawables and GPU times for 10 and 13).
Compute kernels now also use the persistent cache (`compileAir`, task 9).

There are two separate surfaces: the C++ runtime/C bridge and the bundled
`plugin/HakiosMetal.m`. The plugin is substantially older in functionality than
the core. In particular, allocating a non-nil placeholder pipeline or texture does
not connect an application to the implemented render/compute encoders.

All entries in this table are **FACT (source)**; public exposure refers to this
bundled plugin, not to an unvendored upstream integration file.

| Feature | Core status and concrete implementation | Bundled plugin status / remaining gap |
| --- | --- | --- |
| Buffers | **Implemented:** GPU BO allocation, CPU mapping, IOVA, release and accounting in `src/gpu.cpp:45`; retained resources and pooled allocations in `src/command.cpp:59`. | **Partial:** length/bytes allocation uses GPU memory with `HAKIOS_METAL_GPU`, but falls back to CPU mmap on failure (`plugin/HakiosMetal.m:611`). No-copy wraps caller memory rather than importing a GPU BO (`:655`). Options are recorded; distinct private/managed storage and real purgeability are not implemented (`:514`, `:569`). |
| Textures, views and formats | **Partial:** actual format table `src/texture.cpp:36`; layout/type/usage restrictions `:269`; allocation `:441`; sampled/storage descriptors `:778`, `:887`; aliasing buffer/view/swizzle paths `:1038`, `:1067`, `:870`. 1D/2D, arrays, cubes/cube arrays, 3D and 2D multisample have paths; this is not every Metal texture type or usage. | **Missing functional texture objects:** `newTextureWithDescriptor:` is a placeholder (`plugin/HakiosMetal.m:272`). Bridge methods for views, CPU copies and resource IDs must be added. |
| Samplers | **Partial:** real descriptor packing `src/texture.cpp:1239`, nearest/linear/mips, six address modes, three border colors, LOD clamps, anisotropy 1–16 and comparison functions. A640 explicitly refuses bicubic. One sampler cannot combine clamp-to-zero and a nonzero border color. Compiler constexpr comparison samplers still refuse (`src/compiler.cpp:346`). | **Missing:** `newSamplerStateWithDescriptor:` returns a placeholder (`plugin/HakiosMetal.m:271`). |
| Render pipelines / draws | **Partial:** AIR stage compilation, linking and resource checks (`src/compiler.cpp:1284`, `:1399`); vertex fetch (`src/render.cpp:362`), real draw encoding (`:1249`), indexed/instanced/indirect forms (`:1455`, `:2077`). Vertex/fragment stages only; clip/cull distances, shader-selected layer/viewport and primitive ID are refused (`:1299`, `:1337`); direct negative base vertex refused (`:1498`). | **Missing:** pipeline factory is a placeholder (`plugin/HakiosMetal.m:273`); no registered render command encoder (`:1418`). Task 9 needs this bridge, not just a test application. |
| Compute pipelines / dispatch | **Partial:** AIR compilation (`src/compiler.cpp:1076`), resource validation, threadgroup/shared/private memory and PM4 execution (`src/compute.cpp:101`, `:607`); indirect dispatch (`:598`). Image atomics and dynamically chosen texture/sampler indices are refused (`src/compiler.cpp:154`, `:189`). | **Missing:** compute pipeline factory placeholder (`plugin/HakiosMetal.m:274`); no compute encoder registration (`:1418`). Compiler-enabled builds remain blocked by the known frontend mismatch. |
| Function constants | **Partial:** core specialization reaches translation and graphics cache keys (`src/compiler.cpp:273`, `:1076`, `:1161`; C entry points `src/c_api.cpp:1127`). | **Missing semantics:** specialized function lookup explicitly ignores supplied constants (`plugin/HakiosMetal.m:821`); dictionary/attribute reflection is empty (`:721`, `:727`). |
| Argument buffers | **Partial:** AIR layout query (`src/compiler.cpp:1560`) and CPU decoding of 8-byte member words into buffer addresses, texture IDs and sampler IDs, retaining resources (`:1594`). Requires a mapped set and known live resources. This is encode-time CPU expansion, not general GPU-side bindless access or GPU-written argument-buffer support. | **Missing:** no argument encoder factory or implementation in `plugin/HakiosMetal.m:1052`; no claim of full Metal argument-buffer tier conformance. |
| MRT | **Implemented subset:** up to eight color outputs, holes/discarded outputs, per-target format/blend validation (`src/render.cpp:1249`, `:1380`), descriptors (`:1645`), GMEM per-attachment shares (`:2159`). | **Missing** until functional pipelines/textures/render encoder exist. |
| Blending | **Implemented subset:** per-attachment masks, separate RGB/alpha operations and factors 0–18 (`src/render.cpp:850`), blend constants and dual-source output handling (`:1329`, `:1380`). Integer target blending refused. | **Missing:** placeholder render pipeline cannot carry this state (`plugin/HakiosMetal.m:273`). |
| Depth/stencil | **Partial:** tiled Depth32Float and Depth32Float_Stencil8/S8 plane (`src/texture.cpp:89`, `:223`); clear/attachment validation (`src/render.cpp:934`, `:991`), state programming (`:1614`, `:2020`). Stencil must be the depth texture's S8 plane, not an independent stencil attachment. Fragment depth writes have a path; shader-written stencil references refuse (`:1312`). | **Missing:** depth/stencil state factory placeholder (`plugin/HakiosMetal.m:270`). LRZ remains off, independently of ordinary depth testing. |
| MSAA | **Partial:** 2/4-sample 2D targets and clear/layout (`src/texture.cpp:269`), sample-count registers (`src/render.cpp:1664`), explicit 2D-engine resolve (`:2580`). Per-sample shading, centroid/sample barycentrics, shader sample IDs/masks refuse (`:1315`). Only Store/DontCare store actions are accepted (`:1034`), so automatic Metal resolve/store actions are missing. | **Missing** public pipeline/pass/resolve bridge. |
| Framebuffer fetch | **Partial:** compiler lowers target reads (`src/compiler.cpp:580`); attachment descriptors and barriers (`src/render.cpp:634`, `:1591`); GMEM fetch uses tile addresses plus per-tile cache invalidation (`:2490`). | **Missing** public render path; live compiler/A640 validation outstanding. |
| Indirect draws/dispatch | **Implemented subset:** execution-time GPU argument reads via `CP_DRAW_INDIRECT_MULTI` (`src/render.cpp:2077`) and `CP_EXEC_CS_INDIRECT` (`src/compute.cpp:545`); C entry points `src/c_api.cpp:1068`, `:1094`. | **Missing** public encoders and indirect command-buffer object support; a PM4 indirect buffer is not an `MTLIndirectCommandBuffer`. |
| Blits | **Partial:** GPU buffer copy/fill (`src/command.cpp:436`, `:477`); texture copy/scale/filter/rotation/resolve (`src/render.cpp:2544`), mip generation (`:3163`). Resolve cannot scale/filter, and copies into MSAA targets refuse (`:2584`). | **Partial:** only buffer copy/fill, endEncoding and no-op synchronizeResource are registered (`plugin/HakiosMetal.m:1438`). Texture blits/mipmap generation are not exposed. |
| Heaps | **Missing allocator:** texture byte planning exists (`src/c_api.cpp:646`), but no heap object, suballocation, residency/aliasing API in the core. Runtime buffer pools do not implement `MTLHeap`. | **Missing:** device methods fall through forwarding (`plugin/HakiosMetal.m:1052`, `:227`). |
| Events / fences | **Partial synchronization:** internal submit fences, ordered wait thread and completion handlers (`src/gpu.cpp:145`, `src/command.cpp:603`, `:639`). | **Missing Metal semantics:** event/fence factories create empty objects (`plugin/HakiosMetal.m:275`); no encoder signal/wait or shared event implementation. Internal submission fences are not `MTLFence`/`MTLEvent`. |
| IOSurface import | **Partial:** service BO import (`src/gpu.cpp:82`), cached wrappers (`src/command.cpp:207`), linear surface format/pitch/size validation (`src/texture.cpp:921`), 420f/420v two-plane sampling (`:1008`). Requires the kernel's `kHakiosSurface` contract; ordinary rows must have compatible 64-byte alignment. Compressed display imports and display ownership/fence transfer are not provided. | **Missing:** no registered `newTextureWithDescriptor:iosurface:plane:` wrapper in `plugin/HakiosMetal.m:1052`; vayu kernel ABI compatibility remains task 5/10 work. |
| Drawables / presentation | **Missing:** core can draw into a surface but has no drawable lifecycle, display scheduling or presentation ownership. | **Missing:** command buffer registration has no `presentDrawable:` family (`plugin/HakiosMetal.m:1418`); no drawable implementation. CAMetalLayer integration remains task 10. |
| Shader / pipeline caching | **Partial:** persistent graphics-stage serialization keyed by bitcode, constants, IBO base, known-buffer specialization and build identity (`src/compiler.cpp:1146`, `:1161`, `:1197`, `:1284`); writable `HAKIOS_METAL_CACHE`, colon-separated read-only `HAKIOS_METAL_CACHE_SEED`. Queue caches uploaded program BOs (`src/command.cpp:177`). Compute `compileAir` does not use the graphics disk-cache path (`src/compiler.cpp:1076`). | **Missing** real pipeline objects / full binary archive API. Core stage cache is not a complete persistent compute/render pipeline cache. |
| Queues and async commit | **Implemented core:** submit without waiting, retained in-flight work, ordered waiter/callback thread and GPU reset detection (`src/command.cpp:603`, `:639`). Optional C bridge deferred recorder (`src/c_api.cpp:40`, `:790`). | **Partial:** GPU-enabled command buffer/handlers/wait/status/error exist (`plugin/HakiosMetal.m:1176`, `:1207`, `:1418`). Enqueue and waitUntilScheduled are no-ops, scheduled handlers run before core submit, GPU times return zero (`:1221`, `:1236`, `:1251`, `:1304`). |

### Format limits that matter to callers

**FACT (source):** the implementation table (`src/texture.cpp:36`) includes A8;
R/RG/RGBA integer, normalized and float variants; BGRA8 and sRGB variants;
B5G6R5/BGR5A1, RGB/BGR10A2 and RG11B10; depth/stencil; ETC2/EAC; ASTC LDR/sRGB/HDR;
and PVRTC1 RGB/RGBA 2/4 bpp. This is a selected table, not all combinations of
component widths/classes. Unlisted formats refuse (`:319`). PVRTC is CPU-decoded
to RGBA8 (`:592`, `:643`), not sampled compressed in hardware. ASTC HDR entries
exist; correct A640 HDR decoding is unvalidated. Block-compressed formats are
sample-only; two-plane YCbCr is IOSurface-only (`:325`, `:348`). Mip/type/depth
restrictions are enforced in `:269`, including one-level 3D and multisample paths.
Some header comments describe older narrower coverage; the bodies govern this audit.

## Private Metal.framework interface

**FACT (source):** `plugin/HakiosMetal.m:1582` dynamically subclasses `_MTLDevice`,
declares `MTLDevice` and runtime `MTLDeviceSPI`, and registers
`initWithAcceleratorPort:` (`:1625`). Initialization saves the port but does not
open it as the GPU connection (`:49`); the optional GPU path opens a service
separately (`:124`; `src/iokit_transport.cpp:69` tries HakiosGPU then RedwoodGPU).
The advertised name still says **Adreno 642L**, not A640 (`:62`). Registry ID is
the stored port, maxBufferLength is fixed at 256 MiB, and threadgroup dimensions
are each reported as 1024 (`:68`, `:75`, `:82`); these answers need verification
against the vayu limits rather than being treated as a capability profile.

**FACT (source):** `_MTLCommandQueue` is used where available (`:1449`); resource
classes and their public/private protocol families are registered (`:1030`,
`:1107`, `:1124`). The concrete selector sets are `:1052`, `:1418`, `:1625`.
Unknown protocol-declared selectors obtain their signatures at runtime (`:206`)
and `forwardInvocation:` logs once and writes a zero return (`:227`). Thus private
requests such as `featureProfile` are not implemented capability answers. Methods
absent from those protocol descriptions can fall through to superclass behavior;
forwarding is not proof that every private call is safe. `supportsFeatureSet:`
always returns NO (`:997`); no explicit family/feature-profile implementation is
registered. Functional private capability reporting and additional selectors
must be learned from actual task 8/11 runtime logs.

**FACT (source):** registration is process-gated by `metal=` and
`HakiosMetal.clients` (`:1521`, `:1553`): default level 1 allows metal_test, level 2
allows all except backboardd, level 3 all. A clients-file entry (including `*`)
also allows a process. Task 8 must preserve the GL fallback and audit both gates.
Declaring private protocols or returning non-nil stub objects is not functional
Metal.framework conformance.

## Hardware and performance inventory

All implementation claims here are **FACT (source)**. No phone measurement was
performed for this audit. Upstream hardware observations in comments refer to
other hardware and are not promoted to vayu facts.

| Feature | Status, mechanism and limits | Source anchors |
| --- | --- | --- |
| GMEM rendering | **Partial, already present.** Explicit per-command-buffer GMEM gate; pass eligibility and draw replay checks, threshold/dual recording and optional measured tuning. Falls back to sysmem for depth/stencil, MSAA, compressed targets, unsupported color classes/layouts, shader memory writes, visibility queries or attachment alias sampling. | `src/c_api.cpp:895`; `src/render.cpp:2159`, `:2303`, `:2845`, `:3238` |
| Mode selection | **Partial:** draw-count history/threshold and timing-based selection exist; not a general bandwidth/cost model. Tuner tries sysmem then GMEM and periodically revisits the other mode, using an exponential average. | `src/render.cpp:2872`; `src/command.cpp:275`, `:313` |
| Bin sizing/layout | **Implemented subset:** color-only attachment shares below CCU, 16 KiB-aligned; bins aligned 32×16, width capped 1024, height capped 1008 and reduced to fit each attachment; render-area shrink preserves descriptor bin width. Port constants claim 1 MiB GMEM and two CCUs. Constants are source facts here, **not** the Mesa comparison required by task 3. | `src/render.cpp:2193`, `:2211`, `:3276`; `include/hakios-metal/a6xx.hpp:1412`, `:4098` |
| Binning / visibility streams | **Partial, already present:** optional binning pass, 32-pipe VSC draw/primitive buffers, tile-select stream consumption, conditional draw replay and end-of-draw markers. Overflow is checked and disables subsequent binning; fixed buffer pitches and same-frame overflow behavior need A640 stress validation. Visibility *query results* are a separate sysmem feature. | `src/render.cpp:2354`, `:2450`, `:2500`, `:3295`; `include/hakios-metal/render.hpp:540`; `src/command.cpp:262`, `:268` |
| UBWC | **Partial:** Mesa fdl6 layout + flag plane for one-level single-sample 2D RGBA8/BGRA8; CPU-accessible staging for sampled textures, freshening through GPU blit; compressed render targets have descriptor/write paths. Requires `MESA=1`, unavailable in the accepted baseline. Compressed clear refuses; GMEM loads/stores refuse compressed attachments; no compressed IOSurface/display import. | `src/texture.cpp:441`, `:778`; `src/render.cpp:128`, `:999`, `:2178`, `:2647`, `:2770`, `:3102` |
| LRZ | **Missing:** depth testing exists, but GRAS/RB LRZ controls and buffer registers are zeroed. Emitting LRZ_FLUSH alone does not enable LRZ. Needs allocation/layout, initialization, direction/invalidation and draw eligibility tracking. | `src/render.cpp:1688`, `:1175`, `:1230` |
| Load/store elision | **Partial, already present:** coalesced passes; GMEM touched/read/write masks, DontCare load skipping when granules stay inside the image, StoreDontCare skipping, tile clears and render-area restriction. Sysmem DontCare stores still write memory. Depth/stencil are excluded from GMEM. | `src/render.cpp:1034`, `:2845`, `:3238`, `:3283`, `:2477` |
| Resolves | **Partial:** GMEM store via BLIT_EVENT_STORE and CCU resolve flush/wait; explicit MSAA source resolve by 2D engine. Not automatic Metal resolve attachment/store-action support, nor arbitrary depth/stencil resolve. | `src/render.cpp:2339`, `:2504`, `:2515`, `:2580` |
| Hazards / flushes | **Partial:** conservative start/tail invalidates, WFI/flush waits, framebuffer-fetch barriers and register deduplication; not a general fine-grained hazard graph. Some controls are diagnostic bisect options, not safe speed settings. | `src/render.cpp:1141`, `:1209`, `:1591`, `:2724`; `src/compute.cpp:555` |
| Timing / counters | **Partial, already present:** CP_REG_TO_MEM timer register 0x0980, pass timestamps and optional serialized per-draw stamps; conversion assumes 19.2 MHz. Pass/draw/dispatch/copy/clear/GMEM counts and allocation counters have C APIs. Plugin GPUStartTime/GPUEndTime still return zero; no complete fixed-scene CPU/fence/traffic percentile report. | `src/command.cpp:244`, `:332`, `:351`; `src/c_api.cpp:206`, `:868`, `:885`, `:948`; `plugin/HakiosMetal.m:1304` |
| Async / CPU reuse | **Implemented subset:** asynchronous in-flight submission and waiter, pooled BOs/slabs, uploaded program reuse, shared private-memory allocation, optional deferred recording and register shadowing. These are not a Metal heap or a full general texture pool. | `src/command.cpp:80`, `:143`, `:177`, `:564`, `:589`, `:603`, `:639`; `src/c_api.cpp:40`; `src/render.cpp:2724` |

## Consequences for the roadmap

**INFERENCE from the source audit:** steps 8–10 must budget for a real Objective-C
bridge. Registering the current bundle alone can produce a device and non-nil
placeholders but cannot run the public render/compute program required by step 9.
The A640 name/capability answers, constants, textures, samplers, pipelines,
encoders, IOSurface wrappers and drawable presentation need explicit work.

Steps 13–16 now reuse existing instrumentation, GMEM/binning, UBWC and elision
instead of assuming they are absent. First validate them against the task 3 Mesa
audit and task 4 streams, then measure on the phone. LRZ is a new implementation;
UBWC/GMEM extensions are bounded by the exclusions above. Step 17 should also
measure existing async/pooling/deferred paths before adding another mechanism.

Task 12's framework/application results also remain outstanding; no app has been
tested by this inventory and no third-party app selection has been inferred.

## Task 3: A640 constants and generation audit (2026-11-07)

**FACT (source comparison):** the reference is **Mesa 25.0.7**, selected by
`tools/mk-mesa.sh:43`, archive SHA256
`592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6`
(verified against the existing WSL archive). Sources below are relative to that
release's `src/`, under `~/vayu/xnu-deps/build/mesa/mesa-25.0.7/`.
The local Darwin/AIR patches do not change its A640 device facts. `D` below means
`freedreno/common/freedreno_devices.py`; `H` means our
`hakios-metal/include/hakios-metal/a6xx.hpp`. This is a source audit, **not** new
phone evidence. Mesa's own TODOs remain TODOs, particularly the instruction-cache
size and depth-bounds quirk.

| Constant or generation choice | A640 value / local implementation | Mesa 25.0.7 source and disposition |
|---|---|---|
| Chip identity | `kChipId=0x06040001`, gen2 | `D:68–77,603–638`: GPUId(640) becomes `0x060400ff`; `common/freedreno_dev_info.c:26–48` matches wildcard patch byte. Matches. Kernel `vayu_adreno.c:1793,3350` reports this chip and GPU_ID 640. |
| Shader register file / preload | 96 vec4 registers; 64 instruction groups | `D:378–380` gen2. Matches `H` chip facts; preload caps in compute/render use 64. Mesa labels cache size TODO. |
| SPs, fibers, waves | 2 SPs/CCUs, 8192 fibers/SP, wave granularity 2, base threadsize 64, max waves 16 | `D:138,157–165,607–615`. Matches; important private-memory allocation difference from A635's 4096 fibers/SP. |
| Shared memory / double threadsize | 32768 bytes/group; 64/128 threads per wave supported | `D:335,612`; `a6xx/fd6_compute.cc:111–139` under Gallium path `gallium/drivers/freedreno/`. Matches; `kMaxSharedBytes` now aliases the chip constant. |
| LPAC copies | false; omit `SP_CS_CNTL_0/1`, `HLSQ_CS_CTRL_REG1` | `D:378–387` vs gen4 `D:422`; Gallium `a6xx/fd6_compute.cc:131–142,231–235`. `compute::Options::hlsqCopies=false` by default, fake rejects those writes. Matches. |
| Early preamble / cubic / concurrent resolve | all false | None enabled in base+gen2 (`D:325–387`); enabled selectively in later generations. Core defaults match; bicubic sampler refused. Diagnostic/bisect overrides are not evidence of A640 support. |
| GMEM fast clear | true | `D:328`; `fd6_emit.cc:833–837`. Matches. LRZ fast clear/direction tracking/feedback remain unavailable on gen2, and LRZ is disabled locally. |
| Physical GMEM | 1 MiB | Mesa obtains this from kernel, not D (`tu_device.cc:1440`). Local `vayu_adreno.c:3363` returns `0x100000`; kernel UCHE range `:2985–2986` is `[0x100000,0x1fffff]`. Our GMEM offsets are relative, not that physical base. |
| GMEM texture-fetch base | **fixed** from 0 to `0x100000` plus attachment offset; CCU/MRT/blit offsets remain relative | Turnip `tu_cmd_buffer.cc:1934–1949` adds `physical_device->gmem_base`; local kernel `vayu_adreno.c:3374–3376` reports `0x100000`. Old A635/A650 zero-base assumption was wrong for A640. `stageTextures` and fake bounds now include A640 base; framebuffer-fetch test asserts literal `0x100000`. |
| Sysmem CCU | 2 CCUs × 64 KiB each for depth and color; depth offset 0, color offset `0x20000`; register `0x10000000` | `D:337–339,607`; Gallium `a6xx/fd6_screen.cc:149–180` and `fd6_emit.cc:792–849`; Turnip `tu_device.cc:1410–1441`. Matches `rbCcuCntlSysmem()`, including WFI before reconfiguration. |
| GMEM CCU | color 32 KiB at `0xf8000`, register `0x7c400000`; tile allocation ends before cache | Same sources. Mesa QUARTER is enum **2**, divided as `1<<2`; our fraction **4** is the divisor. Matches `kGmemCcuColorOffset`, planner and existing static assertion. Depth unused in supported color-only GMEM passes. |
| Tile geometry / VSC | tile alignment 32×16, max 1024×1008; event-blit granules 16×4; 32 VSC pipes | `D:138–158,608–611`. Matches planner and VSC register arrays. Per-attachment 16 KiB allocation alignment is a conservative driver choice, not an extra physical chip fact. VSC stream capacity/overflow still needs task 14 hardware stress. |
| `PC_POWER_CNTL` / `PC_MODE_CNTL` | `1` / `0x1f` | `D:619,624`; matches static table. |
| `TPL1_DBG_ECO_CNTL` / `GRAS_DBG_ECO_CNTL` | `0x8000` / `0` | `D:620–621`; matches. No A635 `0x05008000`. |
| `SP_CHICKEN_BITS` / `SP_DBG_ECO_CNTL` | `0x420` / `0` | `D:622,625`; matches. No gen4 `0x1400` / `6`. |
| `UCHE_CLIENT_PF` / `UCHE_UNKNOWN_0E12` | `4` / `1` | `D:623,632`; matches userspace static table; kernel boot initialization is a separate phase. |
| `RB_DBG_ECO_CNTL`, including blit | `0x04100000` for both | `D:626–627`; matches, so no 2D-blit switch needed. Corrected misleading A635 source comment. |
| `HLSQ_DBG_ECO_CNTL` / `RB_UNKNOWN_8E01` / `VPC_DBG_ECO_CNTL` | `0` / `1` / `0x02000000` | `D:628–631`; matches. |
| Common A6xx static state | TPL1 B605 `0x44`; HLSQ BE00/BE01 `0x80/0`; SP float F16_NO_INF; SP perf enable `0x3f`; SP TP mode `0xa0`; GRAS SC cache-line setting 2; GRAS8110 `2`; unused stages/fetch sizes zeroed | Gallium `a6xx/fd6_emit.cc:896–1030`. Matches `H:kStaticState` and `render::startBatch`. Compute prelude intentionally replaces SP_FLOAT_CNTL with 0, following computerator; this is a documented emitter choice, not a gen4 magic constant. |
| Primitive/constant/private-memory conventions | prim allocation 7, branchstack max64, pipeline constants512 vec4, VS const floor8; program128-byte, private-memory32-byte alignment | `D:341`; `freedreno/ir3/ir3_compiler.c` (branchstack and max_const_pipeline), `ir3_compiler_nir.c` VS constlen floor, `vulkan/tu_shader.cc` alignment assertions. Matches existing `H` constants. |
| Cache events | UCHE clean4, invalidate49; RB done22; CCU invalidate depth24/color25, resolve26, depth clean28/color clean29 | `freedreno/common/freedreno_gpu_event.h:60–85`, plus generated `adreno_pm4.xml.h`. Matches; asynchronous clean/done events use address+sequence payload and wait, invalidates use one word. Do not substitute A7xx CACHE_CLEAN/CACHE_INVALIDATE7 events. |
| LRZ/BLIT/context events | clear37, flush38, BLIT30, LABEL63, binning UNK_2C/2D44/45 | Same event map and generated headers. Matches. LRZ_FLUSH is emitted without claiming LRZ allocation/use. LABEL around long-CS FS_INSTRLEN workaround matches `a6xx/fd6_compute.cc:186–207`. |
| Indirect draw firmware quirk | **true; fixed**: emit `CP_WAIT_FOR_ME` before indexed and non-indexed indirect draw | `D:382`; `common/freedreno_dev_info.h:100–110` explicitly describes a630 SQE firmware; Turnip `tu_cmd_buffer.cc:6352–6410`, Gallium `a6xx/fd6_barrier.cc:141–153`. Old port omitted WFM. We conservatively emit it for every indirect draw instead of tracking pending WFI. Both packet orders covered by host test. |
| Depth-bounds quirk | true in Mesa; no depth-bounds API or compressed depth targets served locally | `D:383`, `common/freedreno_dev_info.h:112–115`, `tu_cmd_buffer.cc:775`. No applicable emission to fix. Add the workaround if that feature is implemented. |
| UBWC configuration | highest bank bit15, swizzle levels `0x6`, macrotile mode0; no 8bpp UBWC or linear-mipmap fallback | `D:136–137,386,616–617`. Existing kernel `vayu_adreno.c:2941–2945,2995–2999,3350–3353` programs/reports SM8150 configuration. Userspace does not overwrite NC_MODE. Matches; no A635 HBB14/macrotile1 assumptions in runtime. |
| UBWC layout / descriptor fields | Mesa `fdl6_layout` queried with A640 chip ID, TILE6_3; only 2D RGBA8/BGRA8, one mip/sample | `freedreno/fdl/fd6_layout.c:27–66,120–174`, `fd6_view.c`; local `texture.cpp:441–488`. RGBA8 block16×4, base alignment4096; layout returns pitches/flag offsets. No-Mesa refuses UBWC. Limited formats avoid unsupported gen2 8bpp case; no hardware validation claimed. |
| Firmware ownership | kernel loads `a630_sqe.fw`, `a640_gmu.bin`, `a640_zap.mdt/.bNN`; SQE version≥`0x190`; old GMU startup signature `0xbabeface` | Mesa device tables do not load/select these firmwares. Local kernel `vayu_adreno.c:73–74,1793–1795,2457–2483` owns startup and validates SQE. Consistent with Mesa's documented a630 SQE indirect quirk; this task does not revalidate firmware startup on hardware or copy A635/A740 firmware. |
| Shader tool defaults | **fixed** `compiler-smoke` and `ir3-kernel` defaults from A635 `0x06030500` to A640 `0x06040001` | Matches `tools/kernels.mk:24`, runtime compiler lookup and D's A640 row. Explicit `--chip-id` remains available; no fixtures silently regenerated. Newer inherited fixtures still require later A640 regeneration/hardware checks. |

**FACT (automated check):** `docs/metal-step3-register-check.log` records **1879**
matching constants (1821 values, 36 shifts, 4 multipliers, 18 strides), zero missing
sources/mismatches, and **80** explicitly marked non-header constants. Those markers
include the chip facts covered above and common descriptor sizes/alignments/ir3
conventions; they are not all independently verified by the script. Split three
multi-variable declarations, taught the checker C++ integer division, and added
16 missing generated-header references so the existing checker can finish.
No numeric register/header mismatch was found.

**FACT (WSL validation):** `docs/metal-step3-build.log` records
`make -C ~/vayu/hakios-metal -j3 vayu-check CXX=clang++`: **261 tests, zero failed
checks**, arm64 iOS library/probe built, fake-probe stages 1–4 **zero failures**.
The indirect-draw test checks WFM placement for both forms; the GMEM-fetch test
checks the nonzero A640 base. The fake does not
model the actual firmware race. The WSL pull was attempted and refused unrelated
dirty files, which were preserved; only `hakios-metal/` was synced for this build.
No `MESA=1`, live AIR compilation, real shader execution or phone operation was
performed. Task 4 captures and tasks 5–7 hardware validation remain necessary.
