/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Thumb-2's 32-bit instructions, an addition to mGBA's core; see README.md.
 *
 * A first halfword of 0xE800 or above introduces a 32-bit instruction, and
 * every such table entry lands in _ThumbInstructionTHUMB32 below. By then
 * ThumbStep has consumed the first halfword, so the second is prefetch[0]
 * and r15 reads as the instruction's address plus 4, which is exactly what
 * Thumb code expects of the PC. Only once the instruction is done, and if it
 * did not branch, does the pipeline move on past the second halfword.
 *
 * Where a Thumb-2 instruction does just what an ARM one does, and only the
 * fields are laid out differently -- the media instructions, and the
 * multiplies and divides -- it is rewritten into the ARM encoding and run
 * through the ARM table, so that there is one implementation of each. The
 * rest are implemented here. */

#include <mgba/internal/arm/isa-thumb.h>

#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-inlines.h>

#define HW1 (opcode >> 16)
#define HW2 (opcode & 0xFFFF)

/* Run an ARM encoding, built with an AL condition, through the ARM table. */
static void _thumb2RunARM(struct ARMCore* cpu, uint32_t arm) {
	_armTable[((arm >> 16) & 0xFF0) | ((arm >> 4) & 0x00F)](cpu, arm);
}

/* Write a register that may be the PC. The architecture makes nearly every
 * such write in Thumb-2 UNPREDICTABLE; this core takes it as a plain branch
 * that stays in Thumb state (ARMWritePCUnpredictable). */
static void _thumb2WriteReg(struct ARMCore* cpu, int rd, uint32_t value) {
	if (rd == ARM_PC) {
		ARMWritePCUnpredictable(cpu, value);
	} else {
		cpu->gprs[rd] = (int32_t) value;
	}
}

static inline void _thumb2SetNZ(struct ARMCore* cpu, uint32_t value) {
	cpu->cpsr.n = value >> 31;
	cpu->cpsr.z = !value;
}

/* The architecture's AddWithCarry; subtraction is x + ~y + 1. */
static uint32_t _thumb2AddWithCarry(struct ARMCore* cpu, uint32_t x, uint32_t y,
                                    unsigned carryIn, bool setFlags) {
	uint64_t sum = (uint64_t) x + y + carryIn;
	uint32_t result = (uint32_t) sum;
	if (setFlags) {
		_thumb2SetNZ(cpu, result);
		cpu->cpsr.c = (unsigned) (sum >> 32);
		cpu->cpsr.v = ((x ^ result) & (y ^ result)) >> 31;
	}
	return result;
}

enum Thumb2Shift {
	THUMB2_LSL,
	THUMB2_LSR,
	THUMB2_ASR,
	THUMB2_ROR,
	THUMB2_RRX
};

/* The architecture's Shift_C, for any amount, so that it serves both the
 * immediate forms (after _thumb2DecodeImmShift) and the register-controlled
 * shifts, which use the bottom byte of a register. A rotate by zero is not
 * RRX here; that is its own type. */
static uint32_t _thumb2Shift(uint32_t value, enum Thumb2Shift type, unsigned amount,
                             unsigned carryIn, unsigned* carryOut) {
	*carryOut = carryIn;
	if (type == THUMB2_RRX) {
		*carryOut = value & 1;
		return (carryIn << 31) | (value >> 1);
	}
	if (!amount) {
		return value;
	}
	switch (type) {
	case THUMB2_LSL:
		if (amount < 32) {
			*carryOut = (value >> (32 - amount)) & 1;
			return value << amount;
		}
		*carryOut = amount == 32 ? value & 1 : 0;
		return 0;
	case THUMB2_LSR:
		if (amount < 32) {
			*carryOut = (value >> (amount - 1)) & 1;
			return value >> amount;
		}
		*carryOut = amount == 32 ? value >> 31 : 0;
		return 0;
	case THUMB2_ASR:
		if (amount < 32) {
			*carryOut = (value >> (amount - 1)) & 1;
			return (uint32_t) ((int32_t) value >> amount);
		}
		*carryOut = value >> 31;
		return (uint32_t) ((int32_t) value >> 31);
	default:
		amount &= 31;
		if (amount) {
			value = ROR(value, amount);
		}
		*carryOut = value >> 31;
		return value;
	}
}

/* DecodeImmShift: an immediate of zero means 32 for LSR and ASR, and RRX
 * for ROR. */
static enum Thumb2Shift _thumb2DecodeImmShift(unsigned type, unsigned imm5, unsigned* amount) {
	*amount = imm5;
	switch (type) {
	case 0:
		return THUMB2_LSL;
	case 1:
		*amount = imm5 ? imm5 : 32;
		return THUMB2_LSR;
	case 2:
		*amount = imm5 ? imm5 : 32;
		return THUMB2_ASR;
	default:
		if (!imm5) {
			*amount = 1;
			return THUMB2_RRX;
		}
		return THUMB2_ROR;
	}
}

/* ThumbExpandImm_C: the modified immediate constants, which replicate a
 * byte across the word or rotate an 8-bit value with its top bit set. */
static uint32_t _thumb2ExpandImm(unsigned imm12, unsigned carryIn, unsigned* carryOut) {
	uint32_t imm8 = imm12 & 0xFF;
	*carryOut = carryIn;
	if (!(imm12 & 0xC00)) {
		switch ((imm12 >> 8) & 3) {
		case 0:
			return imm8;
		case 1:
			return imm8 * 0x00010001;
		case 2:
			return imm8 * 0x01000100;
		default:
			return imm8 * 0x01010101;
		}
	}
	uint32_t value = ROR(0x80 | (imm12 & 0x7F), (imm12 >> 7) & 0x1F);
	*carryOut = value >> 31;
	return value;
}

