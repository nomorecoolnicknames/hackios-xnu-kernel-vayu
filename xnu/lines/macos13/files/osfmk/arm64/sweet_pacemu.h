/*
 * Trap-and-emulate for ARMv8.3-PAuth (non-HINT pointer-authentication instructions)
 * and ARMv8.3-JSCVT (FJCVTZS) on ARMv8.2 CPUs (SM8150 Kryo 485 / Cortex-A76).
 *
 * Adapted from Renegade/Bergamota 1019-sweet-pac-emulation.patch (github.com/ry-diffusion/Bergamota, a fork
 * of dipa-studio/ios-sweet, MIT licence), as the lab's macOS 13.4 kernel had it (work/macos13-vayu).
 * PACGA here is Xn ^ ROR(Xm, 13), the same as Bergamota's offline pacstrip.py: the two must agree.
 * Used by osfmk/arm64/sleh.c with boot-arg macos_abi=1 only.
 */
#ifndef _ARM64_SWEET_PACEMU_H_
#define _ARM64_SWEET_PACEMU_H_

#include <stdint.h>
#include <stdbool.h>

enum sweet_pac_op {
	SWEET_PAC_NONE = 0,     /* not an instruction handled here */
	SWEET_PAC_RET,          /* PC = X30 */
	SWEET_PAC_BR,           /* PC = Xn */
	SWEET_PAC_BLR,          /* X30 = PC + 4, PC = Xn */
	SWEET_PAC_LDR,          /* Xt = [Xn|SP + imm], optional writeback */
	SWEET_PAC_NOP,          /* PAC*, AUT*, XPAC* */
	SWEET_PAC_PACGA,        /* Xd = Xn ^ ROR(Xm, 13); Xm = SP: Xd = Xn */
};

enum sweet_pac_class {
	SWEET_PACC_RETA = 0,    /* RETAA, RETAB */
	SWEET_PACC_BRA,         /* BRAA, BRAB, BRAAZ, BRABZ */
	SWEET_PACC_BLRA,        /* BLRAA, BLRAB, BLRAAZ, BLRABZ */
	SWEET_PACC_LDRA,        /* LDRAA, LDRAB */
	SWEET_PACC_PAC,         /* PACIA ... PACDZB */
	SWEET_PACC_AUT,         /* AUTIA ... AUTDZB */
	SWEET_PACC_XPAC,        /* XPACI, XPACD */
	SWEET_PACC_PACGA,
	SWEET_PACC_COUNT
};

struct sweet_pac_insn {
	uint8_t op;
	uint8_t cls;
	uint8_t rd;
	uint8_t rn;
	uint8_t rm;
	bool    writeback;
	bool    key_b;
	int32_t imm;
};

static inline bool
sweet_pac_decode(uint32_t w, struct sweet_pac_insn *out)
{
	const uint8_t rd = (uint8_t)(w & 31), rn = (uint8_t)((w >> 5) & 31), rm = (uint8_t)(w & 31);

	*out = (struct sweet_pac_insn){ .op = SWEET_PAC_NONE };

	/* RETAA / RETAB: 1101011 0 0 10 11111 00001 M 11111 11111 */
	if ((w & 0xFFFFFBFFu) == 0xD65F0BFFu) {
		*out = (struct sweet_pac_insn){ .op = SWEET_PAC_RET, .cls = SWEET_PACC_RETA, .rn = 30,
			                        .key_b = (w >> 10) & 1 };
		return true;
	}

	/*
	 * BRAA/BRAB, BLRAA/BLRAB:     1101011 1 0 0 opc 11111 00001 M Rn Rm   (opc 00 = BR, 01 = BLR)
	 * BRAAZ/BRABZ, BLRAAZ/BLRABZ: 1101011 0 0 0 opc 11111 00001 M Rn 11111
	 */
	if ((w & 0xFEDFF800u) == 0xD61F0800u) {
		const bool z = ((w >> 24) & 1) == 0;
		if (z && rm != 31) {
			return false;
		}
		const bool link = (w >> 21) & 1;
		*out = (struct sweet_pac_insn){ .op = link ? SWEET_PAC_BLR : SWEET_PAC_BR,
			                        .cls = link ? SWEET_PACC_BLRA : SWEET_PACC_BRA,
			                        .rn = rn, .rm = rm, .key_b = (w >> 10) & 1 };
		return true;
	}

	/* PACxx/AUTxx Xd, Xn|SP: 1 1 0 11010110 00001 0 Z opc(3) Rn Rd; Z forms need Rn = 11111 */
	if ((w & 0xFFFFC000u) == 0xDAC10000u) {
		const bool z = (w >> 13) & 1;
		if (z && rn != 31) {
			return false;
		}
		const bool aut = (w >> 12) & 1;
		*out = (struct sweet_pac_insn){ .op = SWEET_PAC_NOP, .cls = aut ? SWEET_PACC_AUT : SWEET_PACC_PAC,
			                        .rd = rd, .rn = rn, .key_b = (w >> 10) & 1 };
		return true;
	}

	/* XPACI / XPACD Xd: 1 1 0 11010110 00001 0 1000 D 11111 Rd */
	if ((w & 0xFFFFFBE0u) == 0xDAC143E0u) {
		*out = (struct sweet_pac_insn){ .op = SWEET_PAC_NOP, .cls = SWEET_PACC_XPAC, .rd = rd };
		return true;
	}

	/* PACGA Xd, Xn, Xm|SP: 1 0 0 11010110 Rm 001100 Rn Rd */
	if ((w & 0xFFE0FC00u) == 0x9AC03000u) {
		*out = (struct sweet_pac_insn){ .op = SWEET_PAC_PACGA, .cls = SWEET_PACC_PACGA,
			                        .rd = rd, .rn = rn, .rm = (uint8_t)((w >> 16) & 31) };
		return true;
	}

	/* LDRAA/LDRAB Xt, [Xn|SP, #simm10 * 8]{!}: 11 111 0 00 M S 1 imm9 W 1 Rn Rt */
	if ((w & 0xFF200400u) == 0xF8200400u) {
		const uint32_t s10 = ((w >> 22) & 1) << 9 | ((w >> 12) & 0x1FF);
		const int32_t off = ((int32_t)(s10 << 22) >> 22) * 8;
		*out = (struct sweet_pac_insn){ .op = SWEET_PAC_LDR, .cls = SWEET_PACC_LDRA, .rd = rd, .rn = rn,
			                        .writeback = (w >> 11) & 1, .key_b = (w >> 23) & 1, .imm = off };
		return true;
	}

	return false;
}

