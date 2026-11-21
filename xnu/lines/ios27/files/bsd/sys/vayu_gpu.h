/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU, K8: the Adreno 640 (bsd/dev/vayu_adreno.c), the msm-shaped calls of the service HakiosGPU
 * (iokit/Kernel/hakios.gpu.cpp) and the narrow layer between them (bsd/dev/vayu_gpu_abi.c). The 8796 line declared
 * these in its bsd/sys/hakios.input.h, which on this tree is Scion's and is left alone.
 */
#ifndef _SYS_VAYU_GPU_H_
#define _SYS_VAYU_GPU_H_

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

__BEGIN_DECLS

/* the service's calls (hakios.gpu.cpp), answered by the kept GPU or, without one, by "no GPU" */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void *start, uint64_t size);
/* CPU -> GPU (clean) and GPU -> CPU (invalidate, after the fence) apart: vayu_adreno.c's vayu_gpu_for_device/_for_cpu */
void hakios_gpu_for_device(void *start, uint64_t size);
void hakios_gpu_for_cpu(void *start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void hakios_gpu_report(const char *why);
void hakios_gpu_stop(void);
extern uint32_t gpu_log;                /* boot-arg gpu_log=1: a line for every submission */
extern uint32_t gpu_va;                 /* boot-arg gpu_va */

/* the orderly end of the run (loader_stop_register): the clients' lock, then the GPU given back */
void hakios_adreno_stop(void);

/* bsd/dev/vayu_adreno.c */
void vayu_adreno_init(void);
void vayu_adreno_stop(void);
void vayu_adreno_quiesce(void);
bool vayu_gpu_kept(void);
bool vayu_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t vayu_gpu_address(uint64_t size);
void vayu_gpu_address_free(uint64_t address, uint64_t size);
void vayu_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most, uint64_t *lost,
    uint32_t *refused);
bool vayu_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void vayu_gpu_mapped(void);
void vayu_gpu_unmap(uint64_t address, uint64_t size);
void vayu_gpu_to_memory(void *start, uint64_t size);
void vayu_gpu_for_device(void *start, uint64_t size);
void vayu_gpu_for_cpu(void *start, uint64_t size);
bool vayu_gpu_done(uint32_t fence);
uint32_t vayu_gpu_last(void);
void vayu_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
uint32_t vayu_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
void vayu_gpu_report(const char *why);
bool vayu_gpu_recover(void);
void vayu_gpu_stop(void);
/* is the display putting frames on the panel just now (K5 may set it; NULL: no) */
extern uint32_t (*hakios_display_frames)(void);

/* iokit/Kernel/hakios.gpu.cpp */
void HakiosGPUStart(void);
void HakiosGPUStop(void);
void HakiosAcceleratorStart(void);      /* metal=N: the IOAcceleratorES naming HakiosMetal (vayu27's AFTER_DEVFS hook) */
/* vayu_gpu_selftest=1 (bsd/dev/vayu_adreno.c, after the service started): a submission through the service's path */
void vayu_gpu_selftest(void);
/* vayu_gpufb_test=1 (iokit/Kernel/arm/SweetFramebuffer.cpp): the GPU fills a surface (HakiosGPUFillSurface), shown */
void sweet_fb_vayu_gpu_test(void);

__END_DECLS

#endif /* _SYS_VAYU_GPU_H_ */
