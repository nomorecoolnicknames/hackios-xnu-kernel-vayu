/*
 * a64e_isa_emu: FCMA, I8MM, BF16 and DIT for the undefined-instruction trap, and the trap's tri-state
 * result (a64e_isa_emu.h says what and why). Integer arithmetic only, no FP/SIMD register of the CPU
 * it runs on is touched: the emulated instruction's registers are the thread's saved NEON state.
 *
 * Kernel: osfmk/arm64/a64e_isa_emu.c (conf/files.arm64, next to sweet_a64e.c), called by
 * sweet_a64e.c for what it does not know and by sleh.c's handle_uncategorized() for the whole trap.
 * Harness: tools/a64e-test/trapemu_linux.c #includes this file with A64E_ISA_HARNESS and the kernel's
 * names (arm_saved_state_t, current_thread() with machine.uNeon, copyin/copyout) given; the two
 * copies are the same file (build.sh checks).
 *
 * The floating-point semantics are the Arm ARM's (DDI 0487, shared/functions/float): FPUnpack,
 * FPProcessNaNs3, FPMulAdd, FPAdd, FPRoundBase (tininess before rounding, FZ/FZ16 flushing with UFC,
 * IDC for single/double inputs only), FPConvertBF; and for BFDOT/BFMMLA with FPCR.EBF = 0 (FEAT_EBF16
 * absent): BFDotAdd = FPAdd_BF16(addend, FPAdd_BF16(BFMulH, BFMulH)), round to odd, fixed flush to zero,
 * default NaN, no flags. FPCR.AH/FIZ/NEP (FEAT_AFP) and EBF are RES0 here: FPNeg flips the sign of a
 * NaN too.
 *
 * MIT, the HakiOS authors (LICENSE of the HakiOS repository). Written from the Arm ARM pseudocode.
 */
#if !defined(A64E_ISA_HARNESS)
#include <pexpert/arm64/board_config.h>
#endif

#if defined(A64E_ISA_HARNESS) || defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE) || \
	defined(ARM64_BOARD_CONFIG_VAYU)

#if !defined(A64E_ISA_HARNESS)
#include <mach/mach_types.h>
#include <mach/arm/thread_status.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <arm/thread.h>
#include <string.h>
#include <arm64/a64e_isa_emu.h>

/* the thread's own state (osfmk/arm/thread.h: struct machine_thread, member a64e) */
#define A64E_THREAD_STATE()     (&current_thread()->machine.a64e)

/* the FAR of the copyio fault that made the last copyin/copyout fail, as handle_kernel_abort_recover
 * left it; 0 (no VM fault: the address was refused by copy_validate) with the address asked for */
static inline void
a64e_copyio_begin(void)
{
	current_thread()->machine.recover_esr = 0;
	current_thread()->machine.recover_far = 0;
}

static inline uint32_t
a64e_copyio_far(uint64_t uaddr, unsigned long n, uint32_t write, uint64_t *far)
{
	thread_t th = current_thread();
	(void)n;
	(void)write;
	if (th->machine.recover_esr == 0) {
		*far = uaddr;
		return 0;
	}
	*far = th->machine.recover_far;
	return 1;
}
#endif /* !A64E_ISA_HARNESS */

typedef unsigned __int128 a64e_u128;

#define A64E_TF_FAULT   1u      /* a64e_thread_state.fault */
#define A64E_TF_WRITE   2u
#define A64E_TF_VM      4u

#define FPCR_FZ16       (1u << 19)
#define FPCR_RMODE(c)   (((c) >> 22) & 3u)
#define FPCR_FZ         (1u << 24)
#define FPCR_DN         (1u << 25)
#define FPSR_IOC        (1u << 0)
#define FPSR_OFC        (1u << 2)
#define FPSR_UFC        (1u << 3)
#define FPSR_IXC        (1u << 4)
#define FPSR_IDC        (1u << 7)

/* rounding: FPCR.RMode, and BFRound's round to odd with overflow to infinity */
enum { R_RN = 0, R_RP = 1, R_RM = 2, R_RZ = 3, R_ODD_INF = 4 };
/* FPType */
enum { C_ZERO, C_NUM, C_INF, C_QNAN, C_SNAN };

struct a64e_fmt {
	uint8_t e, f;           /* exponent and fraction bits */
};
static const struct a64e_fmt fmt_h = { 5, 10 }, fmt_s = { 8, 23 }, fmt_d = { 11, 52 }, fmt_bf = { 8, 7 };