/* The i:imm3:imm8 field shared by the immediate data-processing forms. */
static inline unsigned _thumb2Imm12(uint32_t opcode) {
	return ((HW1 & 0x0400) << 1) | ((HW2 >> 4) & 0x0700) | (HW2 & 0x00FF);
}

/* The data-processing operations shared by the modified immediate and
 * shifted register forms, which both put the operation at bits 8:5 of the
 * first halfword. Rd == 15 with S set turns AND, EOR, ADD and SUB into TST,
 * TEQ, CMN and CMP; Rn == 15 turns ORR and ORN into MOV and MVN. */
static bool _thumb2DataProcessing(struct ARMCore* cpu, unsigned op, bool s, int rd, int rn,
                                  uint32_t operand, unsigned carry) {
	/* The tests only exist with S set. Any other write to the PC is
	 * UNPREDICTABLE, and a branch (see _thumb2WriteReg). */
	bool test = rd == ARM_PC && s && (op == 0x0 || op == 0x4 || op == 0x8 || op == 0xD);
	/* MOVS PC is an exception return, undefined in user mode. */
	if (rd == ARM_PC && s && op == 0x2 && rn == ARM_PC) {
		return false;
	}
	uint32_t n = cpu->gprs[rn];
	uint32_t result;
	bool logical = true;
	switch (op) {
	case 0x0:
		result = n & operand;
		break;
	case 0x1:
		result = n & ~operand;
		break;
	case 0x2:
		result = rn == ARM_PC ? operand : n | operand;
		break;
	case 0x3:
		result = rn == ARM_PC ? ~operand : n | ~operand;
		break;
	case 0x4:
		result = n ^ operand;
		break;
	case 0x8:
		result = _thumb2AddWithCarry(cpu, n, operand, 0, s);
		logical = false;
		break;
	case 0xA:
		result = _thumb2AddWithCarry(cpu, n, operand, cpu->cpsr.c, s);
		logical = false;
		break;
	case 0xB:
		result = _thumb2AddWithCarry(cpu, n, ~operand, cpu->cpsr.c, s);
		logical = false;
		break;
	case 0xD:
		result = _thumb2AddWithCarry(cpu, n, ~operand, 1, s);
		logical = false;
		break;
	case 0xE:
		result = _thumb2AddWithCarry(cpu, ~n, operand, 1, s);
		logical = false;
		break;
	default:
		return false;
	}
	if (s && logical) {
		_thumb2SetNZ(cpu, result);
		cpu->cpsr.c = carry;
	}
	if (!test) {
		_thumb2WriteReg(cpu, rd, result);
	}
	return true;
}

/* 1110 101x xxxx xxxx: data-processing, shifted register. */
static bool _thumb2DataProcessingShifted(struct ARMCore* cpu, uint32_t opcode) {
	unsigned op = (HW1 >> 5) & 0xF;
	int rn = HW1 & 0xF;
	int rd = (HW2 >> 8) & 0xF;
	int rm = HW2 & 0xF;
	unsigned imm5 = ((HW2 >> 10) & 0x1C) | ((HW2 >> 6) & 0x3);
	unsigned type = (HW2 >> 4) & 0x3;
	if (HW2 & 0x8000) {
		return false;
	}
	if (op == 0x6) {
		/* PKHBT and PKHTB. Bit 5 of the second halfword is the ARM
		 * encoding's tb bit, and there is no S form. */
		if ((HW1 & 0x10) || (type & 1)) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE6800010 | (rn << 16) | (rd << 12) | (imm5 << 7) |
			((type >> 1) << 6) | rm);
		return true;
	}
	unsigned amount;
	enum Thumb2Shift shift = _thumb2DecodeImmShift(type, imm5, &amount);
	unsigned carry;
	uint32_t operand = _thumb2Shift(cpu->gprs[rm], shift, amount, cpu->cpsr.c, &carry);
	return _thumb2DataProcessing(cpu, op, HW1 & 0x10, rd, rn, operand, carry);
}

/* 1111 0x0x xxxx xxxx 0xxx: data-processing, modified immediate. */
static bool _thumb2DataProcessingModifiedImmediate(struct ARMCore* cpu, uint32_t opcode) {
	unsigned carry;
	uint32_t operand = _thumb2ExpandImm(_thumb2Imm12(opcode), cpu->cpsr.c, &carry);
	return _thumb2DataProcessing(cpu, (HW1 >> 5) & 0xF, HW1 & 0x10, (HW2 >> 8) & 0xF,
		HW1 & 0xF, operand, carry);
}

