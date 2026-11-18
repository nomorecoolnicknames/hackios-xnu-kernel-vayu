# Metal transport on vayu (task 5, 2026-11-07)

*From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (MIT).*

**FACT (`metal-step5`, `metal-step5-phone.log`):** the direct hakios-metal probe
passes stages 1–4 in 40 repeated hardware runs (320 checks), followed by a final
health run. Twenty runs have SpringBoard and backboardd stopped; twenty have
them resumed, including ten during synthetic page swipes. All 41 cleanup audits
report zero outstanding GEM handles, CPU mappings and queues, zero transport
errors, and successful `IOServiceClose`. No kernel change was needed.

**Execution adjustment:** the binaries and `rc.jobs.metal-transport` were uploaded
to `/private/var/tmp` and executed by the existing `/redwood/sbin/init FILE`
runner, rather than rebuilding a root image. No reboot, boot configuration or
partition command was used. The isolated case uses `SIGSTOP` on both UI processes
and verifies `SSTOP` before and after the probes, then resumes and verifies them.
Their existing resources remain resident. An independent 20-second watchdog
resumes them if the runner is killed. This is isolation from the UI's execution,
**not** evidence of a cold boot before UI initialization or of no other open GPU
client. That startup-only scenario remains untested.

## Service and wire ABI

**FACT (phone):** automatic discovery opens **`HakiosGPU`**. An explicit
`--service RedwoodGPU --stages 1` returns status 100, reporting no such service.
The roadmap's earlier assumption that this boot exposes `RedwoodGPU` was wrong.
**FACT (source):** `xnu/files/iokit/Kernel/hakios.gpu.cpp` registers `HakiosGPU`
and `HakiosGPUUserClient`; `bsd/dev/vayu_adreno.c` supplies vayu's hardware
operations. The transport retains `RedwoodGPU` as an older-kernel fallback.

The driver's ABI definitions live in
`hakios-metal/include/hakios-metal/gpu.hpp`; its adapter is
`hakios-metal/src/iokit_transport.cpp`; ownership is implemented in
`hakios-metal/src/gpu.cpp`. These are **struct method selectors**, not encoded
Linux ioctl values. Open with `IOServiceOpen(service, mach_task_self(), 0, ...)`.
Call `IOConnectCallStructMethod(connection, selector, &data, size, &data, &size)`.
All pointers in the structures below are 64-bit addresses in the calling task;
the kernel copies submission arrays with `copyin`. Native arm64 alignment applies.

| Selector | Structure bytes | Contract used by hakios-metal |
| --- | ---: | --- |
| `0x40` GET_PARAM | 24 | `{u32 pipe,param; u64 value; u32 len,pad}`, pipe=1 |
| `0x42` GEM_NEW | 16 | `{u64 size; u32 flags,handle}`; flags `0x20000` write-combined; returns handle |
| `0x43` GEM_INFO | 24 | `{u32 handle,info; u64 value; u32 len,pad}`; info=1 returns GPU IOVA |
| `0x44` CPU_PREP | 24 | `{u32 handle,op; i64 seconds,nanoseconds}`; wait before CPU access |
| `0x45` CPU_FINI | 4 | `{u32 handle}`; currently a kernel no-op |
| `0x46` GEM_SUBMIT | 72 | Flags=pipe 1, BO/command counts and pointers, returned fence, queue ID; remaining fields zero, fence_fd=-1 |
| `0x47` WAIT_FENCE | 32 | `{u32 fence,flags; i64 seconds,nanoseconds; u32 queueid}` including trailing padding |
| `0x4a` QUEUE_NEW | 12 | `{u32 flags,prio,id}`; returns a client-local ID |
| `0x4b` QUEUE_CLOSE | 4 | `{u32 id}`; currently a kernel no-op |
| `0x09` GEM_CLOSE | 8 | `{u32 handle,pad}`; releases this client's handle |

Submission BO entries are 16 bytes: `{u32 flags,handle; u64 presumed}`.
Read=1, write=2; command BO first, remaining resource handles deduplicated.
Command entries are 32 bytes: `{u32 type,submit_idx,submit_offset,size,pad,nr_relocs;
u64 relocs}`. Type=1, byte-aligned-to-4 offset/size, no relocations. The GPU reads
PM4 at the buffer's actual IOVA plus offset. The current kernel limits a client
to 8192 buffers, 256 MiB per buffer, 8192 submission BOs and 128 command entries.

Map each GEM handle with `IOConnectMapMemory64(connection, handle,
mach_task_self(), ..., kIOMapAnywhere)`. Unmap with `IOConnectUnmapMemory64`
**before** GEM_CLOSE. The runtime rounds allocations to 16 KiB; the GPU uses
4 KiB mappings internally. Ordinary GEM mappings are write-combined by default;
`gpu_cached=1` changes kernel behavior and was not used here. Submission performs
the kernel's memory barrier/cache maintenance. Fence completion precedes readback.