/* an unpacked operand: a C_NUM is man / 2^63 * 2^exp, man's top bit set */
struct a64e_fpu {
	int cls;
	uint32_t sign;
	int exp;
	uint64_t man;
	uint64_t bits;
};

static inline int
fmt_bias(const struct a64e_fmt *f)
{
	return (1 << (f->e - 1)) - 1;
}

static inline uint64_t
fmt_sign(const struct a64e_fmt *f, uint32_t sign)
{
	return (uint64_t)sign << (f->e + f->f);
}

static inline uint64_t
fmt_inf(const struct a64e_fmt *f, uint32_t sign)
{
	return fmt_sign(f, sign) | (uint64_t)((1u << f->e) - 1) << f->f;
}

static inline uint64_t
fmt_dnan(const struct a64e_fmt *f)
{
	return fmt_inf(f, 0) | 1ULL << (f->f - 1);
}

static inline unsigned
clz128(a64e_u128 x)
{
	uint64_t hi = (uint64_t)(x >> 64), lo = (uint64_t)x;
	return hi ? (unsigned)__builtin_clzll(hi) : 64 + (unsigned)__builtin_clzll(lo);
}

/* FPUnpack: flush (FZ, or FZ16 for half) makes a denormal a zero, with IDC when idc */
static void
fp_unpack(const struct a64e_fmt *f, uint64_t bits, bool flush, bool idc, uint32_t *flags, struct a64e_fpu *u)
{
	unsigned F = f->f, E = f->e;
	uint64_t frac = bits & ((1ULL << F) - 1);
	unsigned ex = (unsigned)(bits >> F) & ((1u << E) - 1);
	u->bits = bits;
	u->sign = (uint32_t)(bits >> (E + F)) & 1;
	u->exp = 0;
	u->man = 0;
	if (ex == 0) {
		if (frac == 0) {
			u->cls = C_ZERO;
		} else if (flush) {
			u->cls = C_ZERO;
			if (idc) {
				*flags |= FPSR_IDC;
			}
		} else {
			int lz = __builtin_clzll(frac);
			u->cls = C_NUM;
			u->man = frac << lz;
			u->exp = (63 - lz) + 1 - fmt_bias(f) - (int)F;
		}
	} else if (ex == (1u << E) - 1) {
		u->cls = frac == 0 ? C_INF : ((frac >> (F - 1)) & 1) ? C_QNAN : C_SNAN;
	} else {
		u->cls = C_NUM;
		u->man = ((1ULL << F) | frac) << (63 - F);
		u->exp = (int)ex - fmt_bias(f);
	}
}

/*
 * FPRoundBase: sign, the exact value's exponent (sig / 2^127 * 2^exp, sig's top bit set, its low bits
 * a sticky bit below every bit that counts) to the format, under mode, flushing tiny results when flush.
 */
static uint64_t
fp_round(const struct a64e_fmt *f, uint32_t sign, int exp, a64e_u128 sig, unsigned mode, bool flush, uint32_t *flags)
{
	unsigned F = f->f, E = f->e;
	int emin = 1 - fmt_bias(f);
	uint64_t s = fmt_sign(f, sign);
	if (flush && exp < emin) {
		*flags |= FPSR_UFC;
		return s;
	}
	int bexp = exp - emin + 1;
	unsigned shift = 0;
	if (bexp <= 0) {
		shift = (unsigned)(emin - exp);
		bexp = 0;
	}
	unsigned ts = 127 - F + shift;          /* sig >> ts is the integer mantissa */
	uint64_t mant;
	bool err;
	int cmp;                                /* the error against half an ulp */
	if (ts > 128) {
		mant = 0;
		err = true;
		cmp = -1;
	} else if (ts == 128) {
		mant = 0;
		err = true;
		cmp = sig == (a64e_u128)1 << 127 ? 0 : 1;
	} else {
		a64e_u128 rem = sig & (((a64e_u128)1 << ts) - 1), half = (a64e_u128)1 << (ts - 1);
		mant = (uint64_t)(sig >> ts);
		err = rem != 0;
		cmp = rem < half ? -1 : rem == half ? 0 : 1;
	}
	if (bexp == 0 && err) {
		*flags |= FPSR_UFC;
	}
	bool up = false, oinf = false;
	switch (mode) {
	case R_RN:
		up = cmp > 0 || (cmp == 0 && (mant & 1));
		oinf = true;
		break;
	case R_RP:
		up = err && !sign;
		oinf = !sign;
		break;
	case R_RM:
		up = err && sign;
		oinf = sign;
		break;
	case R_ODD_INF:
		oinf = true;
		break;
	default:
		break;
	}
	if (up) {
		mant++;
		if (mant == 1ULL << F) {
			bexp = 1;                       /* a denormal rounded up to the smallest normal */
		}
		if (mant == 1ULL << (F + 1)) {
			bexp++;
			mant >>= 1;
		}
	}
	if (err && mode == R_ODD_INF) {
		mant |= 1;
	}
	if (bexp >= (int)((1u << E) - 1)) {
		*flags |= FPSR_OFC | FPSR_IXC;
		return oinf ? fmt_inf(f, sign) : s | (uint64_t)((1u << E) - 2) << F | ((1ULL << F) - 1);
	}
	if (err) {
		*flags |= FPSR_IXC;
	}
	return s | (uint64_t)bexp << F | (mant & ((1ULL << F) - 1));
}