/* 1111 0x1x xxxx xxxx 0xxx: data-processing, plain binary immediate. */
static bool _thumb2DataProcessingPlainImmediate(struct ARMCore* cpu, uint32_t opcode) {
	int rn = HW1 & 0xF;
	int rd = (HW2 >> 8) & 0xF;
	unsigned imm12 = _thumb2Imm12(opcode);
	/* The shift or lsb field of the saturate and bit field instructions. */
	unsigned imm5 = ((HW2 >> 10) & 0x1C) | ((HW2 >> 6) & 0x3);
	unsigned low5 = HW2 & 0x1F;
	if (rd == ARM_PC && rn == ARM_PC && ((HW1 >> 4) & 0x1F) == 0x00) {
		/* ADR to the PC: UNPREDICTABLE, and taken as an interworking branch. */
		cpu->gprs[ARM_PC] = (int32_t) (((uint32_t) cpu->gprs[ARM_PC] & ~3u) + imm12);
		cpu->cycles += ARMInterworkWritePC(cpu);
		return true;
	}
	if (rd == ARM_PC && rn == ARM_PC && ((HW1 >> 4) & 0x1F) == 0x0A) {
		cpu->gprs[ARM_PC] = (int32_t) (((uint32_t) cpu->gprs[ARM_PC] & ~3u) - imm12);
		cpu->cycles += ARMInterworkWritePC(cpu);
		return true;
	}
	switch ((HW1 >> 4) & 0x1F) {
	case 0x00:
		/* ADDW, or ADR when Rn is the PC, which is word-aligned. */
		_thumb2WriteReg(cpu, rd, (rn == ARM_PC ? (uint32_t) cpu->gprs[ARM_PC] & ~3u : (uint32_t) cpu->gprs[rn]) +
			imm12);
		return true;
	case 0x0A:
		/* SUBW, or ADR's subtracting form. */
		_thumb2WriteReg(cpu, rd, (rn == ARM_PC ? (uint32_t) cpu->gprs[ARM_PC] & ~3u : (uint32_t) cpu->gprs[rn]) -
			imm12);
		return true;
	case 0x04:
		_thumb2WriteReg(cpu, rd, ((HW1 & 0xF) << 12) | imm12);
		return true;
	case 0x0C:
		_thumb2WriteReg(cpu, rd, ((uint32_t) cpu->gprs[rd] & 0xFFFF) |
			((((HW1 & 0xF) << 12) | imm12) << 16));
		return true;
	case 0x10:
	case 0x12:
	case 0x18:
	case 0x1A: {
		if ((HW1 & 0x0400) || (HW2 & 0x0020)) {
			return false;
		}
		/* SSAT and USAT, bit 7 of the first halfword picking USAT and bit 5
		 * (sh) ASR. ASR #0 is not a shift here but SSAT16 or USAT16. */
		uint32_t unsignedBit = (HW1 & 0x80) ? 0x00400000 : 0;
		if ((HW1 & 0x20) && !imm5) {
			if (HW2 & 0x0030) {
				return false;
			}
			_thumb2RunARM(cpu, (0xE6A00F30 | unsignedBit) | ((HW2 & 0xF) << 16) | (rd << 12) | rn);
		} else {
			_thumb2RunARM(cpu, (0xE6A00010 | unsignedBit) | (low5 << 16) | (rd << 12) | (imm5 << 7) |
				(((HW1 >> 5) & 1) << 6) | rn);
		}
		return true;
	}
	case 0x14:
		if ((HW1 & 0x0400) || (HW2 & 0x0020)) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE7A00050 | (low5 << 16) | (rd << 12) | (imm5 << 7) | rn);
		return true;
	case 0x16:
		/* BFI, or BFC when Rn == 15, which the ARM form also means. */
		if ((HW1 & 0x0400) || (HW2 & 0x0020)) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE7C00010 | (low5 << 16) | (rd << 12) | (imm5 << 7) | rn);
		return true;
	case 0x1C:
		if ((HW1 & 0x0400) || (HW2 & 0x0020)) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE7E00050 | (low5 << 16) | (rd << 12) | (imm5 << 7) | rn);
		return true;
	default:
		return false;
	}
}

/* Branch to a Thumb address. The architecture's BranchWritePC. */
static void _thumb2Branch(struct ARMCore* cpu, uint32_t target) {
	cpu->gprs[ARM_PC] = target;
	cpu->cycles += ThumbWritePC(cpu);
}

/* 1111 0xxx xxxx xxxx 1xxx: branches and miscellaneous control. */
static bool _thumb2BranchMisc(struct ARMCore* cpu, uint32_t opcode) {
	uint32_t pc = cpu->gprs[ARM_PC];
	unsigned op1 = (HW2 >> 12) & 0x7;
	unsigned s = (HW1 >> 10) & 1;
	unsigned j1 = (HW2 >> 13) & 1;
	unsigned j2 = (HW2 >> 11) & 1;
	/* For B.W, BL and BLX, the J bits become I1 and I2, which extend the
	 * range; for Thumb-1's BL pair they are both set, and I1 and I2 are
	 * then just more copies of the sign. */
	uint32_t i1 = !(j1 ^ s);
	uint32_t i2 = !(j2 ^ s);
	uint32_t sign = s ? 0xFF000000 : 0;

	if (op1 & 1) {
		/* B.W (T4), or BL. */
		uint32_t offset = sign | (i1 << 23) | (i2 << 22) | ((HW1 & 0x3FF) << 12) |
			((HW2 & 0x7FF) << 1);
		if (op1 & 4) {
			cpu->gprs[ARM_LR] = pc | 1;
		}
		_thumb2Branch(cpu, pc + offset);
		return true;
	}
	if (op1 & 4) {
		/* BLX (immediate), to ARM state. The target is word-aligned. */
		if (HW2 & 1) {
			return false;
		}
		uint32_t offset = sign | (i1 << 23) | (i2 << 22) | ((HW1 & 0x3FF) << 12) |
			((HW2 & 0x7FE) << 1);
		cpu->gprs[ARM_LR] = pc | 1;
		_ARMSetMode(cpu, MODE_ARM);
		cpu->gprs[ARM_PC] = ((pc & ~3) + offset) & ~3;
		cpu->cycles += ARMWritePC(cpu);
		return true;
	}
	if ((HW1 & 0x0380) != 0x0380) {
		/* B<c>.W (T3). Not permitted in an IT block. */
		uint32_t offset = (s ? 0xFFF00000 : 0) | (j2 << 19) | (j1 << 18) |
			((HW1 & 0x3F) << 12) | ((HW2 & 0x7FF) << 1);
		if (ARMTestCondition(cpu, (HW1 >> 6) & 0xF)) {
			_thumb2Branch(cpu, pc + offset);
		}
		return true;
	}

	switch ((HW1 >> 4) & 0x7F) {
	case 0x38:
		/* MSR, to the APSR. Bit 11 writes NZCVQ and bit 10 the GE flags;
		 * the SPSR form (bit 4 of the first halfword) needs privilege. */
		if ((HW2 & 0x0300) || (HW2 & 0x20FF)) {
			return false;
		} else {
			uint32_t value = cpu->gprs[HW1 & 0xF];
			uint32_t mask = ((HW2 & 0x0800) ? 0xF8000000 : 0) | ((HW2 & 0x0400) ? 0x000F0000 : 0);
			cpu->cpsr.packed = (int32_t) (((uint32_t) cpu->cpsr.packed & ~mask) | (value & mask));
			return true;
		}
	case 0x3A:
		/* The wide hints (NOP.W, YIELD.W, WFE.W, WFI.W, SEV.W, DBG) when
		 * bits 10:8 are clear, and otherwise CPS, which changes nothing
		 * touchHLE models. The should-be-one and should-be-zero bits are
		 * checked. */
		return (HW1 & 0xF) == 0xF && !(HW2 & 0x2800);
	case 0x3B:
		if ((HW1 & 0xF) != 0xF || (HW2 & 0x2F00) != 0x0F00) {
			return false;
		}
		switch ((HW2 >> 4) & 0xF) {
		case 0x2:
			/* CLREX. */
			if ((HW2 & 0xF) != 0xF) {
				return false;
			}
			cpu->exclusiveMonitor = false;
			return true;
		case 0x4:
		case 0x5:
			/* DSB and DMB: one in-order core with no cache. */
			return true;
		case 0x6:
			/* ISB. Branching to the next instruction refetches it, so
			 * code the guest has just written is what runs. */
			_thumb2Branch(cpu, pc);
			return true;
		default:
			return false;
		}
	case 0x3C:
		/* BXJ, which is BX with no Jazelle implementation. */
		if (HW2 != 0x8F00) {
			return false;
		}
		cpu->gprs[ARM_PC] = cpu->gprs[HW1 & 0xF];
		cpu->cycles += ARMInterworkWritePC(cpu);
		return true;
	case 0x3E:
		/* MRS from the APSR, as the ARM form does. The IT state reads as
		 * zero. The SPSR form (0x3F) needs privilege. */
		if (((HW2 >> 8) & 0xF) == ARM_PC || (HW1 & 0xF) != 0xF || (HW2 & 0x20FF)) {
			return false;
		}
		cpu->gprs[(HW2 >> 8) & 0xF] = (int32_t) ((uint32_t) cpu->cpsr.packed & ~ARM_IT_MASK);
		return true;
	default:
		/* SUBS PC, LR (an exception return), SMC and UDF.W, and the
		 * privileged forms above. */
		return false;
	}
}

