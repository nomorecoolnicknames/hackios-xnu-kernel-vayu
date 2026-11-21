/*
 * Copyright (c) 2026 Scion Project, HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (POCO X3 Pro, SM8150): what the files of the S9's layer that every generic board compiles ask of the board, where
 * the S9's own drivers (Exynos USB console, Mali-G72, PMIC over ACPM, the S9's UFS root) or SWEET's Qualcomm ones would
 * answer. Those drivers are not built for VAYU (config option scion_exynos), so each entry point is answered here by "not
 * here": nothing below touches hardware, and no PMIC register is read or written. Taken from Scion's
 * bsd/dev/frankel_stubs.c (FRANKEL, same reason), with the UFS root and the S9 boot-argument refusal left out: VAYU
 * builds none of the S9's drivers, so there is nothing a boot argument could start.
 *
 * Owners: the port that brings the real driver deletes its section here in the same commit (PORTING.md):
 *   hakios_loader_core_*                      K1 (loader); sweet_ufs_rootdev is bsd/dev/vayu_ufs.c's (K6)
 *   starlte_gpu_user_*                        K8 (Adreno) if HakiosGPU takes the same calls, else stays
 *   sweet_spmi_*                              stays: no PMIC access from SweetBattery on vayu (A-D8 brings hakios.battery.c)
 */
#include <sys/vayu_12377_abi.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <pexpert/pexpert.h>

/* the kernel log ring the S9 keeps for its UFS/SD log (starlte_usbcon.c); sweet_log_append(), the console's hook, is
 * the loader's log on VAYU: osfmk/arm64/loader_return.c (K1) */
void a5xelte_log_snapshot(const char **ring, uint32_t *size, uint64_t *total);
void
a5xelte_log_snapshot(const char **ring, uint32_t *size, uint64_t *total)
{
	*ring = NULL; *size = 0; *total = 0;
}

/* the S9's USB console verbs (hakios.dwc3.c) */
void hakios_input_console_button(const char *arg);
void hakios_input_console_button(const char *arg) { (void)arg; }
void hakios_input_console_touch(const char *arg);
void hakios_input_console_touch(const char *arg) { (void)arg; }
void hakios_exynos_cpufreq_touch(void);
void hakios_exynos_cpufreq_touch(void) { }

/* the S9's Mali-G72 (StarbiteMali.cpp): no GPU on this board yet (vayu's Adreno 640 is K8's, bsd/dev/vayu_adreno.c) */
int starlte_gpu_user_ready(void);
int starlte_gpu_user_ready(void) { return 0; }
bool starlte_gpu_user_param(uint32_t param, uint64_t *value);
bool starlte_gpu_user_param(uint32_t param, uint64_t *value) { (void)param; (void)value; return false; }
bool starlte_gpu_user_map(uint64_t va, const uint64_t *pages, unsigned n);
bool starlte_gpu_user_map(uint64_t va, const uint64_t *pages, unsigned n) { (void)va; (void)pages; (void)n; return false; }
bool starlte_gpu_user_map_prot(uint64_t va, const uint64_t *pages, unsigned n, bool gpu_writes);
bool starlte_gpu_user_map_prot(uint64_t va, const uint64_t *pages, unsigned n, bool gpu_writes) { (void)va; (void)pages; (void)n; (void)gpu_writes; return false; }
void starlte_gpu_user_unmap(uint64_t va, unsigned n);
void starlte_gpu_user_unmap(uint64_t va, unsigned n) { (void)va; (void)n; }
int starlte_gpu_user_submit(uint64_t jc, uint32_t requirements);
int starlte_gpu_user_submit(uint64_t jc, uint32_t requirements) { (void)jc; (void)requirements; return ENODEV; }
int starlte_gpu_user_submit2(uint64_t jc, uint32_t requirements, bool async, uint64_t *seq);
int
starlte_gpu_user_submit2(uint64_t jc, uint32_t requirements, bool async, uint64_t *seq)
{
	(void)jc; (void)requirements; (void)async;
	if (seq) {
		*seq = 0;
	}
	return ENODEV;
}
int starlte_gpu_user_wait_seq(uint64_t seq, bool block);
int starlte_gpu_user_wait_seq(uint64_t seq, bool block) { (void)seq; (void)block; return 0; }
int starlte_gpu_user_wait_seq_for(uint64_t seq, uint64_t timeout_ns, uint64_t *done);
int
starlte_gpu_user_wait_seq_for(uint64_t seq, uint64_t timeout_ns, uint64_t *done)
{
	(void)seq; (void)timeout_ns;
	if (done) {
		*done = 0;
	}
	return 0;
}
uint64_t starlte_gpu_user_seq(void);
uint64_t starlte_gpu_user_seq(void) { return 0; }

/* the S9's PMIC over ACPM (starlte_usbcon.c): refused. No PMIC access on this board, read or write. */
bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val) { (void)type; (void)reg; (void)val; return false; }
bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val);
bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val) { (void)type; (void)reg; (void)val; return false; }
/* the S9's return to TWRP through its PMU: nothing here (vayu returns to its loader through loader_return(), K1) */
void starlte_return_to_recovery(void);
void starlte_return_to_recovery(void) { printf("VAYU: no return-to-recovery path on this board\n"); }

/* SWEET's Qualcomm SPMI (SweetBattery.cpp): not on this board, so SweetBattery keeps its static fallback */
int sweet_spmi_init(void);
int sweet_spmi_init(void) { return ENODEV; }
int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out);
int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out) { (void)sid; (void)addr; (void)out; return ENODEV; }

/* The loader's core taken over by the kernel (xnu-8796's core0=1, used by hakios.ufs.c's loader-served mode, ufs=3): not on
 * this board until the UFS port (K6) brings hakios.ufs.c's */
void hakios_loader_core_take(void);
void hakios_loader_core_take(void) { }
int hakios_loader_core_taken(void);
int hakios_loader_core_taken(void) { return 0; }

/*
 * The S9's audio (bsd/dev/hakios.exynos_abox.c, compiled for every generic board, nothing without abox=N): its clock
 * domain's rate through the Exynos APM and its memory under 4 GB are the S9's own (hakios.exynos_acpm_dvfs.c, the S9's
 * DMA pool). On this board: no APM is asked, no memory is given, and the driver's steps stop there.
 */
#include <mach/vm_types.h>
int hakios_acpm_dvfs_get(uint32_t index, uint32_t *khz);
int hakios_acpm_dvfs_get(uint32_t index, uint32_t *khz) { (void)index; (void)khz; return ENODEV; }
int hakios_acpm_dvfs_set(uint32_t index, uint32_t khz);
int hakios_acpm_dvfs_set(uint32_t index, uint32_t khz) { (void)index; (void)khz; return ENODEV; }
vm_offset_t hakios_dma32_alloc(vm_size_t size, uint64_t *pa);
vm_offset_t hakios_dma32_alloc(vm_size_t size, uint64_t *pa) { (void)size; (void)pa; return 0; }

/* the S9's sensor hub (hakios.ssp.c), which SweetHIDSensors.cpp's accelerometer and gyroscope take their samples from: none
 * here, so they publish and stay still (vayu's sensors are SLPI's, a later port: bsd/dev/vayu_slpi.c, vayu_ssc.c) */
struct hakios_sensor_sample;
void hakios_ssp_tap(void (*tap)(const struct hakios_sensor_sample *));
void hakios_ssp_tap(void (*tap)(const struct hakios_sensor_sample *)) { (void)tap; }
