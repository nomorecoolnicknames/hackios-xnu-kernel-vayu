/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU: what the ports use of the way back to the loader (osfmk/arm64/loader_return.c, K1). The 8796 line's drivers
 * declared these one by one (extern void loader_log_putc(char c); ...) and loader_return() called each driver's
 * quiesce by name; here a driver registers its own, from its start (a VAYU_HOOK of sys/vayu_12377_abi.h):
 *
 *	static void vayu_mdp_quiesce(void) { ... }       // the boot framebuffer back on the panel
 *	loader_quiesce_register("vayu_mdp", vayu_mdp_quiesce);
 *
 * Quiesce functions run on every way back, a panic's too: interrupts off, no locks, no printf(), no allocation, the
 * other processors possibly stopped where they were. They put the hardware the way UEFI expects it. Stop functions
 * (loader_stop_register) run only on the orderly ways out (loader_return_after=, the host's stop) from a thread, with
 * stop_deadline= bounding each; LOADER_STOP_TIMED_ONLY: only on loader_return_after's. Both run in the order of
 * registration, the stops first; at most 32 of each.
 *
 * sys/hakios.xnu.h (Scion's seam) declares loader_log_line() and loader_return() too; the prototypes agree.
 */
#ifndef _SYS_VAYU_LOADER_H_
#define _SYS_VAYU_LOADER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* the loader's log: every console character reaches it already (sweet_log_append); these add lines of a driver's own */
void loader_log_putc(char c);
void loader_log_line(const char *line);
void loader_log_write(const char *s, int size);
/* the live log for a USB/network reader: characters from *pos on, at most max, *pos moved on */
uint32_t loader_log_copy(uint64_t *pos, char *out, uint32_t max);

/* is a loader listening (it patched start.s's XNULDRST slot)? */
bool loader_present(void);
/* the end of the run: back to the loader (returns only when none listens) */
void loader_return(void);
/* bring-up: back to the loader here if its payload asked for checkpoint n */
void loader_checkpoint(uint64_t n);

#define LOADER_STOP_TIMED_ONLY  0x1
int loader_quiesce_register(const char *name, void (*fn)(void));
int loader_stop_register(const char *name, void (*fn)(void), uint32_t extra_ms, uint32_t flags);
/* told var_save_ask= seconds (default 60) before loader_return_after= is up */
int loader_before_end_register(void (*fn)(const char *why));
/*
 * K24 (osfmk/arm64/psci_smp.c): how the other processors are stopped on every way back, a panic's too; true only when
 * each one is confirmed off. false: the kernel stays where it is instead of giving the loader the machine back.
 */
void loader_set_stop_others(bool (*fn)(void));

/* the watching loader's struct (bootcpu=N): USB handover, the usbctl mailbox, the host's stop */
uint64_t loader_live_usb_take(void);
void loader_live_usb_take_set(uint64_t value);
uint64_t loader_live_usb_given(void);
void loader_live_set_vctl(uint64_t mbox_pa, uint64_t window_pa, uint64_t window_len);
void loader_live_stop_set(void);
/* { physical address, bytes, kernel address } of a screen / sound recording a driver made, for the loader */
extern uint64_t loader_recording[3];
extern uint64_t loader_sound[3];

/* "memory: free ..., wired ... MB" (the heartbeat prints it every minute) */
void hakios_memory_line(char *buf, size_t len);

__END_DECLS

#endif /* _SYS_VAYU_LOADER_H_ */