/*
 * x + y, each sig / 2^125 * 2^exp (top bit at 125, or 0 for a zero; the low 20 bits of a nonzero one
 * clear), exactly but for a sticky bit: false for an exact zero, else the sum as fp_round takes it.
 */
static bool
fp_add_core(uint32_t sx, int ex, a64e_u128 x, uint32_t sy, int ey, a64e_u128 y, uint32_t *sign, int *exp,
    a64e_u128 *sig)
{
	if (x == 0) {
		sx = sy;
		ex = ey;
		x = y;
		y = 0;
	}
	if (x == 0) {
		return false;
	}
	if (y != 0) {
		if (ey > ex || (ey == ex && y > x)) {
			uint32_t ts = sx;
			int te = ex;
			a64e_u128 tv = x;
			sx = sy;
			ex = ey;
			x = y;
			sy = ts;
			ey = te;
			y = tv;
		}
		unsigned d = (unsigned)(ex - ey);
		if (d >= 128) {
			y = 1;
		} else if (d) {
			y = (y >> d) | ((y & (((a64e_u128)1 << d) - 1)) != 0);
		}
		x = sx == sy ? x + y : x - y;
		if (x == 0) {
			return false;
		}
	}
	unsigned p = 127 - clz128(x);
	*sign = sx;
	*exp = ex + (int)p - 125;
	*sig = x << (127 - p);
	return true;
}

static uint64_t
fp_nan(const struct a64e_fmt *f, const struct a64e_fpu *u, uint32_t fpcr, uint32_t *flags)
{
	if (u->cls == C_SNAN) {
		*flags |= FPSR_IOC;
	}
	if (fpcr & FPCR_DN) {
		return fmt_dnan(f);
	}
	return u->bits | 1ULL << (f->f - 1);
}

/* FPMulAdd(addend, op1, op2, fpcr): addend + op1 * op2, one rounding */
static uint64_t
fp_muladd_soft(const struct a64e_fmt *f, uint64_t addend, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t *flags)
{
	bool half = f->f == 10;
	bool flush = (fpcr & (half ? FPCR_FZ16 : FPCR_FZ)) != 0;
	struct a64e_fpu a, n, m;
	fp_unpack(f, addend, flush, !half, flags, &a);
	fp_unpack(f, op1, flush, !half, flags, &n);
	fp_unpack(f, op2, flush, !half, flags, &m);
	bool inf1 = n.cls == C_INF, inf2 = m.cls == C_INF, zero1 = n.cls == C_ZERO, zero2 = m.cls == C_ZERO;
	if (a.cls >= C_QNAN || n.cls >= C_QNAN || m.cls >= C_QNAN) {
		/* FPProcessNaNs3: the signalling ones first, each kind in the order addend, op1, op2 */
		const struct a64e_fpu *p = a.cls == C_SNAN ? &a : n.cls == C_SNAN ? &n : m.cls == C_SNAN ? &m :
		    a.cls == C_QNAN ? &a : n.cls == C_QNAN ? &n : &m;
		uint64_t r = fp_nan(f, p, fpcr, flags);
		if (a.cls == C_QNAN && ((inf1 && zero2) || (zero1 && inf2))) {
			*flags |= FPSR_IOC;
			r = fmt_dnan(f);
		}
		return r;
	}
	uint32_t sp = n.sign ^ m.sign;
	bool infa = a.cls == C_INF, zeroa = a.cls == C_ZERO, infp = inf1 || inf2, zerop = zero1 || zero2;
	if ((inf1 && zero2) || (zero1 && inf2) || (infa && infp && a.sign != sp)) {
		*flags |= FPSR_IOC;
		return fmt_dnan(f);
	}
	if ((infa && !a.sign) || (infp && !sp)) {
		return fmt_inf(f, 0);
	}
	if (infa || infp) {
		return fmt_inf(f, 1);
	}
	if (zeroa && zerop && a.sign == sp) {
		return fmt_sign(f, a.sign);
	}
	a64e_u128 x = 0, y = 0;
	int ex = 0, ey = 0;
	if (!zeroa) {
		x = (a64e_u128)a.man << 62;
		ex = a.exp;
	}
	if (!zerop) {
		a64e_u128 p = (a64e_u128)n.man * m.man;         /* in [2^126, 2^128), its low 22 bits clear */
		if (p >> 127) {
			y = p >> 2;
			ey = n.exp + m.exp + 1;
		} else {
			y = p >> 1;
			ey = n.exp + m.exp;
		}
	}
	unsigned mode = FPCR_RMODE(fpcr);
	uint32_t sign;
	int e;
	a64e_u128 sig;
	if (!fp_add_core(a.sign, ex, x, sp, ey, y, &sign, &e, &sig)) {
		return fmt_sign(f, mode == R_RM);       /* an exact zero: its sign from the rounding mode */
	}
	return fp_round(f, sign, e, sig, mode, flush, flags);
}