Wait deadlines are **absolute nanoseconds of `mach_continuous_time`**, converted
using `mach_timebase_info`, split into signed seconds/nanoseconds; not durations
or wall time. The kernel polls completion with its lock released between polls.
CPU_PREP conservatively waits for the global last submission. All clients share
the GPU address space and ring under a global lock; queue IDs do not establish
independent hardware scheduling or address-space isolation.

Adapter error mapping: success→0, NoMemory→ENOMEM, BadArgument→EINVAL,
Timeout→ETIMEDOUT, Busy→EBUSY, NotFound→ENOENT, Unsupported→ENOSYS;
other IOKit errors are printed and become EIO. RAII destroys command buffers and
queue resources, unmaps/closes BOs, closes the queue, then closes the user client.
The kernel may cache released BO storage until client close; clientClose waits
for outstanding GPU work and drains owned/deferred/cached allocations.

GET_PARAM observed here: GPU_ID=640, CHIP_ID=`0x06040001`, frequency=499 MHz.
Source A640 values also include GMEM_SIZE=`0x100000`, GMEM_BASE=`0x100000`,
priorities=1, highest bank bit=15, UBWC swizzle=6. **Important limitation:** this
vayu source returns a constant zero for FAULTS; the probe's `FAULTS 0` lines
are not a functioning reset counter. Successful fences/readbacks and no
transport errors provide the positive hardware evidence in this task.

Selector `0x7e` imports IOSurfaces using the 40-byte `HakiosSurface` layout (exercised on hardware by task 10: QuartzCore's CAMetalLayer drawables, linear BGRA8, 4544-byte rows; `docs/metal-layer-results.md`).
It is outside stages 1–4 and **not validated here**. Fence FDs, syncobjs, per-process
GPU page tables, reset recovery and shader execution are likewise not validated.

## Checks and lifetime evidence

| Stage | Hardware check per run |
| --- | --- |
| 1 | CP_MEMCPY of 500000 bytes, all bytes and untouched edges checked |
| 2 | Five offset fills: 4, 256, 260, 65540 and 999996 bytes, checked |
| 3 | Forty queued command buffers, completion-handler order and final memory |
| 4 | Five 16 MiB copies complete; reports best commit-to-fence time |

Stage 4 is a fence/completion check, not full readback of the 16 MiB allocations;
stages 1–3 supply readback correctness. During the final active run, S5FPS logs
show 33, 45 and 48 display frames in successive roughly one-second intervals
while the probes pass. These observations demonstrate coexistence with the GL
compositor, not controlled performance measurements.

`--transport-audit` enables opt-in accounting in the real IOKit adapter. Its
destructor checks successful outstanding handle/map/queue operations and the
actual unmap/close return codes. The report validator requires every audit PASS,
as audit failures are logged rather than changing the probe's exit code.
Within both isolated groups every connection's first allocation reused IOVA
`0x20cb0000`. The quiet live group stayed at 34552–34553 wired pages after its
first iteration. The active group's IOVA changed to `0x212c0000` as the UI made
resources and then remained there. System-wide wired pages vary with the UI and
other daemons (including after the run); they are not a GPU allocation counter.

**INFERENCE:** balanced transport ownership, successful client closes, repeated
IOVA reuse and the quiet memory plateau show no accumulating leak in these runs.
They do not prove the absence of every kernel-side leak or long-duration leak.

## Reproduction and artifacts

Build from the Windows checkout in WSL after attempting the normal pull:
`bash tools/metal-transport-build.sh`. It preserves unrelated build-checkout
changes and syncs only the two changed driver sources, runs `vayu-check`, and
cross-builds the runner. **FACT:** 261 host tests / zero failed checks, fake
stages 1–4 / zero failures, iOS library/probe/runner cross-build succeeded.
The pull was refused due to pre-existing unrelated changes, which were preserved.
This is the no-Mesa-compiler baseline; no AIR/compiler work is claimed.

Use `sweet-sh.py --put LOCAL REMOTE MODE` to upload `userland/obj/metal-probe`,
`userland/obj/metal_transport_run` (755) and `userland/rc.jobs.metal-transport`
(644) under `/private/var/tmp`. Capture the console with `sweet-netlog.py`, then
run `/redwood/sbin/init /private/var/tmp/rc.jobs.metal-transport` through sweetd.
The init runner sends job output to the console, not the requesting socket.
Check both runner statuses and all probe/audit results; init itself returns zero
even when one job fails. Avoid running this UI-pausing test during user activity.

Tracked artifacts: `metal-step5-phone.log` (filtered evidence, including final
health check), `metal-step5-summary.json` (groups and deployed binary/job hashes),
`metal-step5-build.log`. Full consoles stay ignored in `logs/metal-step5/`.
`python tools/metal-transport-report.py` validates both captured job runs and
regenerates the filtered artifacts. Phone responsive afterward, both UI processes
resumed with their original PIDs, battery 33 C / 90%. Next: task 6, compute fixtures.