/* 1110 100x x0xx xxxx: LDM and STM, including PUSH.W and POP.W. The other
 * two addressing modes are SRS and RFE, which need privilege. */
static bool _thumb2LoadStoreMultiple(struct ARMCore* cpu, uint32_t opcode) {
	unsigned op = (HW1 >> 7) & 0x3;
	int rn = HW1 & 0xF;
	int list = HW2;
	int cycles = 0;
	if (op == 0 || op == 3 || rn == ARM_PC) {
		return false;
	}
	/* Fewer than two registers, or the base in the list with writeback, is
	 * UNPREDICTABLE, and undefined here. */
	if (!(list & (list - 1)) || ((HW1 & 0x20) && (list & (1 << rn)))) {
		return false;
	}
	enum LSMDirection direction = op == 1 ? LSM_IA : LSM_DB;
	uint32_t address = cpu->gprs[rn];
	/* Lists with SP, or storing the PC, or loading both LR and the PC, are
	 * unpredictable; they are carried out as written. */
	if (!(HW1 & 0x10)) {
		address = cpu->memory.storeMultiple(cpu, address, list, direction, &cycles);
		if (HW1 & 0x20) {
			cpu->gprs[rn] = address;
		}
	} else {
		address = cpu->memory.loadMultiple(cpu, address, list, direction, &cycles);
		if ((HW1 & 0x20) && !(list & (1 << rn))) {
			cpu->gprs[rn] = address;
		}
		if (list & 0x8000) {
			/* Loads to the PC interwork. */
			cycles += ARMInterworkWritePC(cpu);
		}
	}
	cpu->cycles += cycles;
	return true;
}