#if defined(A64E_ISA_HARNESS) && defined(A64E_ISA_HOSTFP)
/*
 * The harness's cross-check build only (never the kernel's): the host's own FMADD and FADD
 * (trapemu_fp.S, at EL0) under the user's FPCR. Exact for these forms: FPMulAdd's operands are
 * FMADD's (Va, Vn, Vm) in the same NaN order, and FPAdd of an FPNeg'd element is FADD of the word
 * with its sign bit flipped, a NaN's included (FSUB would keep the NaN's sign).
 */
uint64_t a64e_host_fmadd(uint64_t a, uint64_t n, uint64_t m, uint32_t size, uint32_t fpcr, uint32_t *fpsr);
uint64_t sweet_a64e_fp_addsub(uint64_t a, uint64_t b, uint32_t op_size, uint32_t fpcr, uint32_t *fpsr);

static inline uint32_t
fmt_size(const struct a64e_fmt *f)
{
	return f->f == 10 ? 1 : f->f == 23 ? 2 : 3;
}

static uint64_t
fp_muladd(const struct a64e_fmt *f, uint64_t addend, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t *flags)
{
	(void)fp_muladd_soft;
	return a64e_host_fmadd(addend, op1, op2, fmt_size(f), fpcr, flags);
}

static uint64_t
fp_add(const struct a64e_fmt *f, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t *flags)
{
	return sweet_a64e_fp_addsub(op1, op2, fmt_size(f), fpcr, flags);
}
#else
#define fp_muladd fp_muladd_soft

/* FPAdd(op1, op2, fpcr) is FPMulAdd(op1, op2, 1.0) step for step: the same NaN order (op1, op2), the
 * same infinity, zero and rounding cases, op2 * 1.0 exact */
static uint64_t
fp_add(const struct a64e_fmt *f, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t *flags)
{
	return fp_muladd(f, op1, op2, (uint64_t)fmt_bias(f) << f->f, fpcr, flags);
}
#endif

/* ---- BF16 (FEAT_EBF16 absent: FPCR.EBF reads 0) */

#define BF_DNAN 0x7fc00000u

/* BFUnpack: a denormal is a zero, every NaN a quiet one, no flags */
static void
bf_unpack(uint32_t bits, struct a64e_fpu *u)
{
	uint32_t none = 0;
	fp_unpack(&fmt_s, bits, true, false, &none, u);
	if (u->cls == C_SNAN) {
		u->cls = C_QNAN;
	}
}