static inline uint64_t
sweet_pacga(uint64_t xn, uint64_t xm, unsigned rm)
{
	if (rm == 31) {
		return xn;
	}
	return xn ^ ((xm >> 13) | (xm << (64 - 13)));
}

struct sweet_pac_cpu {
	uint64_t x[31];
	uint64_t sp;
	uint64_t pc;
	uint32_t written;
};

enum sweet_pac_result {
	SWEET_PAC_DONE = 0,
	SWEET_PAC_FAULT,
	SWEET_PAC_UNHANDLED,
};

typedef int (*sweet_pac_load64_t)(void *ctx, uint64_t addr, uint64_t *value);

static inline uint64_t
sweet_pac_xzr(const struct sweet_pac_cpu *c, unsigned r)
{
	return r == 31 ? 0 : c->x[r];
}

static inline void
sweet_pac_set_xzr(struct sweet_pac_cpu *c, unsigned r, uint64_t v)
{
	if (r != 31) {
		c->x[r] = v;
		c->written |= 1u << r;
	}
}

static inline enum sweet_pac_result
sweet_pac_execute(uint32_t instr, struct sweet_pac_cpu *cpu, sweet_pac_load64_t load, void *ctx,
    uint64_t *fault_addr)
{
	struct sweet_pac_insn in;

	if (!sweet_pac_decode(instr, &in)) {
		return SWEET_PAC_UNHANDLED;
	}

	cpu->written = 0;
	switch (in.op) {
	case SWEET_PAC_RET:
		cpu->pc = cpu->x[30];
		break;
	case SWEET_PAC_BR:
		cpu->pc = sweet_pac_xzr(cpu, in.rn);
		break;
	case SWEET_PAC_BLR: {
		const uint64_t target = sweet_pac_xzr(cpu, in.rn);
		sweet_pac_set_xzr(cpu, 30, cpu->pc + 4);
		cpu->pc = target;
		break;
	}
	case SWEET_PAC_NOP:
		cpu->pc += 4;
		break;
	case SWEET_PAC_PACGA:
		sweet_pac_set_xzr(cpu, in.rd,
		    sweet_pacga(sweet_pac_xzr(cpu, in.rn), in.rm == 31 ? cpu->sp : cpu->x[in.rm], in.rm));
		cpu->pc += 4;
		break;
	case SWEET_PAC_LDR: {
		const uint64_t base = in.rn == 31 ? cpu->sp : cpu->x[in.rn];
		const uint64_t addr = base + (uint64_t)(int64_t)in.imm;
		uint64_t v = 0;
		if (load(ctx, addr, &v) != 0) {
			*fault_addr = addr;
			return SWEET_PAC_FAULT;
		}
		if (in.writeback) {
			if (in.rn == 31) {
				cpu->sp = addr;
				cpu->written |= 1u << 31;
			} else {
				sweet_pac_set_xzr(cpu, in.rn, addr);
			}
		}
		sweet_pac_set_xzr(cpu, in.rd, v);
		cpu->pc += 4;
		break;
	}
	default:
		return SWEET_PAC_UNHANDLED;
	}
	return SWEET_PAC_DONE;
}

static inline const char *
sweet_pac_class_name(unsigned cls)
{
	static const char *const names[SWEET_PACC_COUNT] = {
		"RETA*", "BRA*", "BLRA*", "LDRA*", "PAC*", "AUT*", "XPAC*", "PACGA",
	};
	return cls < SWEET_PACC_COUNT ? names[cls] : "?";
}

#endif /* _ARM64_SWEET_PACEMU_H_ */