/* 1110 100x x1xx xxxx: LDRD and STRD, the exclusives, and TBB and TBH. */
static bool _thumb2LoadStoreDual(struct ARMCore* cpu, uint32_t opcode) {
	bool p = HW1 & 0x100;
	bool u = HW1 & 0x080;
	bool w = HW1 & 0x020;
	bool load = HW1 & 0x010;
	int rn = HW1 & 0xF;
	int rt = HW2 >> 12;
	int rt2 = (HW2 >> 8) & 0xF;
	int cycles = 0;
	uint32_t pc = cpu->gprs[ARM_PC];

	if (p || w) {
		/* LDRD and STRD, offset by imm8 * 4; Rn == 15 is the literal form,
		 * word-aligned. The PC anywhere else, and writeback to or a store
		 * based on it, are UNPREDICTABLE; they are carried out as written,
		 * with each write to the PC a branch and the last one winning, and
		 * the writeback coming last when Rn is loaded. */
		uint32_t base = rn == ARM_PC ? pc & ~3 : (uint32_t) cpu->gprs[rn];
		uint32_t imm = (HW2 & 0xFF) << 2;
		uint32_t offsetAddress = u ? base + imm : base - imm;
		uint32_t address = p ? offsetAddress : base;
		if (load) {
			uint32_t lo = cpu->memory.load32(cpu, address, &cycles);
			uint32_t hi = cpu->memory.load32(cpu, address + 4, &cycles);
			_thumb2WriteReg(cpu, rt, lo);
			_thumb2WriteReg(cpu, rt2, hi);
		} else {
			uint32_t lo = cpu->gprs[rt];
			uint32_t hi = cpu->gprs[rt2];
			cpu->memory.store32(cpu, address, lo, &cycles);
			cpu->memory.store32(cpu, address + 4, hi, &cycles);
		}
		if (w) {
			_thumb2WriteReg(cpu, rn, offsetAddress);
		}
		cpu->cycles += cycles;
		return true;
	}

	if (!u) {
		/* LDREX and STREX, the word forms, with an offset of imm8 * 4. SP
		 * or the PC as any register but the base, the PC as the base, or a
		 * status register that is also Rt or Rn, is UNPREDICTABLE, and
		 * undefined. */
		uint32_t address = cpu->gprs[rn] + ((HW2 & 0xFF) << 2);
		if (load && rt2 != 0xF) {
			return false;
		}
		if (rt == ARM_SP || rt == ARM_PC || rn == ARM_PC ||
		    (!load && (rt2 == ARM_SP || rt2 == ARM_PC || rt2 == rn || rt2 == rt))) {
			return false;
		}
		if (load) {
			cpu->exclusiveAddress = address;
			cpu->exclusiveMonitor = true;
			cpu->gprs[rt] = cpu->memory.load32(cpu, address, &cycles);
		} else {
			int rd = (HW2 >> 8) & 0xF;
			if (cpu->exclusiveMonitor && cpu->exclusiveAddress == address) {
				cpu->memory.store32(cpu, address, cpu->gprs[rt], &cycles);
				cpu->gprs[rd] = 0;
			} else {
				cpu->gprs[rd] = 1;
			}
			cpu->exclusiveMonitor = false;
		}
		cpu->cycles += cycles;
		return true;
	}

	unsigned op3 = (HW2 >> 4) & 0xF;
	uint32_t address = cpu->gprs[rn];
	/* The fields these forms do not use must be all ones or all zeros:
	 * TBB and TBH have 1111 0000 above the op; the byte and halfword
	 * exclusives have 1111 in place of Rt2; and the loads among them have
	 * 1111 in place of Rd. */
	if (op3 < 0x2) {
		if (!load || rt != 0xF || rt2 != 0x0) {
			return false;
		}
	} else if (op3 != 0x7 && rt2 != 0xF) {
		return false;
	}
	if (load && op3 >= 0x4 && (HW2 & 0xF) != 0xF) {
		return false;
	}
	/* The byte, halfword and doubleword exclusives' register rules, as for
	 * the word forms above; the doubleword pair must also be distinct. */
	if (op3 >= 0x4) {
		bool dual = op3 == 0x7;
		int rd = HW2 & 0xF;
		if (rt == ARM_SP || rt == ARM_PC || rn == ARM_PC ||
		    (dual && (rt2 == ARM_SP || rt2 == ARM_PC))) {
			return false;
		}
		if (load && dual && rt == rt2) {
			return false;
		}
		if (!load && (rd == ARM_SP || rd == ARM_PC || rd == rn || rd == rt || (dual && rd == rt2))) {
			return false;
		}
	}
	if (load) {
		switch (op3) {
		case 0x0:
		case 0x1: {
			/* TBB and TBH. The table holds halfword counts forward from the
			 * PC, which reads as the address after this instruction; with
			 * Rn == 15 the table follows it directly. */
			uint32_t base = rn == ARM_PC ? pc : address;
			uint32_t index = cpu->gprs[HW2 & 0xF];
			uint32_t count = op3 ? cpu->memory.load16(cpu, base + (index << 1), &cycles)
			                     : cpu->memory.load8(cpu, base + index, &cycles);
			cpu->cycles += cycles;
			_thumb2Branch(cpu, pc + (count << 1));
			return true;
		}
		case 0x4:
			cpu->exclusiveAddress = address;
			cpu->exclusiveMonitor = true;
			cpu->gprs[rt] = cpu->memory.load8(cpu, address, &cycles);
			break;
		case 0x5:
			cpu->exclusiveAddress = address;
			cpu->exclusiveMonitor = true;
			cpu->gprs[rt] = cpu->memory.load16(cpu, address, &cycles);
			break;
		case 0x7:
			cpu->exclusiveAddress = address;
			cpu->exclusiveMonitor = true;
			cpu->gprs[rt] = cpu->memory.load32(cpu, address, &cycles);
			cpu->gprs[rt2] = cpu->memory.load32(cpu, address + 4, &cycles);
			break;
		default:
			return false;
		}
	} else {
		int rd = HW2 & 0xF;
		if (op3 != 0x4 && op3 != 0x5 && op3 != 0x7) {
			return false;
		}
		if (cpu->exclusiveMonitor && cpu->exclusiveAddress == address) {
			switch (op3) {
			case 0x4:
				cpu->memory.store8(cpu, address, cpu->gprs[rt], &cycles);
				break;
			case 0x5:
				cpu->memory.store16(cpu, address, cpu->gprs[rt], &cycles);
				break;
			default:
				cpu->memory.store32(cpu, address, cpu->gprs[rt], &cycles);
				cpu->memory.store32(cpu, address + 4, cpu->gprs[rt2], &cycles);
				break;
			}
			cpu->gprs[rd] = 0;
		} else {
			cpu->gprs[rd] = 1;
		}
		cpu->exclusiveMonitor = false;
	}
	cpu->cycles += cycles;
	return true;
}

/* 1111 100x xxxx xxxx: single loads and stores, and the memory hints. In the
 * first halfword, bit 8 is the signed-load bit, bit 7 selects the 12-bit
 * immediate form (or, for a literal, is U), bits 6:5 are the size and bit 4
 * is L. */
