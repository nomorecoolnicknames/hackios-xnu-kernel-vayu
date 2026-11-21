/*
 * a64e_isa_emu: the ARMv8.3+/8.6 data-processing instructions newer Apple userland carries and the
 * Kryo 485 (Cortex-A76 / A55, ARMv8.2-A) lacks, emulated at the undefined-instruction trap after
 * sweet_a64e.c has passed on them; and the tri-state result of the trap emulation as a whole, so a
 * load that sweet_a64e.c emulates and that hits an unreadable address ends in SIGSEGV/SIGBUS at that
 * address, as the load itself would, instead of in SIGILL.
 *
 *   FCMA   FCMLA (vector, by element; #0/#90/#180/#270; H, S, D), FCADD (#90/#270; H, S, D)
 *   I8MM   SMMLA, UMMLA, USMMLA, USDOT (vector, by element), SUDOT (by element)
 *   BF16   BFDOT (vector, by element), BFMMLA, BFMLALB/BFMLALT (vector, by element), BFCVT, BFCVTN{2}
 *   DIT    MSR DIT, #imm / MSR DIT, Xt / MRS Xt, DIT on a per-thread bit
 *
 * Bit-exact with the Arm ARM pseudocode (FPMulAdd, FPAdd, FPRound, BFDotAdd, BFMulH, FPAdd_BF16,
 * BFRound, FPConvertBF) under the thread's FPCR (RMode, FZ, FZ16, DN; AH/FIZ/NEP/EBF are RES0 on these
 * cores, FEAT_AFP and FEAT_EBF16 being absent), with the cumulative FPSR flags. Integer arithmetic
 * only: nothing here touches the FP/SIMD registers of the CPU it runs on (the kernel's are not the
 * user's at EL1).
 *
 * The same file is the kernel's (osfmk/arm64/a64e_isa_emu.[ch], board config of sweet_a64e.c) and the
 * harness's (tools/a64e-test: trapemu_linux.c #includes the .c after giving it the kernel's names,
 * with A64E_ISA_HARNESS defined).
 *
 * MIT, the HakiOS authors. Written from the Arm Architecture Reference Manual (DDI 0487) pseudocode;
 * no code of another project.
 */
#ifndef A64E_ISA_EMU_H
#define A64E_ISA_EMU_H

#include <stdint.h>
#include <stdbool.h>

/* The outcome of emulating one trapped instruction. */
enum a64e_emu_result {
	A64E_EMU_NOT_OURS = 0,  /* not an instruction we emulate (or an unallocated form of one): SIGILL */
	A64E_EMU_HANDLED  = 1,  /* done: registers and PC updated */
	A64E_EMU_FAULT    = 2,  /* its memory access failed: deliver the data abort described by the fault */
};

/* The data abort an emulated access would have taken. Nothing of the instruction has been done. */
struct a64e_emu_fault {
	uint64_t addr;          /* the first byte the access could not reach (FAR) */
	uint32_t write;         /* 1: a store (copyout), 0: a load (copyin) */
	uint32_t vm;            /* 1: copyio took a VM fault there (recover_far); 0: the address was refused before
	                         *    any access (outside the user map): a translation fault, KERN_INVALID_ADDRESS */
};

/*
 * The per-thread state: in the kernel machine_thread.a64e, declared in osfmk/arm/thread.h with these
 * members; in the harness a thread-local of this one.
 *   fault_addr   where the last failed copy of the current trap failed
 *   fault        0 none, else 1 | 2 for a write | 4 when copyio took a VM fault there
 *   dit          PSTATE.DIT as the thread's last MSR DIT left it, 0 or 1
 */
#if defined(A64E_ISA_HARNESS)
struct a64e_thread_state {
	uint64_t fault_addr;
	uint32_t fault;
	uint32_t dit;
};
#else
#include <mach/arm/thread_status.h>     /* arm_saved_state_t */
#endif

/*
 * The whole trap, as sleh.c's handle_uncategorized() runs it: sweet_a64e_emulate() (which calls
 * a64e_isa_emulate() for what it does not know), and HANDLED / NOT_OURS / FAULT with *fault filled in
 * when the instruction's access failed (copyin/copyout through a64e_emu_copyin/_copyout).
 */
enum a64e_emu_result a64e_emulate_trap(arm_saved_state_t *ss, uint32_t w, struct a64e_emu_fault *fault);

/*
 * The instructions of this file, for sweet_a64e.c's fall-through: HANDLED (PC advanced) or NOT_OURS.
 * None of them accesses memory, so FAULT does not come from here; the type is the trap's.
 */
enum a64e_emu_result a64e_isa_emulate(arm_saved_state_t *ss, uint32_t w);

/*
 * The SIMD&FP part alone, on a register file: q[32] the V registers (16 bytes each, little-endian),
 * fpcr the FPCR, *fpsr or-ed with the cumulative flags raised. 1 when w is one of ours (and done),
 * 0 when not (nothing changed). For the differential tests (a64e_isa_diff.c).
 */
int a64e_isa_simd(uint32_t w, uint8_t q[32][16], uint32_t fpcr, uint32_t *fpsr);

/* copyin/copyout for the emulators: the same, and a failure is remembered for a64e_emulate_trap(). */
int a64e_emu_copyin(uint64_t uaddr, void *kaddr, unsigned long n);
int a64e_emu_copyout(const void *kaddr, uint64_t uaddr, unsigned long n);

#endif /* A64E_ISA_EMU_H */