/* BFMulH: the product of two bfloat16s as a single, BFRound */
static uint32_t
bf_mulh(uint16_t op1, uint16_t op2)
{
	struct a64e_fpu a, b;
	uint32_t none = 0;
	bf_unpack((uint32_t)op1 << 16, &a);
	bf_unpack((uint32_t)op2 << 16, &b);
	if (a.cls == C_QNAN || b.cls == C_QNAN) {
		return BF_DNAN;
	}
	uint32_t sign = a.sign ^ b.sign;
	bool inf1 = a.cls == C_INF, inf2 = b.cls == C_INF, zero1 = a.cls == C_ZERO, zero2 = b.cls == C_ZERO;
	if ((inf1 && zero2) || (zero1 && inf2)) {
		return BF_DNAN;
	}
	if (inf1 || inf2) {
		return (uint32_t)fmt_inf(&fmt_s, sign);
	}
	if (zero1 || zero2) {
		return sign << 31;
	}
	a64e_u128 p = (a64e_u128)a.man * b.man;
	int e = a.exp + b.exp;
	if (p >> 127) {
		e++;
	} else {
		p <<= 1;
	}
	return (uint32_t)fp_round(&fmt_s, sign, e, p, R_ODD_INF, true, &none);
}

/* FPAdd_BF16: single + single, BFRound; an exact zero is +0 (round to odd) */
static uint32_t
bf_add(uint32_t op1, uint32_t op2)
{
	struct a64e_fpu a, b;
	uint32_t none = 0;
	bf_unpack(op1, &a);
	bf_unpack(op2, &b);
	if (a.cls == C_QNAN || b.cls == C_QNAN) {
		return BF_DNAN;
	}
	bool inf1 = a.cls == C_INF, inf2 = b.cls == C_INF, zero1 = a.cls == C_ZERO, zero2 = b.cls == C_ZERO;
	if (inf1 && inf2 && a.sign != b.sign) {
		return BF_DNAN;
	}
	if ((inf1 && !a.sign) || (inf2 && !b.sign)) {
		return (uint32_t)fmt_inf(&fmt_s, 0);
	}
	if (inf1 || inf2) {
		return (uint32_t)fmt_inf(&fmt_s, 1);
	}
	if (zero1 && zero2 && a.sign == b.sign) {
		return a.sign << 31;
	}
	uint32_t sign;
	int e;
	a64e_u128 sig;
	if (!fp_add_core(a.sign, a.exp, zero1 ? 0 : (a64e_u128)a.man << 62, b.sign, b.exp,
	    zero2 ? 0 : (a64e_u128)b.man << 62, &sign, &e, &sig)) {
		return 0;
	}
	return (uint32_t)fp_round(&fmt_s, sign, e, sig, R_ODD_INF, true, &none);
}

/* BFDotAdd(addend, op1_a, op1_b, op2_a, op2_b) with FPCR.EBF = 0 */
static uint32_t
bf_dotadd(uint32_t addend, uint16_t n_a, uint16_t n_b, uint16_t m_a, uint16_t m_b)
{
	return bf_add(addend, bf_add(bf_mulh(n_a, m_a), bf_mulh(n_b, m_b)));
}

/* FPConvertBF(op, fpcr): single to bfloat16 under FPCR (RMode, FZ, DN), with flags */
static uint16_t
bf_cvt(uint32_t op, uint32_t fpcr, uint32_t *flags)
{
	bool flush = (fpcr & FPCR_FZ) != 0;
	struct a64e_fpu u;
	fp_unpack(&fmt_s, op, flush, true, flags, &u);
	switch (u.cls) {
	case C_SNAN:
	case C_QNAN:
		if (u.cls == C_SNAN) {
			*flags |= FPSR_IOC;
		}
		return (fpcr & FPCR_DN) ? 0x7fc0 : (uint16_t)((op | 0x400000u) >> 16);
	case C_INF:
		return (uint16_t)(u.sign << 15 | 0x7f80);
	case C_ZERO:
		return (uint16_t)(u.sign << 15);
	default:
		return (uint16_t)fp_round(&fmt_bf, u.sign, u.exp, (a64e_u128)u.man << 64, FPCR_RMODE(fpcr), flush, flags);
	}
}

/* ---- the instructions */

static inline uint64_t
el_get(const uint8_t *v, unsigned i, unsigned bytes)
{
	uint64_t x = 0;
	memcpy(&x, v + i * bytes, bytes);       /* little-endian */
	return x;
}

static inline void
el_set(uint8_t *v, unsigned i, unsigned bytes, uint64_t x)
{
	memcpy(v + i * bytes, &x, bytes);
}

static inline int64_t
i8(uint8_t b, bool is_unsigned)
{
	return is_unsigned ? (int64_t)b : (int64_t)(int8_t)b;
}