static bool _thumb2LoadStoreSingle(struct ARMCore* cpu, uint32_t opcode) {
	unsigned size = (HW1 >> 5) & 0x3;
	bool load = HW1 & 0x010;
	bool sign = HW1 & 0x100;
	int rn = HW1 & 0xF;
	int rt = HW2 >> 12;
	int cycles = 0;
	uint32_t address;
	bool writeback = false;
	uint32_t offsetAddress = 0;

	if (size == 3 || (sign && size == 2) || (!load && (sign || rn == ARM_PC))) {
		return false;
	}
	if (rn == ARM_PC) {
		/* Literal: word-aligned PC, plus or minus imm12. */
		uint32_t base = cpu->gprs[ARM_PC] & ~3;
		address = (HW1 & 0x80) ? base + (HW2 & 0xFFF) : base - (HW2 & 0xFFF);
	} else if (HW1 & 0x80) {
		address = cpu->gprs[rn] + (HW2 & 0xFFF);
	} else if (HW2 & 0x800) {
		/* imm8, with P, U and W at bits 10:8. P = 1, U = 1, W = 0 is the
		 * unprivileged form, which is the same thing in user mode. */
		bool p = HW2 & 0x400;
		bool u = HW2 & 0x200;
		writeback = HW2 & 0x100;
		if (!p && !writeback) {
			return false;
		}
		offsetAddress = u ? cpu->gprs[rn] + (HW2 & 0xFF) : cpu->gprs[rn] - (HW2 & 0xFF);
		address = p ? offsetAddress : (uint32_t) cpu->gprs[rn];
	} else if (!(HW2 & 0x7C0)) {
		address = cpu->gprs[rn] + ((uint32_t) cpu->gprs[HW2 & 0xF] << ((HW2 >> 4) & 0x3));
	} else {
		return false;
	}

	if (!load) {
		/* Storing the PC is UNPREDICTABLE; it stores the instruction's
		 * address plus 4. */
		switch (size) {
		case 0:
			cpu->memory.store8(cpu, address, cpu->gprs[rt], &cycles);
			break;
		case 1:
			cpu->memory.store16(cpu, address, cpu->gprs[rt], &cycles);
			break;
		default:
			cpu->memory.store32(cpu, address, cpu->gprs[rt], &cycles);
			break;
		}
		if (writeback) {
			cpu->gprs[rn] = offsetAddress;
		}
		cpu->cycles += cycles;
		return true;
	}

	if (rt == ARM_PC && size != 2 &&
	    (rn == ARM_PC || (HW1 & 0x80) || !(HW2 & 0x800) || (HW2 & 0xF00) == 0xC00)) {
		/* PLD, PLDW and PLI, and the unallocated hints around them: all
		 * no-ops, since touchHLE models no cache. The other forms with
		 * Rt == 15 -- writeback, and the unprivileged loads -- are
		 * UNPREDICTABLE loads to the PC, which interwork. */
		return true;
	}
	uint32_t value;
	switch (size) {
	case 0:
		value = cpu->memory.load8(cpu, address, &cycles);
		if (sign) {
			value = (uint32_t) (int32_t) (int8_t) value;
		}
		break;
	case 1:
		value = cpu->memory.load16(cpu, address, &cycles);
		if (sign) {
			value = (uint32_t) (int32_t) (int16_t) value;
		}
		break;
	default:
		value = cpu->memory.load32(cpu, address, &cycles);
		break;
	}
	if (writeback) {
		cpu->gprs[rn] = offsetAddress;
	}
	cpu->gprs[rt] = value;
	if (rt == ARM_PC) {
		/* Loads to the PC interwork. */
		cycles += ARMInterworkWritePC(cpu);
	}
	cpu->cycles += cycles;
	return true;
}

/* 1111 1010 xxxx xxxx 1111: data-processing, register. The register-
 * controlled shifts are implemented here; everything else has an ARM
 * twin. */
static bool _thumb2DataProcessingRegister(struct ARMCore* cpu, uint32_t opcode) {
	unsigned op1 = (HW1 >> 4) & 0xF;
	unsigned op2 = (HW2 >> 4) & 0xF;
	int rn = HW1 & 0xF;
	int rd = (HW2 >> 8) & 0xF;
	int rm = HW2 & 0xF;
	if ((HW2 & 0xF000) != 0xF000) {
		return false;
	}

	if (op1 < 0x8) {
		if (!op2) {
			/* LSL, LSR, ASR and ROR by register, S at bit 0 of op1. A
			 * rotate by a multiple of 32 changes nothing but C. */
			unsigned carry;
			if (rd == ARM_PC && (op1 & 1)) {
				/* A MOVS PC, undefined as in the other forms. */
				return false;
			}
			uint32_t result = _thumb2Shift(cpu->gprs[rn], (enum Thumb2Shift) (op1 >> 1),
				cpu->gprs[rm] & 0xFF, cpu->cpsr.c, &carry);
			if (op1 & 1) {
				_thumb2SetNZ(cpu, result);
				cpu->cpsr.c = carry;
			}
			_thumb2WriteReg(cpu, rd, result);
			return true;
		}
		if (op2 & 0x8) {
			/* The extends, with Rn == 15 as the non-accumulating form in
			 * both instruction sets. Bits 5:4 are the rotation. */
			static const uint8_t armOp[6] = {
				0x6B, /* SXTAH */
				0x6F, /* UXTAH */
				0x68, /* SXTAB16 */
				0x6C, /* UXTAB16 */
				0x6A, /* SXTAB */
				0x6E, /* UXTAB */
			};
			if (op1 > 5 || (op2 & 0x4)) {
				return false;
			}
			_thumb2RunARM(cpu, 0xE0000070 | ((uint32_t) armOp[op1] << 20) | (rn << 16) |
				(rd << 12) | (((HW2 >> 4) & 0x3) << 10) | rm);
			return true;
		}
		return false;
	}

	if (op2 < 0x8) {
		/* Parallel addition and subtraction. op2 picks the variant, which
		 * becomes bits 22:20 of the ARM form, and op1 the operation, which
		 * becomes bits 7:5. */
		static const int8_t armVariant[8] = { 1, 2, 3, -1, 5, 6, 7, -1 };
		static const int8_t armOperation[8] = {
			4,  /* ADD8 */
			0,  /* ADD16 */
			1,  /* ASX */
			-1,
			7,  /* SUB8 */
			3,  /* SUB16 */
			2,  /* SAX */
			-1,
		};
		int variant = armVariant[op2];
		int operation = armOperation[op1 & 0x7];
		if (variant < 0 || operation < 0) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE6000F10 | (variant << 20) | (rn << 16) | (rd << 12) |
			(operation << 5) | rm);
		return true;
	}

	if (op1 < 0xC && op2 < 0xC) {
		unsigned group = op1 & 0x3;
		unsigned which = op2 & 0x3;
		switch (group) {
		case 0: {
			/* QADD, QDADD, QSUB and QDSUB: result = Rm op Rn, as in the ARM
			 * encoding, which puts them in rows 0x10, 0x14, 0x12, 0x16. */
			static const uint8_t armRow[4] = { 0x10, 0x14, 0x12, 0x16 };
			_thumb2RunARM(cpu, 0xE0000050 | ((uint32_t) armRow[which] << 20) | (rn << 16) |
				(rd << 12) | rm);
			return true;
		}
		case 1: {
			/* REV, REV16, RBIT and REVSH. Rm appears twice in the Thumb
			 * encoding; the copy at bits 3:0 is the one used. */
			static const uint32_t arm[4] = { 0xE6BF0F30, 0xE6BF0FB0, 0xE6FF0F30, 0xE6FF0FB0 };
			_thumb2RunARM(cpu, arm[which] | (rd << 12) | rm);
			return true;
		}
		case 2:
			if (which) {
				return false;
			}
			_thumb2RunARM(cpu, 0xE6800FB0 | (rn << 16) | (rd << 12) | rm);
			return true;
		default:
			if (which) {
				return false;
			}
			_thumb2RunARM(cpu, 0xE16F0F10 | (rd << 12) | rm);
			return true;
		}
	}
	return false;
}

