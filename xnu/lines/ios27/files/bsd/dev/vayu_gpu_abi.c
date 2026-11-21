/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU, K8: the calls the service HakiosGPU makes (sys/vayu_gpu.h), on the vayu GPU alone. On the 8796 line they
 * were bsd/dev/hakios.adreno.c's, redwood's Adreno 642L driver, which handed them to vayu_adreno.c's vayu_gpu_*()
 * while vayu's GPU was kept; that file's MMIO is another SoC's and is not in this tree. Here: the kept vayu GPU, or
 * "no GPU" -- not ready, no addresses, nothing mapped, every fence done (nothing will ever run), submissions refused.
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/hakios.adreno.c:3175-3560 (hakios_gpu_*, hakios_adreno_stop, gpu_log), only
 * the vayu branch of each.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vayu_gpu.h>
#include <sys/vayu_12377_abi.h>

uint32_t gpu_log;

bool
hakios_gpu_ready(void)
{
	return vayu_gpu_kept();
}

bool
hakios_gpu_parameter(uint32_t which, uint64_t *value)
{
	return vayu_gpu_kept() && vayu_gpu_parameter(which, value);
}

uint64_t
hakios_gpu_address(uint64_t size)
{
	return vayu_gpu_kept() ? vayu_gpu_address(size) : 0;
}

void
hakios_gpu_address_free(uint64_t address, uint64_t size)
{
	if (vayu_gpu_kept()) {
		vayu_gpu_address_free(address, size);
	}
}

bool
hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return vayu_gpu_kept() && vayu_gpu_map(address, phys, read_only);
}

void
hakios_gpu_mapped(void)
{
	if (vayu_gpu_kept()) {
		vayu_gpu_mapped();
	}
}

void
hakios_gpu_unmap(uint64_t address, uint64_t size)
{
	if (vayu_gpu_kept()) {
		vayu_gpu_unmap(address, size);
	}
}

void
hakios_gpu_to_memory(void *start, uint64_t size)
{
	if (vayu_gpu_kept()) {
		vayu_gpu_to_memory(start, size);
	}
}

void
hakios_gpu_for_device(void *start, uint64_t size)
{
	if (vayu_gpu_kept()) {
		vayu_gpu_for_device(start, size);
	}
}

void
hakios_gpu_for_cpu(void *start, uint64_t size)
{
	if (vayu_gpu_kept()) {
		vayu_gpu_for_cpu(start, size);
	}
}

/* (no core: nothing will run, so nothing is outstanding -- a waiter must not wait for ever) */
bool
hakios_gpu_done(uint32_t fence)
{
	return vayu_gpu_kept() ? vayu_gpu_done(fence) : true;
}

uint32_t
hakios_gpu_last(void)
{
	return vayu_gpu_kept() ? vayu_gpu_last() : 0;
}

void
hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	if (vayu_gpu_kept()) {
		vayu_gpu_worked(microseconds, submissions, by);
		return;
	}
	*microseconds = 0;
	*submissions = 0;
	for (int i = 0; i < 7; i++) {
		by[i] = 0;
	}
}

uint32_t
hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	return vayu_gpu_kept() ? vayu_gpu_submit(addresses, words, count) : 0;
}

void
hakios_gpu_report(const char *why)
{
	if (vayu_gpu_kept()) {
		vayu_gpu_report(why);
	}
}

void
hakios_gpu_stop(void)
{
	vayu_gpu_stop();        /* (returns at once when the GPU is not kept) */
}

/* the orderly end of a run (loader_stop_register, bsd/dev/vayu_adreno.c): from a thread that may wait */
void
hakios_adreno_stop(void)
{
	if (vayu_gpu_kept()) {
		HakiosGPUStop();        /* the clients' lock, then hakios_gpu_stop() */
		return;
	}
	vayu_adreno_stop();
}

/* metal=N: the IOAcceleratorES that names the Metal plug-in (iokit/Kernel/hakios.gpu.cpp), GPU or not */
VAYU_HOOK(VAYU_PHASE_AFTER_DEVFS, metal, HakiosAcceleratorStart);