int
a64e_isa_simd(uint32_t w, uint8_t q[32][16], uint32_t fpcr, uint32_t *fpsr)
{
	unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
	unsigned Q = (w >> 30) & 1, size = (w >> 22) & 3;
	unsigned H = (w >> 11) & 1, L = (w >> 21) & 1, M = (w >> 20) & 1;
	const uint8_t *n = q[rn], *d = q[rd];
	uint8_t r[16];
	uint32_t flags = 0;
	memset(r, 0, sizeof(r));

	if ((w & 0xBF20E400) == 0x2E00C400 ||                   /* FCMLA (vector) */
	    (w & 0xBF009400) == 0x2F001000) {                   /* FCMLA (by element) */
		bool elem = (w & 0x01000000) != 0;
		unsigned rot = elem ? (w >> 13) & 3 : (w >> 11) & 3, index = 0;
		if (size == 0 || (size == 3 && !Q)) {
			return 0;
		}
		if (elem) {
			if (size == 1) {
				if (H && !Q) {
					return 0;
				}
				index = H << 1 | L;
			} else if (size == 2) {
				if (L || !Q) {
					return 0;
				}
				index = H;
			} else {
				return 0;
			}
		}
		const struct a64e_fmt *f = size == 1 ? &fmt_h : size == 2 ? &fmt_s : &fmt_d;
		unsigned eb = 1u << size, pairs = (Q ? 16 : 8) / eb / 2;
		uint64_t sb = 1ULL << (8 * eb - 1);
		const uint8_t *m = q[rm];
		for (unsigned e = 0; e < pairs; e++) {
			unsigned mi = elem ? index : e;
			uint64_t nre = el_get(n, 2 * e, eb), nim = el_get(n, 2 * e + 1, eb);
			uint64_t mre = el_get(m, 2 * mi, eb), mim = el_get(m, 2 * mi + 1, eb);
			uint64_t e1, e2, e3;            /* element4 is element2 */
			switch (rot) {
			case 0: e1 = mre; e2 = nre; e3 = mim; break;
			case 1: e1 = mim ^ sb; e2 = nim; e3 = mre; break;
			case 2: e1 = mre ^ sb; e2 = nre; e3 = mim ^ sb; break;
			default: e1 = mim; e2 = nim; e3 = mre ^ sb; break;
			}
			el_set(r, 2 * e, eb, fp_muladd(f, el_get(d, 2 * e, eb), e2, e1, fpcr, &flags));
			el_set(r, 2 * e + 1, eb, fp_muladd(f, el_get(d, 2 * e + 1, eb), e2, e3, fpcr, &flags));
		}
	} else if ((w & 0xBF20EC00) == 0x2E00E400) {            /* FCADD */
		if (size == 0 || (size == 3 && !Q)) {
			return 0;
		}
		const struct a64e_fmt *f = size == 1 ? &fmt_h : size == 2 ? &fmt_s : &fmt_d;
		unsigned eb = 1u << size, pairs = (Q ? 16 : 8) / eb / 2;
		uint64_t sb = 1ULL << (8 * eb - 1);
		bool rot270 = (w >> 12) & 1;
		const uint8_t *m = q[rm];
		for (unsigned e = 0; e < pairs; e++) {
			uint64_t mre = el_get(m, 2 * e, eb), mim = el_get(m, 2 * e + 1, eb);
			/* #90: (n.re - m.im, n.im + m.re), #270: (n.re + m.im, n.im - m.re), as FPAdd of FPNeg */
			uint64_t e1 = rot270 ? mim : mim ^ sb, e3 = rot270 ? mre ^ sb : mre;
			el_set(r, 2 * e, eb, fp_add(f, el_get(n, 2 * e, eb), e1, fpcr, &flags));
			el_set(r, 2 * e + 1, eb, fp_add(f, el_get(n, 2 * e + 1, eb), e3, fpcr, &flags));
		}
	} else if ((w & 0xFFE0F400) == 0x4E80A400 ||            /* SMMLA, USMMLA */
	    (w & 0xFFE0FC00) == 0x6E80A400) {                   /* UMMLA */
		bool un = (w & 0x20000800) != 0, um = (w & 0x20000000) != 0;
		const uint8_t *m = q[rm];
		for (unsigned i = 0; i < 2; i++) {
			for (unsigned j = 0; j < 2; j++) {
				int64_t s = 0;
				for (unsigned k = 0; k < 8; k++) {
					s += i8(n[8 * i + k], un) * i8(m[8 * j + k], um);
				}
				el_set(r, 2 * i + j, 4, (uint32_t)(el_get(d, 2 * i + j, 4) + (uint64_t)s));
			}
		}
	} else if ((w & 0xBFE0FC00) == 0x0E809C00 ||            /* USDOT (vector) */
	    (w & 0xBFC0F400) == 0x0F80F000 ||                   /* USDOT (by element) */
	    (w & 0xBFC0F400) == 0x0F00F000) {                   /* SUDOT (by element) */
		bool elem = (w & 0x01000000) != 0, su = elem && size == 0;
		unsigned index = H << 1 | L;
		const uint8_t *m = q[rm];
		for (unsigned e = 0; e < (Q ? 4u : 2u); e++) {
			int64_t s = 0;
			for (unsigned k = 0; k < 4; k++) {
				uint8_t mb = m[(elem ? index : e) * 4 + k];
				s += i8(n[4 * e + k], !su) * i8(mb, su);
			}
			el_set(r, e, 4, (uint32_t)(el_get(d, e, 4) + (uint64_t)s));
		}
	} else if ((w & 0xBFE0FC00) == 0x2E40FC00 ||            /* BFDOT (vector) */
	    (w & 0xBFC0F400) == 0x0F40F000) {                   /* BFDOT (by element) */
		bool elem = (w & 0x01000000) != 0;
		unsigned index = H << 1 | L;
		const uint8_t *m = q[rm];
		for (unsigned e = 0; e < (Q ? 4u : 2u); e++) {
			unsigned mi = elem ? index : e;
			el_set(r, e, 4, bf_dotadd((uint32_t)el_get(d, e, 4), (uint16_t)el_get(n, 2 * e, 2),
			    (uint16_t)el_get(n, 2 * e + 1, 2), (uint16_t)el_get(m, 2 * mi, 2), (uint16_t)el_get(m, 2 * mi + 1, 2)));
		}
	} else if ((w & 0xFFE0FC00) == 0x6E40EC00) {            /* BFMMLA */
		const uint8_t *m = q[rm];
		for (unsigned i = 0; i < 2; i++) {
			for (unsigned j = 0; j < 2; j++) {
				uint32_t s = (uint32_t)el_get(d, 2 * i + j, 4);
				for (unsigned k = 0; k < 2; k++) {
					s = bf_dotadd(s, (uint16_t)el_get(n, 4 * i + 2 * k, 2), (uint16_t)el_get(n, 4 * i + 2 * k + 1, 2),
					    (uint16_t)el_get(m, 4 * j + 2 * k, 2), (uint16_t)el_get(m, 4 * j + 2 * k + 1, 2));
				}
				el_set(r, 2 * i + j, 4, s);
			}
		}
	} else if ((w & 0xBFE0FC00) == 0x2EC0FC00 ||            /* BFMLALB/T (vector) */
	    (w & 0xBFC0F400) == 0x0FC0F000) {                   /* BFMLALB/T (by element), Vm in V0-V15 */
		bool elem = (w & 0x01000000) != 0;
		unsigned index = H << 2 | L << 1 | M;
		const uint8_t *m = q[elem ? rm & 15 : rm];
		for (unsigned e = 0; e < 4; e++) {
			uint64_t a = el_get(n, 2 * e + Q, 2) << 16, b = el_get(m, elem ? index : 2 * e + Q, 2) << 16;
			el_set(r, e, 4, fp_muladd(&fmt_s, el_get(d, e, 4), a, b, fpcr, &flags));
		}
	} else if ((w & 0xFFFFFC00) == 0x1E634000) {            /* BFCVT Hd, Sn */
		el_set(r, 0, 2, bf_cvt((uint32_t)el_get(n, 0, 4), fpcr, &flags));
	} else if ((w & 0xBFFFFC00) == 0x0EA16800) {            /* BFCVTN{2} Vd.4H|8H, Vn.4S */
		if (Q) {
			memcpy(r, d, 8);                /* the 2 form keeps the lower half */
		}
		for (unsigned e = 0; e < 4; e++) {
			el_set(r, 4 * Q + e, 2, bf_cvt((uint32_t)el_get(n, e, 4), fpcr, &flags));
		}
	} else {
		return 0;
	}
	memcpy(q[rd], r, 16);           /* a 64-bit form clears the upper half */
	*fpsr |= flags;
	return 1;
}