/* 1111 1011 0xxx xxxx: the 32-bit multiplies. The ARM forms take the first
 * operand at bits 3:0 and the second at 11:8, and the Thumb ones at bits 3:0
 * of each halfword; Ra and Rd keep their positions. */
static bool _thumb2Multiply(struct ARMCore* cpu, uint32_t opcode) {
	unsigned op1 = (HW1 >> 4) & 0x7;
	unsigned op2 = (HW2 >> 4) & 0x3;
	uint32_t rn = HW1 & 0xF;
	uint32_t ra = HW2 >> 12;
	uint32_t rd = (HW2 >> 8) & 0xF;
	uint32_t rm = HW2 & 0xF;
	uint32_t fields = (rd << 16) | (ra << 12) | (rm << 8) | rn;
	/* The PC as a destination is UNPREDICTABLE; the ARM forms treat it as a
	 * branch (see ARM_PC_DEST in isa-arm.c). */
	if (HW2 & 0x00C0) {
		return false;
	}
	switch (op1) {
	case 0:
		if (op2 == 0) {
			/* MUL, or MLA with an accumulator. */
			_thumb2RunARM(cpu, (ra == ARM_PC ? 0xE0000090 : 0xE0200090) | fields);
		} else if (op2 == 1) {
			_thumb2RunARM(cpu, 0xE0600090 | fields);
		} else {
			return false;
		}
		return true;
	case 1:
		/* SMLA<x><y>, or SMUL<x><y> with no accumulator. N (bit 5) picks
		 * Rn's half and M (bit 4) Rm's, which are bits 5 and 6 in ARM. */
		_thumb2RunARM(cpu, (ra == ARM_PC ? 0xE1600080 | (fields & ~0xF000u)
		                                 : 0xE1000080 | fields) |
			((op2 & 1) << 6) | ((op2 >> 1) << 5));
		return true;
	case 2:
		/* SMLAD and SMUAD, X at bit 4. */
		if (op2 & 2) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE7000010 | fields | ((op2 & 1) << 5));
		return true;
	case 3:
		/* SMLAW<y> and SMULW<y>, y at bit 4. */
		if (op2 & 2) {
			return false;
		}
		_thumb2RunARM(cpu, (ra == ARM_PC ? 0xE12000A0 | (fields & ~0xF000u)
		                                 : 0xE1200080 | fields) |
			((op2 & 1) << 6));
		return true;
	case 4:
		/* SMLSD and SMUSD. */
		if (op2 & 2) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE7000050 | fields | ((op2 & 1) << 5));
		return true;
	case 5:
		/* SMMLA and SMMUL, R (round) at bit 4. */
		if (op2 & 2) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE7500010 | fields | ((op2 & 1) << 5));
		return true;
	case 6:
		/* SMMLS. */
		if (op2 & 2) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE75000D0 | fields | ((op2 & 1) << 5));
		return true;
	default:
		/* USADA8 and USAD8. */
		if (op2) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE7800010 | fields);
		return true;
	}
}

/* 1111 1011 1xxx xxxx: long multiplies and divides. RdLo is at 15:12 and
 * RdHi at 11:8, which the ARM forms put at 15:12 and 19:16. */