/* ---- the trap */

static uint64_t
a64e_gpr_get(arm_saved_state_t *ss, unsigned r)
{
	arm_saved_state64_t *s = saved_state64(ss);
	return r < 29 ? s->x[r] : r == 29 ? s->fp : r == 30 ? s->lr : 0;    /* 31: XZR */
}

static void
a64e_gpr_set(arm_saved_state_t *ss, unsigned r, uint64_t v)
{
	arm_saved_state64_t *s = saved_state64(ss);
	if (r < 29) {
		s->x[r] = v;
	} else if (r == 29) {
		s->fp = v;
	} else if (r == 30) {
		s->lr = v;
	}
}

enum a64e_emu_result
a64e_isa_emulate(arm_saved_state_t *ss, uint32_t w)
{
	struct a64e_thread_state *ts = A64E_THREAD_STATE();
	/* FEAT_DIT (ARMv8.4; corecrypto sets it around its operations and reads it back): the bit is the
	 * thread's, PSTATE.DIT at bit 24 of MRS/MSR's register. The cores here have no data-dependent
	 * timing mode to switch: keeping the bit is all DIT can mean on them. */
	if ((w & 0xFFFFF0FF) == 0xD503405F) {                   /* MSR DIT, #imm: CRm<0> */
		ts->dit = (w >> 8) & 1;
	} else if ((w & 0xFFFFFFE0) == 0xD51B42A0) {            /* MSR DIT, Xt */
		ts->dit = (uint32_t)(a64e_gpr_get(ss, w & 31) >> 24) & 1;
	} else if ((w & 0xFFFFFFE0) == 0xD53B42A0) {            /* MRS Xt, DIT */
		a64e_gpr_set(ss, w & 31, (uint64_t)ts->dit << 24);
	} else {
		thread_t th = current_thread();
		if (th->machine.uNeon == NULL) {
			return A64E_EMU_NOT_OURS;
		}
		arm_neon_saved_state64_t *ns = &th->machine.uNeon->ns_64;
		uint32_t fpsr = ns->fpsr;
		if (!a64e_isa_simd(w, (uint8_t (*)[16])(void *)ns->v.q, ns->fpcr, &fpsr)) {
			return A64E_EMU_NOT_OURS;
		}
		ns->fpsr = fpsr;
	}
	set_saved_state_pc(ss, get_saved_state_pc(ss) + 4);
	return A64E_EMU_HANDLED;
}

static void
a64e_note_fault(uint64_t uaddr, unsigned long n, uint32_t write)
{
	struct a64e_thread_state *ts = A64E_THREAD_STATE();
	uint64_t far = uaddr;
	uint32_t vm = a64e_copyio_far(uaddr, n, write, &far);
	ts->fault = A64E_TF_FAULT | (write ? A64E_TF_WRITE : 0) | (vm ? A64E_TF_VM : 0);
	ts->fault_addr = far;
}

int
a64e_emu_copyin(uint64_t uaddr, void *kaddr, unsigned long n)
{
	a64e_copyio_begin();
	int e = copyin((user_addr_t)uaddr, kaddr, n);
	if (e != 0) {
		a64e_note_fault(uaddr, n, 0);
	}
	return e;
}

int
a64e_emu_copyout(const void *kaddr, uint64_t uaddr, unsigned long n)
{
	a64e_copyio_begin();
	int e = copyout(kaddr, (user_addr_t)uaddr, n);
	if (e != 0) {
		a64e_note_fault(uaddr, n, 1);
	}
	return e;
}

extern bool sweet_a64e_emulate(arm_saved_state_t *ss, uint32_t w);

enum a64e_emu_result
a64e_emulate_trap(arm_saved_state_t *ss, uint32_t w, struct a64e_emu_fault *fault)
{
	struct a64e_thread_state *ts = A64E_THREAD_STATE();
	ts->fault = 0;
	if (sweet_a64e_emulate(ss, w)) {
		return A64E_EMU_HANDLED;
	}
	if (ts->fault) {
		fault->addr = ts->fault_addr;
		fault->write = (ts->fault & A64E_TF_WRITE) != 0;
		fault->vm = (ts->fault & A64E_TF_VM) != 0;
		ts->fault = 0;
		return A64E_EMU_FAULT;
	}
	return A64E_EMU_NOT_OURS;
}

#endif /* A64E_ISA_HARNESS || SWEET || A5XELTE || VAYU */