static bool _thumb2LongMultiply(struct ARMCore* cpu, uint32_t opcode) {
	unsigned op1 = (HW1 >> 4) & 0x7;
	unsigned op2 = (HW2 >> 4) & 0xF;
	uint32_t rn = HW1 & 0xF;
	uint32_t rdLo = HW2 >> 12;
	uint32_t rdHi = (HW2 >> 8) & 0xF;
	uint32_t rm = HW2 & 0xF;
	uint32_t fields = (rdHi << 16) | (rdLo << 12) | (rm << 8) | rn;
	/* As for the short multiplies, a PC destination is a branch. SDIV and
	 * UDIV have 1111 in RdLo's place, which is checked below. */
	switch (op1) {
	case 0:
		if (op2) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE0C00090 | fields);
		return true;
	case 1:
	case 3:
		/* SDIV and UDIV. Rd is at 11:8, and 15:12 must be all ones. */
		if (op2 != 0xF || rdLo != 0xF) {
			return false;
		}
		_thumb2RunARM(cpu, (op1 == 1 ? 0xE710F010 : 0xE730F010) | (rdHi << 16) |
			(rm << 8) | rn);
		return true;
	case 2:
		if (op2) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE0800090 | fields);
		return true;
	case 4:
		if (op2 == 0) {
			_thumb2RunARM(cpu, 0xE0E00090 | fields);
		} else if ((op2 & 0xC) == 0x8) {
			/* SMLAL<x><y>: N at bit 5, M at bit 4. */
			_thumb2RunARM(cpu, 0xE1400080 | fields | ((op2 & 1) << 6) | (((op2 >> 1) & 1) << 5));
		} else if ((op2 & 0xE) == 0xC) {
			/* SMLALD, X at bit 4. */
			_thumb2RunARM(cpu, 0xE7400010 | fields | ((op2 & 1) << 5));
		} else {
			return false;
		}
		return true;
	case 5:
		/* SMLSLD. */
		if ((op2 & 0xE) != 0xC) {
			return false;
		}
		_thumb2RunARM(cpu, 0xE7400050 | fields | ((op2 & 1) << 5));
		return true;
	case 6:
		if (op2 == 0) {
			_thumb2RunARM(cpu, 0xE0A00090 | fields);
		} else if (op2 == 0x6) {
			_thumb2RunARM(cpu, 0xE0400090 | fields);
		} else {
			return false;
		}
		return true;
	default:
		return false;
	}
}

/* Advanced SIMD takes the ARM encoding. Thumb-2's data-processing form is
 * 111U 1111 where ARM's is 1111 001U, and its load/store form 1111 1001
 * where ARM's is 1111 0100; the other 24 bits are the same. */
static bool _thumb2AdvancedSimd(struct ARMCore* cpu, uint32_t opcode) {
	uint32_t arm;
	if ((HW1 & 0xEF00) == 0xEF00) {
		arm = 0xF2000000 | ((opcode >> 4) & 0x01000000) | (opcode & 0x00FFFFFF);
	} else {
		arm = 0xF4000000 | (opcode & 0x00FFFFFF);
	}
	return cpu->advancedSimd && cpu->advancedSimd(cpu, arm);
}

/* 111x 11xx xxxx xxxx: coprocessor instructions, which is to say VFP, and
 * Advanced SIMD data processing. With bit 12 clear the coprocessor ones are
 * exactly the ARM encodings with an AL condition, so the handlers vfp.c and
 * cp15.c install take them as they are. Bit 12 set is the unconditional
 * coprocessor forms, which are not implemented. */
static bool _thumb2Coprocessor(struct ARMCore* cpu, uint32_t opcode) {
	int cp = (HW2 >> 8) & 0xF;
	if ((HW1 & 0xEF00) == 0xEF00) {
		return _thumb2AdvancedSimd(cpu, opcode);
	}
	if ((HW1 & 0x1000) || (HW1 & 0x0300) == 0x0300) {
		return false;
	}
	return cpu->cp[cp].raw && cpu->cp[cp].raw(cpu, opcode);
}

static bool _thumb2Execute(struct ARMCore* cpu, uint32_t opcode) {
	switch ((HW1 >> 11) & 0x3) {
	case 1:
		/* 11101 */
		if (HW1 & 0x0400) {
			return _thumb2Coprocessor(cpu, opcode);
		}
		if (HW1 & 0x0200) {
			return _thumb2DataProcessingShifted(cpu, opcode);
		}
		if (HW1 & 0x0040) {
			return _thumb2LoadStoreDual(cpu, opcode);
		}
		return _thumb2LoadStoreMultiple(cpu, opcode);
	case 2:
		/* 11110 */
		if (HW2 & 0x8000) {
			return _thumb2BranchMisc(cpu, opcode);
		}
		if (HW1 & 0x0200) {
			return _thumb2DataProcessingPlainImmediate(cpu, opcode);
		}
		return _thumb2DataProcessingModifiedImmediate(cpu, opcode);
	case 3:
		/* 11111 */
		if (HW1 & 0x0400) {
			return _thumb2Coprocessor(cpu, opcode);
		}
		if ((HW1 & 0xFF10) == 0xF900) {
			return _thumb2AdvancedSimd(cpu, opcode);
		}
		if (!(HW1 & 0x0200)) {
			return _thumb2LoadStoreSingle(cpu, opcode);
		}
		if (!(HW1 & 0x0100)) {
			return _thumb2DataProcessingRegister(cpu, opcode);
		}
		if (!(HW1 & 0x0080)) {
			return _thumb2Multiply(cpu, opcode);
		}
		return _thumb2LongMultiply(cpu, opcode);
	default:
		return false;
	}
}

void _ThumbInstructionTHUMB32(struct ARMCore* cpu, unsigned hw1) {
	uint32_t opcode = (hw1 << 16) | (cpu->prefetch[0] & 0xFFFF);
	cpu->pcWritten = false;
	cpu->cycles += 1 + cpu->memory.activeSeqCycles16;
	if (!_thumb2Execute(cpu, opcode)) {
		/* Reported before moving past the second halfword, so r15 still
		 * places the instruction correctly. */
		cpu->irqh.hitIllegal(cpu, opcode);
		return;
	}
	/* Anything that branched has refilled the prefetch from its target;
	 * otherwise step over the second halfword. */
	if (cpu->executionMode == MODE_THUMB && !cpu->pcWritten) {
		cpu->prefetch[0] = cpu->prefetch[1];
		cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
		LOAD_16(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask,
			cpu->memory.activeRegion);
	}
}
