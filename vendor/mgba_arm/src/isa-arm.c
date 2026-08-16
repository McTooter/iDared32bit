/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/arm/isa-arm.h>

#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/emitter-arm.h>
#include <mgba/internal/arm/isa-inlines.h>

// The flags field is NZCV on ARMv4T and NZCVQ from ARMv5TE, and the status
// field carries the ARMv6 GE flags.
#define PSR_USER_MASK   0xF8000000
#define PSR_STATUS_MASK 0x000F0000
#define PSR_PRIV_MASK   0x000000CF
#define PSR_STATE_MASK  0x00000020

// Addressing mode 1
static inline void _shiftLSL(struct ARMCore* cpu, uint32_t opcode) {
	int rm = opcode & 0x0000000F;
	if (opcode & 0x00000010) {
		int rs = (opcode >> 8) & 0x0000000F;
		++cpu->cycles;
		int32_t shiftVal = cpu->gprs[rm];
		int shift = cpu->gprs[rs] & 0xFF;
		if (!shift) {
			cpu->shifterOperand = shiftVal;
			cpu->shifterCarryOut = cpu->cpsr.c;
		} else if (shift < 32) {
			cpu->shifterOperand = shiftVal << shift;
			cpu->shifterCarryOut = (shiftVal >> (32 - shift)) & 1;
		} else if (shift == 32) {
			cpu->shifterOperand = 0;
			cpu->shifterCarryOut = shiftVal & 1;
		} else {
			cpu->shifterOperand = 0;
			cpu->shifterCarryOut = 0;
		}
	} else {
		int immediate = (opcode & 0x00000F80) >> 7;
		if (!immediate) {
			cpu->shifterOperand = cpu->gprs[rm];
			cpu->shifterCarryOut = cpu->cpsr.c;
		} else {
			cpu->shifterOperand = cpu->gprs[rm] << immediate;
			cpu->shifterCarryOut = (cpu->gprs[rm] >> (32 - immediate)) & 1;
		}
	}
}

static inline void _shiftLSR(struct ARMCore* cpu, uint32_t opcode) {
	int rm = opcode & 0x0000000F;
	if (opcode & 0x00000010) {
		int rs = (opcode >> 8) & 0x0000000F;
		++cpu->cycles;
		uint32_t shiftVal = cpu->gprs[rm];
		int shift = cpu->gprs[rs] & 0xFF;
		if (!shift) {
			cpu->shifterOperand = shiftVal;
			cpu->shifterCarryOut = cpu->cpsr.c;
		} else if (shift < 32) {
			cpu->shifterOperand = shiftVal >> shift;
			cpu->shifterCarryOut = (shiftVal >> (shift - 1)) & 1;
		} else if (shift == 32) {
			cpu->shifterOperand = 0;
			cpu->shifterCarryOut = shiftVal >> 31;
		} else {
			cpu->shifterOperand = 0;
			cpu->shifterCarryOut = 0;
		}
	} else {
		int immediate = (opcode & 0x00000F80) >> 7;
		if (immediate) {
			cpu->shifterOperand = ((uint32_t) cpu->gprs[rm]) >> immediate;
			cpu->shifterCarryOut = (cpu->gprs[rm] >> (immediate - 1)) & 1;
		} else {
			cpu->shifterOperand = 0;
			cpu->shifterCarryOut = ARM_SIGN(cpu->gprs[rm]);
		}
	}
}

static inline void _shiftASR(struct ARMCore* cpu, uint32_t opcode) {
	int rm = opcode & 0x0000000F;
	if (opcode & 0x00000010) {
		int rs = (opcode >> 8) & 0x0000000F;
		++cpu->cycles;
		int shiftVal =  cpu->gprs[rm];
		int shift = cpu->gprs[rs] & 0xFF;
		if (!shift) {
			cpu->shifterOperand = shiftVal;
			cpu->shifterCarryOut = cpu->cpsr.c;
		} else if (shift < 32) {
			cpu->shifterOperand = shiftVal >> shift;
			cpu->shifterCarryOut = (shiftVal >> (shift - 1)) & 1;
		} else if (cpu->gprs[rm] >> 31) {
			cpu->shifterOperand = 0xFFFFFFFF;
			cpu->shifterCarryOut = 1;
		} else {
			cpu->shifterOperand = 0;
			cpu->shifterCarryOut = 0;
		}
	} else {
		int immediate = (opcode & 0x00000F80) >> 7;
		if (immediate) {
			cpu->shifterOperand = cpu->gprs[rm] >> immediate;
			cpu->shifterCarryOut = (cpu->gprs[rm] >> (immediate - 1)) & 1;
		} else {
			cpu->shifterCarryOut = ARM_SIGN(cpu->gprs[rm]);
			cpu->shifterOperand = cpu->shifterCarryOut;
		}
	}
}

static inline void _shiftROR(struct ARMCore* cpu, uint32_t opcode) {
	int rm = opcode & 0x0000000F;
	if (opcode & 0x00000010) {
		int rs = (opcode >> 8) & 0x0000000F;
		++cpu->cycles;
		int shiftVal =  cpu->gprs[rm];
		int shift = cpu->gprs[rs] & 0xFF;
		int rotate = shift & 0x1F;
		if (!shift) {
			cpu->shifterOperand = shiftVal;
			cpu->shifterCarryOut = cpu->cpsr.c;
		} else if (rotate) {
			cpu->shifterOperand = ROR(shiftVal, rotate);
			cpu->shifterCarryOut = (shiftVal >> (rotate - 1)) & 1;
		} else {
			cpu->shifterOperand = shiftVal;
			cpu->shifterCarryOut = ARM_SIGN(shiftVal);
		}
	} else {
		int immediate = (opcode & 0x00000F80) >> 7;
		if (immediate) {
			cpu->shifterOperand = ROR(cpu->gprs[rm], immediate);
			cpu->shifterCarryOut = (cpu->gprs[rm] >> (immediate - 1)) & 1;
		} else {
			// RRX
			cpu->shifterOperand = (cpu->cpsr.c << 31) | (((uint32_t) cpu->gprs[rm]) >> 1);
			cpu->shifterCarryOut = cpu->gprs[rm] & 0x00000001;
		}
	}
}

static inline void _immediate(struct ARMCore* cpu, uint32_t opcode) {
	int rotate = (opcode & 0x00000F00) >> 7;
	int immediate = opcode & 0x000000FF;
	if (!rotate) {
		cpu->shifterOperand = immediate;
		cpu->shifterCarryOut = cpu->cpsr.c;
	} else {
		cpu->shifterOperand = ROR(immediate, rotate);
		cpu->shifterCarryOut = ARM_SIGN(cpu->shifterOperand);
	}
}

// Instruction definitions
// Beware pre-processor antics

// These set N, Z, C and V one by one. Upstream first cleared the whole top
// byte of the CPSR, which on ARMv4T is only those four, but from ARMv5TE it
// also holds the sticky Q flag, and from ARMv6T2 two bits of the IT state.
ATTRIBUTE_NOINLINE static void _additionS(struct ARMCore* cpu, int32_t m, int32_t n, int32_t d) {
	cpu->cpsr.n = ARM_SIGN(d);
	cpu->cpsr.z = !d;
	cpu->cpsr.c = ARM_CARRY_FROM(m, n, d);
	cpu->cpsr.v = ARM_V_ADDITION(m, n, d);
}

ATTRIBUTE_NOINLINE static void _subtractionS(struct ARMCore* cpu, int32_t m, int32_t n, int32_t d) {
	cpu->cpsr.n = ARM_SIGN(d);
	cpu->cpsr.z = !d;
	cpu->cpsr.c = ARM_BORROW_FROM(m, n, d);
	cpu->cpsr.v = ARM_V_SUBTRACTION(m, n, d);
}

ATTRIBUTE_NOINLINE static void _neutralS(struct ARMCore* cpu, int32_t d) {
	cpu->cpsr.n = ARM_SIGN(d);
	cpu->cpsr.z = !d; \
	cpu->cpsr.c = cpu->shifterCarryOut; \
}

#define ARM_ADDITION_S(M, N, D) \
	if (rd == ARM_PC && _ARMModeHasSPSR(cpu->cpsr.priv)) { \
		cpu->cpsr = cpu->spsr; \
		_ARMReadCPSR(cpu); \
	} else { \
		_additionS(cpu, M, N, D); \
	}

#define ARM_SUBTRACTION_S(M, N, D) \
	if (rd == ARM_PC && _ARMModeHasSPSR(cpu->cpsr.priv)) { \
		cpu->cpsr = cpu->spsr; \
		_ARMReadCPSR(cpu); \
	} else { \
		_subtractionS(cpu, M, N, D); \
	}

#define ARM_SUBTRACTION_CARRY_S(M, N, D, C) \
	if (rd == ARM_PC && _ARMModeHasSPSR(cpu->cpsr.priv)) { \
		cpu->cpsr = cpu->spsr; \
		_ARMReadCPSR(cpu); \
	} else { \
		cpu->cpsr.n = ARM_SIGN(D); \
		cpu->cpsr.z = !(D); \
		cpu->cpsr.c = ARM_BORROW_FROM_CARRY(M, N, D, C); \
		cpu->cpsr.v = ARM_V_SUBTRACTION(M, N, D); \
	}

#define ARM_NEUTRAL_S(M, N, D) \
	if (rd == ARM_PC && _ARMModeHasSPSR(cpu->cpsr.priv)) { \
		cpu->cpsr = cpu->spsr; \
		_ARMReadCPSR(cpu); \
	} else { \
		_neutralS(cpu, D); \
	}

#define ARM_NEUTRAL_HI_S(DLO, DHI) \
	cpu->cpsr.n = ARM_SIGN(DHI); \
	cpu->cpsr.z = !((DHI) | (DLO));

#define ADDR_MODE_2_I_TEST (opcode & 0x00000F80)
#define ADDR_MODE_2_I ((opcode & 0x00000F80) >> 7)
#define ADDR_MODE_2_ADDRESS (address)
#define ADDR_MODE_2_RN (cpu->gprs[rn])
#define ADDR_MODE_2_RM (cpu->gprs[rm])
#define ADDR_MODE_2_IMMEDIATE (opcode & 0x00000FFF)
#define ADDR_MODE_2_INDEX(U_OP, M) (cpu->gprs[rn] U_OP M)
#define ADDR_MODE_2_WRITEBACK(ADDR) \
	if (UNLIKELY(rn == ARM_PC)) { \
		ARMWritePCUnpredictable(cpu, ADDR); \
	} else { \
		cpu->gprs[rn] = ADDR; \
	}

#define ADDR_MODE_2_WRITEBACK_PRE_STORE(WB)
#define ADDR_MODE_2_WRITEBACK_POST_STORE(WB) WB
#define ADDR_MODE_2_WRITEBACK_PRE_LOAD(WB) WB
#define ADDR_MODE_2_WRITEBACK_POST_LOAD(WB)
#define ADDR_MODE_2_WRITEBACK_PRE_LOADD(WB)
#define ADDR_MODE_2_WRITEBACK_POST_LOADD(WB)

#define ADDR_MODE_2_LSL (cpu->gprs[rm] << ADDR_MODE_2_I)
#define ADDR_MODE_2_LSR (ADDR_MODE_2_I_TEST ? ((uint32_t) cpu->gprs[rm]) >> ADDR_MODE_2_I : 0)
#define ADDR_MODE_2_ASR (ADDR_MODE_2_I_TEST ? ((int32_t) cpu->gprs[rm]) >> ADDR_MODE_2_I : ((int32_t) cpu->gprs[rm]) >> 31)
#define ADDR_MODE_2_ROR (ADDR_MODE_2_I_TEST ? ROR(cpu->gprs[rm], ADDR_MODE_2_I) : (cpu->cpsr.c << 31) | (((uint32_t) cpu->gprs[rm]) >> 1))

#define ADDR_MODE_3_ADDRESS ADDR_MODE_2_ADDRESS
#define ADDR_MODE_3_RN ADDR_MODE_2_RN
#define ADDR_MODE_3_RM ADDR_MODE_2_RM
#define ADDR_MODE_3_IMMEDIATE (((opcode & 0x00000F00) >> 4) | (opcode & 0x0000000F))
#define ADDR_MODE_3_INDEX(U_OP, M) ADDR_MODE_2_INDEX(U_OP, M)
#define ADDR_MODE_3_WRITEBACK(ADDR) ADDR_MODE_2_WRITEBACK(ADDR)

#define ADDR_MODE_4_WRITEBACK_LDM \
		if (!((1 << rn) & rs)) { \
			cpu->gprs[rn] = address; \
		}

#define ADDR_MODE_4_WRITEBACK_STM cpu->gprs[rn] = address;

// A load to the PC interworks from ARMv5T; see ARMInterworkWritePC.
#define ARM_LOAD_POST_BODY \
	currentCycles += cpu->memory.activeNonseqCycles32 - cpu->memory.activeSeqCycles32; \
	if (rd == ARM_PC) { \
		currentCycles += ARMInterworkWritePC(cpu); \
	}

#define ARM_STORE_POST_BODY \
	currentCycles += cpu->memory.activeNonseqCycles32 - cpu->memory.activeSeqCycles32;

// pcDest is set by instructions for which a destination of r15 is
// UNPREDICTABLE, and which have written their result to gprs[ARM_PC]
// directly; it is turned into a branch afterwards (see ARM_PC_DEST).
#define DEFINE_INSTRUCTION_ARM(NAME, BODY) \
	static void _ARMInstruction ## NAME (struct ARMCore* cpu, uint32_t opcode) { \
		int currentCycles = ARM_PREFETCH_CYCLES; \
		bool pcDest = false; \
		BODY; \
		cpu->cycles += currentCycles; \
		if (UNLIKELY(pcDest)) { \
			ARMWritePCUnpredictable(cpu, cpu->gprs[ARM_PC]); \
		} \
	}

#define DEFINE_ALU_INSTRUCTION_EX_ARM(NAME, S_BODY, SHIFTER, BODY, DO_WRITE) \
	DEFINE_INSTRUCTION_ARM(NAME, \
		SHIFTER(cpu, opcode); \
		int rd = (opcode >> 12) & 0xF; \
		int rn = (opcode >> 16) & 0xF; \
		/* The PC reads as the instruction plus 8 even with a register- \
		 * specified shift, where ARM7TDMI read plus 12. Unpredictable from \
		 * ARMv6, and consistent with every other read of the PC. */ \
		int32_t n ATTRIBUTE_UNUSED = cpu->gprs[rn]; \
		BODY; \
		S_BODY; \
		/* ARMv7 interworks here too (ALUWritePC). The mode is only already \
		 * Thumb if S_BODY restored the CPSR. */ \
		if (DO_WRITE && rd == ARM_PC) { \
			if (cpu->executionMode == MODE_ARM) { \
				currentCycles += ARMInterworkWritePC(cpu); \
			} else { \
				currentCycles += ThumbWritePC(cpu); \
			} \
		})

#define DEFINE_ALU_INSTRUCTION_ARM(NAME, S_BODY, BODY) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## _LSL, , _shiftLSL, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## S_LSL, S_BODY, _shiftLSL, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## _LSR, , _shiftLSR, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## S_LSR, S_BODY, _shiftLSR, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## _ASR, , _shiftASR, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## S_ASR, S_BODY, _shiftASR, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## _ROR, , _shiftROR, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## S_ROR, S_BODY, _shiftROR, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## I, , _immediate, BODY, 1) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## SI, S_BODY, _immediate, BODY, 1)

#define DEFINE_ALU_INSTRUCTION_S_ONLY_ARM(NAME, S_BODY, BODY) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## _LSL, S_BODY, _shiftLSL, BODY, 0) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## _LSR, S_BODY, _shiftLSR, BODY, 0) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## _ASR, S_BODY, _shiftASR, BODY, 0) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## _ROR, S_BODY, _shiftROR, BODY, 0) \
	DEFINE_ALU_INSTRUCTION_EX_ARM(NAME ## I, S_BODY, _immediate, BODY, 0)

#define DEFINE_MULTIPLY_INSTRUCTION_EX_ARM(NAME, BODY, S_BODY, SIGNED) \
	DEFINE_INSTRUCTION_ARM(NAME, \
		int rd = (opcode >> 16) & 0xF; \
		int rs = (opcode >> 8) & 0xF; \
		int rm = opcode & 0xF; \
		ARM_WAIT_ ## SIGNED ## MUL(cpu->gprs[rs], 0); \
		BODY; \
		S_BODY; \
		pcDest = rd == ARM_PC; \
		currentCycles += cpu->memory.activeNonseqCycles32 - cpu->memory.activeSeqCycles32)

#define DEFINE_MULTIPLY_INSTRUCTION_2_EX_ARM(NAME, BODY, S_BODY, SIGNED, WAIT) \
	DEFINE_INSTRUCTION_ARM(NAME, \
		int rd = (opcode >> 12) & 0xF; \
		int rdHi = (opcode >> 16) & 0xF; \
		int rs = (opcode >> 8) & 0xF; \
		int rm = opcode & 0xF; \
		ARM_WAIT_ ## SIGNED ## MUL(cpu->gprs[rs], WAIT); \
		BODY; \
		S_BODY; \
		pcDest = rd == ARM_PC || rdHi == ARM_PC; \
		currentCycles += cpu->memory.activeNonseqCycles32 - cpu->memory.activeSeqCycles32)

#define DEFINE_MULTIPLY_INSTRUCTION_ARM(NAME, BODY, S_BODY, SIGNED) \
	DEFINE_MULTIPLY_INSTRUCTION_EX_ARM(NAME, BODY, , SIGNED) \
	DEFINE_MULTIPLY_INSTRUCTION_EX_ARM(NAME ## S, BODY, S_BODY, SIGNED)

#define DEFINE_MULTIPLY_INSTRUCTION_2_ARM(NAME, BODY, S_BODY, SIGNED, WAIT) \
	DEFINE_MULTIPLY_INSTRUCTION_2_EX_ARM(NAME, BODY, , SIGNED, WAIT) \
	DEFINE_MULTIPLY_INSTRUCTION_2_EX_ARM(NAME ## S, BODY, S_BODY, SIGNED, WAIT)

#define DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME, ADDRESS, WRITEBACK, LS, BODY) \
	DEFINE_INSTRUCTION_ARM(NAME, \
		uint32_t address; \
		int rn = (opcode >> 16) & 0xF; \
		int rd = (opcode >> 12) & 0xF; \
		/* Storing the PC stores the instruction's address plus 8, as every \
		 * other read of it does. That is ARMv7's rule, and what STM here \
		 * stores too; ARM7TDMI stored plus 12. */ \
		int32_t d ATTRIBUTE_UNUSED = cpu->gprs[rd]; \
		int rm = opcode & 0xF; \
		UNUSED(rm); \
		address = ADDRESS; \
		ADDR_MODE_2_WRITEBACK_PRE_ ## LS (WRITEBACK); \
		BODY; \
		ADDR_MODE_2_WRITEBACK_POST_ ## LS (WRITEBACK);)

#define DEFINE_LOAD_STORE_INSTRUCTION_SHIFTER_ARM(NAME, SHIFTER, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME, ADDR_MODE_2_RN, ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_INDEX(-, SHIFTER)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## U, ADDR_MODE_2_RN, ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_INDEX(+, SHIFTER)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## P, ADDR_MODE_2_INDEX(-, SHIFTER), , LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## PW, ADDR_MODE_2_INDEX(-, SHIFTER), ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_ADDRESS), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## PU, ADDR_MODE_2_INDEX(+, SHIFTER), , LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## PUW, ADDR_MODE_2_INDEX(+, SHIFTER), ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_ADDRESS), LS, BODY)

#define DEFINE_LOAD_STORE_INSTRUCTION_ARM(NAME, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_SHIFTER_ARM(NAME ## _LSL_, ADDR_MODE_2_LSL, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_SHIFTER_ARM(NAME ## _LSR_, ADDR_MODE_2_LSR, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_SHIFTER_ARM(NAME ## _ASR_, ADDR_MODE_2_ASR, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_SHIFTER_ARM(NAME ## _ROR_, ADDR_MODE_2_ROR, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## I, ADDR_MODE_2_RN, ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_INDEX(-, ADDR_MODE_2_IMMEDIATE)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IU, ADDR_MODE_2_RN, ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_INDEX(+, ADDR_MODE_2_IMMEDIATE)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IP, ADDR_MODE_2_INDEX(-, ADDR_MODE_2_IMMEDIATE), , LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IPW, ADDR_MODE_2_INDEX(-, ADDR_MODE_2_IMMEDIATE), ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_ADDRESS), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IPU, ADDR_MODE_2_INDEX(+, ADDR_MODE_2_IMMEDIATE), , LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IPUW, ADDR_MODE_2_INDEX(+, ADDR_MODE_2_IMMEDIATE), ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_ADDRESS), LS, BODY) \

#define DEFINE_LOAD_STORE_MODE_3_INSTRUCTION_ARM(NAME, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME, ADDR_MODE_3_RN, ADDR_MODE_3_WRITEBACK(ADDR_MODE_3_INDEX(-, ADDR_MODE_3_RM)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## U, ADDR_MODE_3_RN, ADDR_MODE_3_WRITEBACK(ADDR_MODE_3_INDEX(+, ADDR_MODE_3_RM)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## P, ADDR_MODE_3_INDEX(-, ADDR_MODE_3_RM), , LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## PW, ADDR_MODE_3_INDEX(-, ADDR_MODE_3_RM), ADDR_MODE_3_WRITEBACK(ADDR_MODE_3_ADDRESS), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## PU, ADDR_MODE_3_INDEX(+, ADDR_MODE_3_RM), , LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## PUW, ADDR_MODE_3_INDEX(+, ADDR_MODE_3_RM), ADDR_MODE_3_WRITEBACK(ADDR_MODE_3_ADDRESS), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## I, ADDR_MODE_3_RN, ADDR_MODE_3_WRITEBACK(ADDR_MODE_3_INDEX(-, ADDR_MODE_3_IMMEDIATE)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IU, ADDR_MODE_3_RN, ADDR_MODE_3_WRITEBACK(ADDR_MODE_3_INDEX(+, ADDR_MODE_3_IMMEDIATE)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IP, ADDR_MODE_3_INDEX(-, ADDR_MODE_3_IMMEDIATE), , LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IPW, ADDR_MODE_3_INDEX(-, ADDR_MODE_3_IMMEDIATE), ADDR_MODE_3_WRITEBACK(ADDR_MODE_3_ADDRESS), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IPU, ADDR_MODE_3_INDEX(+, ADDR_MODE_3_IMMEDIATE), , LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IPUW, ADDR_MODE_3_INDEX(+, ADDR_MODE_3_IMMEDIATE), ADDR_MODE_3_WRITEBACK(ADDR_MODE_3_ADDRESS), LS, BODY) \

#define DEFINE_LOAD_STORE_T_INSTRUCTION_SHIFTER_ARM(NAME, SHIFTER, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME, ADDR_MODE_2_RN, ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_INDEX(-, SHIFTER)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## U, ADDR_MODE_2_RN, ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_INDEX(+, SHIFTER)), LS, BODY) \

#define DEFINE_LOAD_STORE_T_INSTRUCTION_ARM(NAME, LS, BODY) \
	DEFINE_LOAD_STORE_T_INSTRUCTION_SHIFTER_ARM(NAME ## _LSL_, ADDR_MODE_2_LSL, LS, BODY) \
	DEFINE_LOAD_STORE_T_INSTRUCTION_SHIFTER_ARM(NAME ## _LSR_, ADDR_MODE_2_LSR, LS, BODY) \
	DEFINE_LOAD_STORE_T_INSTRUCTION_SHIFTER_ARM(NAME ## _ASR_, ADDR_MODE_2_ASR, LS, BODY) \
	DEFINE_LOAD_STORE_T_INSTRUCTION_SHIFTER_ARM(NAME ## _ROR_, ADDR_MODE_2_ROR, LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## I, ADDR_MODE_2_RN, ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_INDEX(-, ADDR_MODE_2_IMMEDIATE)), LS, BODY) \
	DEFINE_LOAD_STORE_INSTRUCTION_EX_ARM(NAME ## IU, ADDR_MODE_2_RN, ADDR_MODE_2_WRITEBACK(ADDR_MODE_2_INDEX(+, ADDR_MODE_2_IMMEDIATE)), LS, BODY) \

#define ARM_MS_PRE_store \
	enum PrivilegeMode privilegeMode = cpu->privilegeMode; \
	ARMSetPrivilegeMode(cpu, MODE_SYSTEM);

#define ARM_MS_PRE_load \
	enum PrivilegeMode privilegeMode; \
	if (!(rs & 0x8000) && rs) { \
		privilegeMode = cpu->privilegeMode; \
		ARMSetPrivilegeMode(cpu, MODE_SYSTEM); \
	}

#define ARM_MS_POST_store ARMSetPrivilegeMode(cpu, privilegeMode);

#define ARM_MS_POST_load \
	if (!(rs & 0x8000) && rs) { \
		ARMSetPrivilegeMode(cpu, privilegeMode); \
	} else if (_ARMModeHasSPSR(cpu->cpsr.priv)) { \
		cpu->cpsr = cpu->spsr; \
		_ARMReadCPSR(cpu); \
	}

#define DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME, LS, WRITEBACK, S_PRE, S_POST, DIRECTION, POST_BODY) \
	DEFINE_INSTRUCTION_ARM(NAME, \
		int rn = (opcode >> 16) & 0xF; \
		int rs = opcode & 0x0000FFFF; \
		uint32_t address = cpu->gprs[rn]; \
		S_PRE; \
		address = cpu->memory. LS ## Multiple(cpu, address, rs, LSM_ ## DIRECTION, &currentCycles); \
		WRITEBACK; \
		S_POST; \
		POST_BODY;)


#define DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_ARM(NAME, LS, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## DA,   LS,                               ,                  ,                   , DA, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## DAW,  LS, ADDR_MODE_4_WRITEBACK_ ## NAME,                  ,                   , DA, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## DB,   LS,                               ,                  ,                   , DB, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## DBW,  LS, ADDR_MODE_4_WRITEBACK_ ## NAME,                  ,                   , DB, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## IA,   LS,                               ,                  ,                   , IA, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## IAW,  LS, ADDR_MODE_4_WRITEBACK_ ## NAME,                  ,                   , IA, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## IB,   LS,                               ,                  ,                   , IB, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## IBW,  LS, ADDR_MODE_4_WRITEBACK_ ## NAME,                  ,                   , IB, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## SDA,  LS,                               , ARM_MS_PRE_ ## LS, ARM_MS_POST_ ## LS, DA, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## SDAW, LS, ADDR_MODE_4_WRITEBACK_ ## NAME, ARM_MS_PRE_ ## LS, ARM_MS_POST_ ## LS, DA, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## SDB,  LS,                               , ARM_MS_PRE_ ## LS, ARM_MS_POST_ ## LS, DB, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## SDBW, LS, ADDR_MODE_4_WRITEBACK_ ## NAME, ARM_MS_PRE_ ## LS, ARM_MS_POST_ ## LS, DB, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## SIA,  LS,                               , ARM_MS_PRE_ ## LS, ARM_MS_POST_ ## LS, IA, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## SIAW, LS, ADDR_MODE_4_WRITEBACK_ ## NAME, ARM_MS_PRE_ ## LS, ARM_MS_POST_ ## LS, IA, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## SIB,  LS,                               , ARM_MS_PRE_ ## LS, ARM_MS_POST_ ## LS, IB, POST_BODY) \
	DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_EX_ARM(NAME ## SIBW, LS, ADDR_MODE_4_WRITEBACK_ ## NAME, ARM_MS_PRE_ ## LS, ARM_MS_POST_ ## LS, IB, POST_BODY)

// Begin ALU definitions

DEFINE_ALU_INSTRUCTION_ARM(ADD, ARM_ADDITION_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = n + cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_ARM(ADC, ARM_ADDITION_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = n + cpu->shifterOperand + cpu->cpsr.c;)

DEFINE_ALU_INSTRUCTION_ARM(AND, ARM_NEUTRAL_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = n & cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_ARM(BIC, ARM_NEUTRAL_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = n & ~cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_S_ONLY_ARM(CMN, ARM_ADDITION_S(n, cpu->shifterOperand, aluOut),
	int32_t aluOut = n + cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_S_ONLY_ARM(CMP, ARM_SUBTRACTION_S(n, cpu->shifterOperand, aluOut),
	int32_t aluOut = n - cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_ARM(EOR, ARM_NEUTRAL_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = n ^ cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_ARM(MOV, ARM_NEUTRAL_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_ARM(MVN, ARM_NEUTRAL_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = ~cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_ARM(ORR, ARM_NEUTRAL_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = n | cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_ARM(RSB, ARM_SUBTRACTION_S(cpu->shifterOperand, n, cpu->gprs[rd]),
	cpu->gprs[rd] = cpu->shifterOperand - n;)

DEFINE_ALU_INSTRUCTION_ARM(RSC, ARM_SUBTRACTION_CARRY_S(cpu->shifterOperand, n, cpu->gprs[rd], !cpu->cpsr.c),
	cpu->gprs[rd] = cpu->shifterOperand - n - !cpu->cpsr.c;)

DEFINE_ALU_INSTRUCTION_ARM(SBC, ARM_SUBTRACTION_CARRY_S(n, cpu->shifterOperand, cpu->gprs[rd], !cpu->cpsr.c),
	cpu->gprs[rd] = n - cpu->shifterOperand - !cpu->cpsr.c;)

DEFINE_ALU_INSTRUCTION_ARM(SUB, ARM_SUBTRACTION_S(n, cpu->shifterOperand, cpu->gprs[rd]),
	cpu->gprs[rd] = n - cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_S_ONLY_ARM(TEQ, ARM_NEUTRAL_S(n, cpu->shifterOperand, aluOut),
	int32_t aluOut = n ^ cpu->shifterOperand;)

DEFINE_ALU_INSTRUCTION_S_ONLY_ARM(TST, ARM_NEUTRAL_S(n, cpu->shifterOperand, aluOut),
	int32_t aluOut = n & cpu->shifterOperand;)

// End ALU definitions

// Begin multiply definitions

// The S forms set N and Z only. ARMv4 left C meaningless, and upstream sets
// it from the stale shifter carry; from ARMv5 it is unchanged.
// MLA's Rd is at 19:16 (rdHi here) and Ra at 15:12 (rd), so its PC
// destination is rdHi alone.
DEFINE_MULTIPLY_INSTRUCTION_2_ARM(MLA, int32_t r = cpu->gprs[rm] * cpu->gprs[rs] + cpu->gprs[rd]; cpu->gprs[rdHi] = r; rd = rdHi, ARM_NEUTRAL_HI_S(0, r), S, 1)
DEFINE_MULTIPLY_INSTRUCTION_ARM(MUL, int32_t r = cpu->gprs[rm] * cpu->gprs[rs]; cpu->gprs[rd] = r, ARM_NEUTRAL_HI_S(0, r), S)

DEFINE_MULTIPLY_INSTRUCTION_2_ARM(SMLAL,
	int64_t d = ((int64_t) cpu->gprs[rm]) * ((int64_t) cpu->gprs[rs]) + ((uint32_t) cpu->gprs[rd]);
	int32_t dHi = cpu->gprs[rdHi] + (d >> 32);
	cpu->gprs[rd] = d;
	cpu->gprs[rdHi] = dHi;,
	ARM_NEUTRAL_HI_S((int32_t) d, dHi), S, 2)

DEFINE_MULTIPLY_INSTRUCTION_2_ARM(SMULL,
	int64_t d = ((int64_t) cpu->gprs[rm]) * ((int64_t) cpu->gprs[rs]);
	cpu->gprs[rd] = d;
	cpu->gprs[rdHi] = d >> 32;,
	ARM_NEUTRAL_HI_S((int32_t) d, (int32_t) (d >> 32)), S, 1)

DEFINE_MULTIPLY_INSTRUCTION_2_ARM(UMLAL,
	uint64_t d = ARM_UXT_64(cpu->gprs[rm]) * ARM_UXT_64(cpu->gprs[rs]) + ((uint32_t) cpu->gprs[rd]);
	uint32_t dHi = ((uint32_t) cpu->gprs[rdHi]) + (d >> 32);
	cpu->gprs[rd] = d;
	cpu->gprs[rdHi] = dHi;,
	ARM_NEUTRAL_HI_S((int32_t) d, dHi), U, 2)

DEFINE_MULTIPLY_INSTRUCTION_2_ARM(UMULL,
	uint64_t d = ARM_UXT_64(cpu->gprs[rm]) * ARM_UXT_64(cpu->gprs[rs]);
	cpu->gprs[rd] = d;
	cpu->gprs[rdHi] = d >> 32;,
	ARM_NEUTRAL_HI_S((int32_t) d, (int32_t) (d >> 32)), U, 1)

// End multiply definitions

// Begin load/store definitions

DEFINE_LOAD_STORE_INSTRUCTION_ARM(LDR, LOAD, cpu->gprs[rd] = cpu->memory.load32(cpu, address, &currentCycles); ARM_LOAD_POST_BODY;)
DEFINE_LOAD_STORE_INSTRUCTION_ARM(LDRB, LOAD, cpu->gprs[rd] = cpu->memory.load8(cpu, address, &currentCycles); ARM_LOAD_POST_BODY;)
DEFINE_LOAD_STORE_MODE_3_INSTRUCTION_ARM(LDRH, LOAD, cpu->gprs[rd] = cpu->memory.load16(cpu, address, &currentCycles); ARM_LOAD_POST_BODY;)
DEFINE_LOAD_STORE_MODE_3_INSTRUCTION_ARM(LDRSB, LOAD, cpu->gprs[rd] = ARM_SXT_8(cpu->memory.load8(cpu, address, &currentCycles)); ARM_LOAD_POST_BODY;)
// Upstream sign-extends a byte when the address is odd, which is ARM7TDMI's
// unaligned behaviour. ARMv6 loads the halfword at the address as given.
DEFINE_LOAD_STORE_MODE_3_INSTRUCTION_ARM(LDRSH, LOAD, cpu->gprs[rd] = ARM_SXT_16(cpu->memory.load16(cpu, address, &currentCycles)); ARM_LOAD_POST_BODY;)
DEFINE_LOAD_STORE_INSTRUCTION_ARM(STR, STORE, cpu->memory.store32(cpu, address, d, &currentCycles); ARM_STORE_POST_BODY;)
DEFINE_LOAD_STORE_INSTRUCTION_ARM(STRB, STORE, cpu->memory.store8(cpu, address, d, &currentCycles); ARM_STORE_POST_BODY;)
DEFINE_LOAD_STORE_MODE_3_INSTRUCTION_ARM(STRH, STORE, cpu->memory.store16(cpu, address, d, &currentCycles); ARM_STORE_POST_BODY;)

// ARMv5 doubleword transfers. Rd must be even (an odd one is rejected, see
// ARMStrictCheck) and Rd+1 is the second register. Rd == LR makes Rd+1 the
// PC, which is UNPREDICTABLE: the PC is loaded as a plain branch,
// or stored as the instruction plus 8. Writeback comes after both
// loads, which matters when the base is one of them.
// The writeback address is worked out here, from the registers as they were,
// because the loads may overwrite the base or the index.
#define ARM_LDRD_BODY \
	uint32_t offset = (opcode & 0x00400000) ? (((opcode >> 4) & 0xF0) | (opcode & 0xF)) \
	                                        : (uint32_t) cpu->gprs[rm]; \
	uint32_t updated = (opcode & 0x00800000) ? (uint32_t) cpu->gprs[rn] + offset \
	                                         : (uint32_t) cpu->gprs[rn] - offset; \
	bool writeback = !(opcode & 0x01000000) || (opcode & 0x00200000); \
	int32_t lo = cpu->memory.load32(cpu, address, &currentCycles); \
	int32_t hi = cpu->memory.load32(cpu, address + 4, &currentCycles); \
	cpu->gprs[rd] = lo; \
	if (rd + 1 == ARM_PC) { \
		ARMWritePCUnpredictable(cpu, hi); \
	} else { \
		cpu->gprs[(rd + 1) & 0xF] = hi; \
	} \
	if (writeback) { \
		if (rn == ARM_PC) { \
			ARMWritePCUnpredictable(cpu, updated); \
		} else { \
			cpu->gprs[rn] = updated; \
		} \
	} \
	currentCycles += cpu->memory.activeNonseqCycles32 - cpu->memory.activeSeqCycles32;
DEFINE_LOAD_STORE_MODE_3_INSTRUCTION_ARM(LDRD, LOADD, ARM_LDRD_BODY)
DEFINE_LOAD_STORE_MODE_3_INSTRUCTION_ARM(STRD, STORE,
	cpu->memory.store32(cpu, address, d, &currentCycles);
	cpu->memory.store32(cpu, address + 4, cpu->gprs[(rd + 1) & 0xF], &currentCycles);
	ARM_STORE_POST_BODY;)

DEFINE_LOAD_STORE_T_INSTRUCTION_ARM(LDRBT, LOAD,
	enum PrivilegeMode priv = cpu->privilegeMode;
	ARMSetPrivilegeMode(cpu, MODE_USER);
	int32_t r = cpu->memory.load8(cpu, address, &currentCycles);
	ARMSetPrivilegeMode(cpu, priv);
	cpu->gprs[rd] = r;
	ARM_LOAD_POST_BODY;)

DEFINE_LOAD_STORE_T_INSTRUCTION_ARM(LDRT, LOAD,
	enum PrivilegeMode priv = cpu->privilegeMode;
	ARMSetPrivilegeMode(cpu, MODE_USER);
	int32_t r = cpu->memory.load32(cpu, address, &currentCycles);
	ARMSetPrivilegeMode(cpu, priv);
	cpu->gprs[rd] = r;
	ARM_LOAD_POST_BODY;)

DEFINE_LOAD_STORE_T_INSTRUCTION_ARM(STRBT, STORE,
	enum PrivilegeMode priv = cpu->privilegeMode;
	int32_t r = cpu->gprs[rd];
	ARMSetPrivilegeMode(cpu, MODE_USER);
	cpu->memory.store8(cpu, address, r, &currentCycles);
	ARMSetPrivilegeMode(cpu, priv);
	ARM_STORE_POST_BODY;)

DEFINE_LOAD_STORE_T_INSTRUCTION_ARM(STRT, STORE,
	enum PrivilegeMode priv = cpu->privilegeMode;
	int32_t r = cpu->gprs[rd];
	ARMSetPrivilegeMode(cpu, MODE_USER);
	cpu->memory.store32(cpu, address, r, &currentCycles);
	ARMSetPrivilegeMode(cpu, priv);
	ARM_STORE_POST_BODY;)

DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_ARM(LDM,
	load,
	currentCycles += cpu->memory.activeNonseqCycles32 - cpu->memory.activeSeqCycles32;
	if ((rs & 0x8000) || !rs) {
		if (!(opcode & 0x00400000)) {
			// A load to the PC, so it interworks from ARMv5T.
			currentCycles += ARMInterworkWritePC(cpu);
		} else if (cpu->executionMode == MODE_THUMB) {
			// LDM^ with the PC is an exception return, and the state
			// comes from the SPSR it has just restored instead.
			currentCycles += ThumbWritePC(cpu);
		} else {
			currentCycles += ARMWritePC(cpu);
		}
	})

DEFINE_LOAD_STORE_MULTIPLE_INSTRUCTION_ARM(STM,
	store,
	ARM_STORE_POST_BODY;)

// The ARMv6 exclusive monitor (see below) is cleared here. Spurious clearing
// is always architecturally permitted, and software is required to keep SWP
// out from between a LDREX/STREX pair in any case, so this only makes a
// difference to code that was already wrong -- but it is what real ARM11
// hardware does.
DEFINE_INSTRUCTION_ARM(SWP,
	int rm = opcode & 0xF;
	int rd = (opcode >> 12) & 0xF;
	int rn = (opcode >> 16) & 0xF;
	int32_t d = cpu->memory.load32(cpu, cpu->gprs[rn], &currentCycles);
	cpu->memory.store32(cpu, cpu->gprs[rn], cpu->gprs[rm], &currentCycles);
	cpu->gprs[rd] = d;
	cpu->exclusiveMonitor = false;)

DEFINE_INSTRUCTION_ARM(SWPB,
	int rm = opcode & 0xF;
	int rd = (opcode >> 12) & 0xF;
	int rn = (opcode >> 16) & 0xF;
	int32_t d = cpu->memory.load8(cpu, cpu->gprs[rn], &currentCycles);
	cpu->memory.store8(cpu, cpu->gprs[rn], cpu->gprs[rm], &currentCycles);
	cpu->gprs[rd] = d;
	cpu->exclusiveMonitor = false;)

// End load/store definitions

// Begin branch definitions

DEFINE_INSTRUCTION_ARM(B,
	int32_t offset = opcode << 8;
	offset >>= 6;
	cpu->gprs[ARM_PC] += offset;
	currentCycles += ARMWritePC(cpu);)

DEFINE_INSTRUCTION_ARM(BL,
	int32_t immediate = (opcode & 0x00FFFFFF) << 8;
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC] - WORD_SIZE_ARM;
	cpu->gprs[ARM_PC] += immediate >> 6;
	currentCycles += ARMWritePC(cpu);)

DEFINE_INSTRUCTION_ARM(BX,
	int rm = opcode & 0x0000000F;
	_ARMSetMode(cpu, cpu->gprs[rm] & 0x00000001);
	cpu->gprs[ARM_PC] = cpu->gprs[rm] & 0xFFFFFFFE;
	if (cpu->executionMode == MODE_THUMB) {
		currentCycles += ThumbWritePC(cpu);
	} else {
		currentCycles += ARMWritePC(cpu);
	})

// ARMv5 additions (mGBA targets ARM7TDMI/ARMv4T); see README.md.

// The instructions added below write their results straight to gprs[]. A
// destination of r15 is unpredictable for all of them; it is taken as a
// plain branch to the result (see DEFINE_INSTRUCTION_ARM). The exclusive
// stores, whose status register may not be the PC, are undefined instead.
#define ARM_PC_DEST(R) pcDest = pcDest || (R) == ARM_PC;

#define ARM_ILL_IF_PC(R)                                                       \
	if (UNLIKELY((R) == ARM_PC)) {                                             \
		ARM_ILL;                                                               \
		return;                                                                \
	}

DEFINE_INSTRUCTION_ARM(BLX_R,
	int rm = opcode & 0x0000000F;
	int32_t target = cpu->gprs[rm];
	// PC is two instructions ahead, so the return address is PC - 4.
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC] - WORD_SIZE_ARM;
	_ARMSetMode(cpu, target & 0x00000001);
	cpu->gprs[ARM_PC] = target & 0xFFFFFFFE;
	if (cpu->executionMode == MODE_THUMB) {
		currentCycles += ThumbWritePC(cpu);
	} else {
		currentCycles += ARMWritePC(cpu);
	})

DEFINE_INSTRUCTION_ARM(BLX_I,
	// Unconditional encoding: the H bit supplies a halfword of the offset,
	// and the target is always Thumb.
	int32_t offset = (((int32_t) opcode << 8) >> 6) | ((opcode >> 23) & 2);
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC] - WORD_SIZE_ARM;
	_ARMSetMode(cpu, MODE_THUMB);
	cpu->gprs[ARM_PC] += offset;
	currentCycles += ThumbWritePC(cpu);)

DEFINE_INSTRUCTION_ARM(CLZ,
	int rd = (opcode >> 12) & 0xF;
	ARM_PC_DEST(rd);
	int rm = opcode & 0xF;
	uint32_t value = cpu->gprs[rm];
	cpu->gprs[rd] = value ? __builtin_clz(value) : 32;)

// ARMv5TE saturating arithmetic and DSP multiplies. Note Rd sits at bits
// 19:16 in the multiply encodings, not 15:12.

// Neither the Q flag nor the ARMv6 GE flags have a named field in union PSR
// -- both fall inside its 20-bit `unused` run -- so they are reached through
// `packed`.
#define ARM_Q_BIT (1 << 27)
#define ARM_GE_SHIFT 16
#define ARM_GE_MASK (0xF << ARM_GE_SHIFT)

#define DSP_RS ((opcode >> 8) & 0xF)
#define DSP_RM (opcode & 0xF)
// Bit 5 picks the Rm halfword, bit 6 the Rs halfword; 1 means the top half.
#define DSP_RM_HALF                                                            \
	((int32_t) (int16_t) ((opcode & 0x00000020)                                \
	                          ? (cpu->gprs[DSP_RM] >> 16)                      \
	                          : cpu->gprs[DSP_RM]))
#define DSP_RS_HALF                                                            \
	((int32_t) (int16_t) ((opcode & 0x00000040)                                \
	                          ? (cpu->gprs[DSP_RS] >> 16)                      \
	                          : cpu->gprs[DSP_RS]))

static inline int32_t _ARMSatAdd(struct ARMCore* cpu, int32_t a, int32_t b) {
	int32_t r = (int32_t) ((uint32_t) a + (uint32_t) b);
	if (((a ^ r) & (b ^ r)) < 0) {
		r = (a < 0) ? INT32_MIN : INT32_MAX;
		cpu->cpsr.packed |= ARM_Q_BIT;
	}
	return r;
}

static inline int32_t _ARMSatSub(struct ARMCore* cpu, int32_t a, int32_t b) {
	int32_t r = (int32_t) ((uint32_t) a - (uint32_t) b);
	if (((a ^ b) & (a ^ r)) < 0) {
		r = (a < 0) ? INT32_MIN : INT32_MAX;
		cpu->cpsr.packed |= ARM_Q_BIT;
	}
	return r;
}

// The accumulating multiplies wrap rather than clamp, and only flag the
// overflow in Q.
static inline int32_t _ARMWrapQ(struct ARMCore* cpu, int64_t value) {
	if (value != (int32_t) value) {
		cpu->cpsr.packed |= ARM_Q_BIT;
	}
	return (int32_t) value;
}

#define DEFINE_SATURATING_INSTRUCTION_ARM(NAME, EXPR)                          \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rn = (opcode >> 16) & 0xF;                                         \
		int rd = (opcode >> 12) & 0xF;                                         \
		int rm = opcode & 0xF;                                                 \
		ARM_PC_DEST(rd);                                                     \
		int32_t n = cpu->gprs[rn];                                             \
		int32_t m = cpu->gprs[rm];                                             \
		UNUSED(n);                                                             \
		cpu->gprs[rd] = EXPR;)

DEFINE_SATURATING_INSTRUCTION_ARM(QADD, _ARMSatAdd(cpu, m, n))
DEFINE_SATURATING_INSTRUCTION_ARM(QSUB, _ARMSatSub(cpu, m, n))
DEFINE_SATURATING_INSTRUCTION_ARM(QDADD, _ARMSatAdd(cpu, m, _ARMSatAdd(cpu, n, n)))
DEFINE_SATURATING_INSTRUCTION_ARM(QDSUB, _ARMSatSub(cpu, m, _ARMSatAdd(cpu, n, n)))

DEFINE_INSTRUCTION_ARM(SMULXY,
	int rd = (opcode >> 16) & 0xF;
	ARM_PC_DEST(rd);
	cpu->gprs[rd] = DSP_RM_HALF * DSP_RS_HALF;)

// The accumulating DSP multiplies wrap, and set Q if the sum overflowed;
// unlike QADD, they do not saturate.
DEFINE_INSTRUCTION_ARM(SMLAXY,
	int rd = (opcode >> 16) & 0xF;
	ARM_PC_DEST(rd);
	int rn = (opcode >> 12) & 0xF;
	cpu->gprs[rd] = _ARMWrapQ(cpu, (int64_t) (DSP_RM_HALF * DSP_RS_HALF) + cpu->gprs[rn]);)

DEFINE_INSTRUCTION_ARM(SMULWY,
	int rd = (opcode >> 16) & 0xF;
	ARM_PC_DEST(rd);
	int64_t product = (int64_t) cpu->gprs[DSP_RM] * DSP_RS_HALF;
	cpu->gprs[rd] = (int32_t) (product >> 16);)

DEFINE_INSTRUCTION_ARM(SMLAWY,
	int rd = (opcode >> 16) & 0xF;
	ARM_PC_DEST(rd);
	int rn = (opcode >> 12) & 0xF;
	int64_t product = (int64_t) cpu->gprs[DSP_RM] * DSP_RS_HALF;
	cpu->gprs[rd] = _ARMWrapQ(cpu, (product >> 16) + cpu->gprs[rn]);)

DEFINE_INSTRUCTION_ARM(SMLALXY,
	int rdHi = (opcode >> 16) & 0xF;
	int rdLo = (opcode >> 12) & 0xF;
	ARM_PC_DEST(rdHi);
	ARM_PC_DEST(rdLo);
	int64_t acc = ((int64_t) cpu->gprs[rdHi] << 32) |
		(uint32_t) cpu->gprs[rdLo];
	acc += (int64_t) (DSP_RM_HALF * DSP_RS_HALF);
	cpu->gprs[rdLo] = (int32_t) acc;
	cpu->gprs[rdHi] = (int32_t) (acc >> 32);)

// End ARMv5 additions

// ARMv6 additions. Everything here fills slots that were ILL; see README.md
// for the encoding-space survey.

static inline void _ARMSetGE(struct ARMCore* cpu, unsigned ge) {
	cpu->cpsr.packed = (cpu->cpsr.packed & ~ARM_GE_MASK) |
		(int32_t) (ge << ARM_GE_SHIFT);
}

// Element extraction for the parallel (SIMD) instructions. Results are
// always packed back with an explicit mask, so a signed intermediate is
// fine even where the architecture specifies an unsigned one.
#define ARM_S16_LO(V) ((int32_t) (int16_t) (uint16_t) (uint32_t) (V))
#define ARM_S16_HI(V) ((int32_t) (int16_t) (uint16_t) ((uint32_t) (V) >> 16))
#define ARM_U16_LO(V) ((int32_t) ((uint32_t) (V) & 0xFFFF))
#define ARM_U16_HI(V) ((int32_t) ((uint32_t) (V) >> 16))
#define ARM_S8_N(V, I) ((int32_t) (int8_t) (uint8_t) ((uint32_t) (V) >> ((I) * 8)))
#define ARM_U8_N(V, I) ((int32_t) (((uint32_t) (V) >> ((I) * 8)) & 0xFF))

// SSAT and USAT clamp and set Q; the SIMD saturating instructions clamp
// without touching it. int64_t throughout so that a 32-bit saturation
// bound does not overflow while being computed.
static inline int32_t _ARMSignedSat(int64_t value, int bits) {
	int64_t max = ((int64_t) 1 << (bits - 1)) - 1;
	if (value > max) {
		return (int32_t) max;
	}
	if (value < -max - 1) {
		return (int32_t) (-max - 1);
	}
	return (int32_t) value;
}

static inline int32_t _ARMUnsignedSat(int64_t value, int bits) {
	int64_t max = ((int64_t) 1 << bits) - 1;
	if (value > max) {
		return (int32_t) max;
	}
	if (value < 0) {
		return 0;
	}
	return (int32_t) value;
}

static inline int32_t _ARMSignedSatQ(struct ARMCore* cpu, int64_t value,
                                     int bits) {
	int32_t result = _ARMSignedSat(value, bits);
	if (result != value) {
		cpu->cpsr.packed |= ARM_Q_BIT;
	}
	return result;
}

static inline int32_t _ARMUnsignedSatQ(struct ARMCore* cpu, int64_t value,
                                       int bits) {
	int32_t result = _ARMUnsignedSat(value, bits);
	if (result != value) {
		cpu->cpsr.packed |= ARM_Q_BIT;
	}
	return result;
}

// Parallel addition and subtraction, and the pack/unpack/saturate/reverse
// group: all Rn at 19:16, Rd at 15:12, Rm at 3:0.
#define DEFINE_PARALLEL_INSTRUCTION_ARM(NAME, BODY)                            \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rn = (opcode >> 16) & 0xF;                                         \
		int rd = (opcode >> 12) & 0xF;                                         \
		int rm = opcode & 0xF;                                                 \
		ARM_PC_DEST(rd);                                                     \
		int32_t n ATTRIBUTE_UNUSED = cpu->gprs[rn];                            \
		int32_t m ATTRIBUTE_UNUSED = cpu->gprs[rm];                            \
		BODY;)

// GELO and GEHI are predicates over `lo` and `hi`, which hold the
// untruncated element results, so a signed test reads the carry or borrow
// that the architecture asks for.
#define ARM_PAR_PACK_16(LO, HI, GELO, GEHI)                                    \
	{                                                                          \
		int32_t lo = (LO);                                                     \
		int32_t hi = (HI);                                                     \
		cpu->gprs[rd] = (int32_t) (((uint32_t) lo & 0xFFFF) |                  \
			((uint32_t) hi << 16));                                            \
		_ARMSetGE(cpu, ((GELO) ? 0x3 : 0) | ((GEHI) ? 0xC : 0));               \
	}

#define ARM_PAR_PACK_16_NO_GE(LO, HI)                                          \
	cpu->gprs[rd] = (int32_t) (((uint32_t) (LO) & 0xFFFF) |                    \
		((uint32_t) (HI) << 16));

// EXPR is evaluated per byte with `i` in scope; GE is a predicate over `e`.
#define ARM_PAR_PACK_8(EXPR, GE)                                               \
	{                                                                          \
		uint32_t result = 0;                                                   \
		unsigned ge = 0;                                                       \
		int i;                                                                 \
		for (i = 0; i < 4; ++i) {                                              \
			int32_t e = (EXPR);                                                \
			result |= ((uint32_t) e & 0xFF) << (i * 8);                        \
			if (GE) {                                                          \
				ge |= 1 << i;                                                  \
			}                                                                  \
		}                                                                      \
		cpu->gprs[rd] = (int32_t) result;                                      \
		_ARMSetGE(cpu, ge);                                                    \
	}

#define ARM_PAR_PACK_8_NO_GE(EXPR)                                             \
	{                                                                          \
		uint32_t result = 0;                                                   \
		int i;                                                                 \
		for (i = 0; i < 4; ++i) {                                              \
			result |= ((uint32_t) (EXPR) & 0xFF) << (i * 8);                   \
		}                                                                      \
		cpu->gprs[rd] = (int32_t) result;                                      \
	}

// Signed, modulo. These and the unsigned forms below are the only parallel
// instructions that write GE.
DEFINE_PARALLEL_INSTRUCTION_ARM(SADD16,
	ARM_PAR_PACK_16(ARM_S16_LO(n) + ARM_S16_LO(m),
		ARM_S16_HI(n) + ARM_S16_HI(m), lo >= 0, hi >= 0))
DEFINE_PARALLEL_INSTRUCTION_ARM(SASX,
	ARM_PAR_PACK_16(ARM_S16_LO(n) - ARM_S16_HI(m),
		ARM_S16_HI(n) + ARM_S16_LO(m), lo >= 0, hi >= 0))
DEFINE_PARALLEL_INSTRUCTION_ARM(SSAX,
	ARM_PAR_PACK_16(ARM_S16_LO(n) + ARM_S16_HI(m),
		ARM_S16_HI(n) - ARM_S16_LO(m), lo >= 0, hi >= 0))
DEFINE_PARALLEL_INSTRUCTION_ARM(SSUB16,
	ARM_PAR_PACK_16(ARM_S16_LO(n) - ARM_S16_LO(m),
		ARM_S16_HI(n) - ARM_S16_HI(m), lo >= 0, hi >= 0))
DEFINE_PARALLEL_INSTRUCTION_ARM(SADD8,
	ARM_PAR_PACK_8(ARM_S8_N(n, i) + ARM_S8_N(m, i), e >= 0))
DEFINE_PARALLEL_INSTRUCTION_ARM(SSUB8,
	ARM_PAR_PACK_8(ARM_S8_N(n, i) - ARM_S8_N(m, i), e >= 0))

// Signed saturating. Note these do not set Q.
DEFINE_PARALLEL_INSTRUCTION_ARM(QADD16,
	ARM_PAR_PACK_16_NO_GE(_ARMSignedSat(ARM_S16_LO(n) + ARM_S16_LO(m), 16),
		_ARMSignedSat(ARM_S16_HI(n) + ARM_S16_HI(m), 16)))
DEFINE_PARALLEL_INSTRUCTION_ARM(QASX,
	ARM_PAR_PACK_16_NO_GE(_ARMSignedSat(ARM_S16_LO(n) - ARM_S16_HI(m), 16),
		_ARMSignedSat(ARM_S16_HI(n) + ARM_S16_LO(m), 16)))
DEFINE_PARALLEL_INSTRUCTION_ARM(QSAX,
	ARM_PAR_PACK_16_NO_GE(_ARMSignedSat(ARM_S16_LO(n) + ARM_S16_HI(m), 16),
		_ARMSignedSat(ARM_S16_HI(n) - ARM_S16_LO(m), 16)))
DEFINE_PARALLEL_INSTRUCTION_ARM(QSUB16,
	ARM_PAR_PACK_16_NO_GE(_ARMSignedSat(ARM_S16_LO(n) - ARM_S16_LO(m), 16),
		_ARMSignedSat(ARM_S16_HI(n) - ARM_S16_HI(m), 16)))
DEFINE_PARALLEL_INSTRUCTION_ARM(QADD8,
	ARM_PAR_PACK_8_NO_GE(_ARMSignedSat(ARM_S8_N(n, i) + ARM_S8_N(m, i), 8)))
DEFINE_PARALLEL_INSTRUCTION_ARM(QSUB8,
	ARM_PAR_PACK_8_NO_GE(_ARMSignedSat(ARM_S8_N(n, i) - ARM_S8_N(m, i), 8)))

// Signed halving. The result is bit 16:1 (or 8:1) of the element, which an
// arithmetic shift gives correctly once the pack masks off the rest.
DEFINE_PARALLEL_INSTRUCTION_ARM(SHADD16,
	ARM_PAR_PACK_16_NO_GE((ARM_S16_LO(n) + ARM_S16_LO(m)) >> 1,
		(ARM_S16_HI(n) + ARM_S16_HI(m)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(SHASX,
	ARM_PAR_PACK_16_NO_GE((ARM_S16_LO(n) - ARM_S16_HI(m)) >> 1,
		(ARM_S16_HI(n) + ARM_S16_LO(m)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(SHSAX,
	ARM_PAR_PACK_16_NO_GE((ARM_S16_LO(n) + ARM_S16_HI(m)) >> 1,
		(ARM_S16_HI(n) - ARM_S16_LO(m)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(SHSUB16,
	ARM_PAR_PACK_16_NO_GE((ARM_S16_LO(n) - ARM_S16_LO(m)) >> 1,
		(ARM_S16_HI(n) - ARM_S16_HI(m)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(SHADD8,
	ARM_PAR_PACK_8_NO_GE((ARM_S8_N(n, i) + ARM_S8_N(m, i)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(SHSUB8,
	ARM_PAR_PACK_8_NO_GE((ARM_S8_N(n, i) - ARM_S8_N(m, i)) >> 1))

// Unsigned, modulo. GE is the carry out of each sum, or the absence of a
// borrow from each difference.
DEFINE_PARALLEL_INSTRUCTION_ARM(UADD16,
	ARM_PAR_PACK_16(ARM_U16_LO(n) + ARM_U16_LO(m),
		ARM_U16_HI(n) + ARM_U16_HI(m), lo >= 0x10000, hi >= 0x10000))
DEFINE_PARALLEL_INSTRUCTION_ARM(UASX,
	ARM_PAR_PACK_16(ARM_U16_LO(n) - ARM_U16_HI(m),
		ARM_U16_HI(n) + ARM_U16_LO(m), lo >= 0, hi >= 0x10000))
DEFINE_PARALLEL_INSTRUCTION_ARM(USAX,
	ARM_PAR_PACK_16(ARM_U16_LO(n) + ARM_U16_HI(m),
		ARM_U16_HI(n) - ARM_U16_LO(m), lo >= 0x10000, hi >= 0))
DEFINE_PARALLEL_INSTRUCTION_ARM(USUB16,
	ARM_PAR_PACK_16(ARM_U16_LO(n) - ARM_U16_LO(m),
		ARM_U16_HI(n) - ARM_U16_HI(m), lo >= 0, hi >= 0))
DEFINE_PARALLEL_INSTRUCTION_ARM(UADD8,
	ARM_PAR_PACK_8(ARM_U8_N(n, i) + ARM_U8_N(m, i), e >= 0x100))
DEFINE_PARALLEL_INSTRUCTION_ARM(USUB8,
	ARM_PAR_PACK_8(ARM_U8_N(n, i) - ARM_U8_N(m, i), e >= 0))

// Unsigned saturating.
DEFINE_PARALLEL_INSTRUCTION_ARM(UQADD16,
	ARM_PAR_PACK_16_NO_GE(_ARMUnsignedSat(ARM_U16_LO(n) + ARM_U16_LO(m), 16),
		_ARMUnsignedSat(ARM_U16_HI(n) + ARM_U16_HI(m), 16)))
DEFINE_PARALLEL_INSTRUCTION_ARM(UQASX,
	ARM_PAR_PACK_16_NO_GE(_ARMUnsignedSat(ARM_U16_LO(n) - ARM_U16_HI(m), 16),
		_ARMUnsignedSat(ARM_U16_HI(n) + ARM_U16_LO(m), 16)))
DEFINE_PARALLEL_INSTRUCTION_ARM(UQSAX,
	ARM_PAR_PACK_16_NO_GE(_ARMUnsignedSat(ARM_U16_LO(n) + ARM_U16_HI(m), 16),
		_ARMUnsignedSat(ARM_U16_HI(n) - ARM_U16_LO(m), 16)))
DEFINE_PARALLEL_INSTRUCTION_ARM(UQSUB16,
	ARM_PAR_PACK_16_NO_GE(_ARMUnsignedSat(ARM_U16_LO(n) - ARM_U16_LO(m), 16),
		_ARMUnsignedSat(ARM_U16_HI(n) - ARM_U16_HI(m), 16)))
DEFINE_PARALLEL_INSTRUCTION_ARM(UQADD8,
	ARM_PAR_PACK_8_NO_GE(_ARMUnsignedSat(ARM_U8_N(n, i) + ARM_U8_N(m, i), 8)))
DEFINE_PARALLEL_INSTRUCTION_ARM(UQSUB8,
	ARM_PAR_PACK_8_NO_GE(_ARMUnsignedSat(ARM_U8_N(n, i) - ARM_U8_N(m, i), 8)))

// Unsigned halving.
DEFINE_PARALLEL_INSTRUCTION_ARM(UHADD16,
	ARM_PAR_PACK_16_NO_GE((ARM_U16_LO(n) + ARM_U16_LO(m)) >> 1,
		(ARM_U16_HI(n) + ARM_U16_HI(m)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(UHASX,
	ARM_PAR_PACK_16_NO_GE((ARM_U16_LO(n) - ARM_U16_HI(m)) >> 1,
		(ARM_U16_HI(n) + ARM_U16_LO(m)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(UHSAX,
	ARM_PAR_PACK_16_NO_GE((ARM_U16_LO(n) + ARM_U16_HI(m)) >> 1,
		(ARM_U16_HI(n) - ARM_U16_LO(m)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(UHSUB16,
	ARM_PAR_PACK_16_NO_GE((ARM_U16_LO(n) - ARM_U16_LO(m)) >> 1,
		(ARM_U16_HI(n) - ARM_U16_HI(m)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(UHADD8,
	ARM_PAR_PACK_8_NO_GE((ARM_U8_N(n, i) + ARM_U8_N(m, i)) >> 1))
DEFINE_PARALLEL_INSTRUCTION_ARM(UHSUB8,
	ARM_PAR_PACK_8_NO_GE((ARM_U8_N(n, i) - ARM_U8_N(m, i)) >> 1))

// Halfword packing. imm5 == 0 encodes ASR #32 for PKHTB.
DEFINE_PARALLEL_INSTRUCTION_ARM(PKHBT,
	unsigned shift = (opcode >> 7) & 0x1F;
	cpu->gprs[rd] = (int32_t) (((uint32_t) n & 0xFFFF) |
		(((uint32_t) m << shift) & 0xFFFF0000));)

DEFINE_PARALLEL_INSTRUCTION_ARM(PKHTB,
	unsigned shift = (opcode >> 7) & 0x1F;
	int32_t shifted = shift ? (m >> shift) : (m >> 31);
	cpu->gprs[rd] = (int32_t) (((uint32_t) shifted & 0xFFFF) |
		((uint32_t) n & 0xFFFF0000));)

// Byte-wise select, driven by the GE flags the parallel instructions write.
DEFINE_PARALLEL_INSTRUCTION_ARM(SEL,
	unsigned ge = ((unsigned) cpu->cpsr.packed & ARM_GE_MASK) >> ARM_GE_SHIFT;
	uint32_t result = 0;
	int i;
	for (i = 0; i < 4; ++i) {
		uint32_t byte = ((ge >> i) & 1) ? ARM_U8_N(n, i) : ARM_U8_N(m, i);
		result |= byte << (i * 8);
	}
	cpu->gprs[rd] = (int32_t) result;)

// Byte reversal. Rn is unused in these encodings (it reads as 1111).
DEFINE_PARALLEL_INSTRUCTION_ARM(REV,
	cpu->gprs[rd] = (int32_t) __builtin_bswap32((uint32_t) m);)

DEFINE_PARALLEL_INSTRUCTION_ARM(REV16,
	uint32_t v = (uint32_t) m;
	cpu->gprs[rd] =
		(int32_t) (((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF));)

DEFINE_PARALLEL_INSTRUCTION_ARM(REVSH,
	uint32_t v = (uint32_t) m;
	cpu->gprs[rd] =
		(int32_t) (int16_t) (uint16_t) (((v & 0xFF) << 8) | ((v >> 8) & 0xFF));)

// Saturation. Bit 6 picks LSL or ASR for the pre-shift, and ASR #0 encodes
// ASR #32. SSAT saturates to sat_imm + 1 bits, USAT to sat_imm.
#define DEFINE_SATURATE_INSTRUCTION_ARM(NAME, SAT, BITS)                       \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rd = (opcode >> 12) & 0xF;                                         \
		int rm = opcode & 0xF;                                                 \
		ARM_PC_DEST(rd);                                                     \
		int bits = (BITS);                                                     \
		unsigned shift = (opcode >> 7) & 0x1F;                                 \
		int32_t m = cpu->gprs[rm];                                             \
		if (opcode & 0x00000040) {                                             \
			m = shift ? (m >> shift) : (m >> 31);                              \
		} else {                                                               \
			m = (int32_t) ((uint32_t) m << shift);                             \
		}                                                                      \
		cpu->gprs[rd] = SAT(cpu, m, bits);)

DEFINE_SATURATE_INSTRUCTION_ARM(SSAT, _ARMSignedSatQ,
	(((opcode >> 16) & 0x1F) + 1))
DEFINE_SATURATE_INSTRUCTION_ARM(USAT, _ARMUnsignedSatQ,
	((opcode >> 16) & 0x1F))

// The halfword forms take signed halfwords of Rm even when saturating to an
// unsigned range.
#define DEFINE_SATURATE_16_INSTRUCTION_ARM(NAME, SAT, BITS)                    \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rd = (opcode >> 12) & 0xF;                                         \
		int rm = opcode & 0xF;                                                 \
		ARM_PC_DEST(rd);                                                     \
		int bits = (BITS);                                                     \
		int32_t m = cpu->gprs[rm];                                             \
		int32_t lo = SAT(cpu, ARM_S16_LO(m), bits);                            \
		int32_t hi = SAT(cpu, ARM_S16_HI(m), bits);                            \
		cpu->gprs[rd] = (int32_t) (((uint32_t) lo & 0xFFFF) |                  \
			((uint32_t) hi << 16));)

DEFINE_SATURATE_16_INSTRUCTION_ARM(SSAT16, _ARMSignedSatQ,
	(((opcode >> 16) & 0xF) + 1))
DEFINE_SATURATE_16_INSTRUCTION_ARM(USAT16, _ARMUnsignedSatQ,
	((opcode >> 16) & 0xF))

// Extend and optionally accumulate. Bits 11:10 rotate Rm right by 0, 8, 16
// or 24 first, and Rn == 15 picks the plain (non-accumulating) form.
#define DEFINE_EXTEND_INSTRUCTION_ARM(NAME, BODY)                              \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rn = (opcode >> 16) & 0xF;                                         \
		int rd = (opcode >> 12) & 0xF;                                         \
		int rm = opcode & 0xF;                                                 \
		ARM_PC_DEST(rd);                                                     \
		int32_t n = rn == ARM_PC ? 0 : cpu->gprs[rn];                          \
		uint32_t m = ROR(cpu->gprs[rm], (((opcode >> 10) & 3) * 8));           \
		BODY;)

DEFINE_EXTEND_INSTRUCTION_ARM(SXTAB,
	cpu->gprs[rd] = n + (int32_t) (int8_t) (uint8_t) m;)
DEFINE_EXTEND_INSTRUCTION_ARM(SXTAH,
	cpu->gprs[rd] = n + (int32_t) (int16_t) (uint16_t) m;)
DEFINE_EXTEND_INSTRUCTION_ARM(UXTAB, cpu->gprs[rd] = n + (int32_t) (m & 0xFF);)
DEFINE_EXTEND_INSTRUCTION_ARM(UXTAH,
	cpu->gprs[rd] = n + (int32_t) (m & 0xFFFF);)

// The 16-bit forms extend two bytes into two halfwords and accumulate
// halfword-wise, so the sign of the Rn halves is irrelevant.
DEFINE_EXTEND_INSTRUCTION_ARM(SXTAB16,
	int32_t lo = ARM_S16_LO(n) + (int32_t) (int8_t) (uint8_t) m;
	int32_t hi = ARM_S16_HI(n) + (int32_t) (int8_t) (uint8_t) (m >> 16);
	cpu->gprs[rd] = (int32_t) (((uint32_t) lo & 0xFFFF) |
		((uint32_t) hi << 16));)

DEFINE_EXTEND_INSTRUCTION_ARM(UXTAB16,
	int32_t lo = ARM_S16_LO(n) + (int32_t) (m & 0xFF);
	int32_t hi = ARM_S16_HI(n) + (int32_t) ((m >> 16) & 0xFF);
	cpu->gprs[rd] = (int32_t) (((uint32_t) lo & 0xFFFF) |
		((uint32_t) hi << 16));)

// Dual halfword multiplies. Rd (or RdHi) is at 19:16 and Ra (or RdLo) at
// 15:12; bit 5 swaps the second operand's halfwords, giving the X forms.
#define DEFINE_DUAL_MULTIPLY_INSTRUCTION_ARM(NAME, BODY)                       \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rd = (opcode >> 16) & 0xF;                                         \
		int ra = (opcode >> 12) & 0xF;                                         \
		ARM_PC_DEST(rd);                                                     \
		int32_t n = cpu->gprs[opcode & 0xF];                                   \
		int32_t m = (opcode & 0x00000020)                                      \
			? (int32_t) ROR(cpu->gprs[(opcode >> 8) & 0xF], 16)                \
			: cpu->gprs[(opcode >> 8) & 0xF];                                  \
		int32_t p1 = ARM_S16_LO(n) * ARM_S16_LO(m);                            \
		int32_t p2 = ARM_S16_HI(n) * ARM_S16_HI(m);                            \
		BODY;)

// Ra == 15 is SMUAD, with no accumulate; likewise SMUSD below.
DEFINE_DUAL_MULTIPLY_INSTRUCTION_ARM(SMLAD,
	int64_t result = (int64_t) p1 + p2;
	if (ra != ARM_PC) {
		result += cpu->gprs[ra];
	}
	cpu->gprs[rd] = _ARMWrapQ(cpu, result);)

DEFINE_DUAL_MULTIPLY_INSTRUCTION_ARM(SMLSD,
	int64_t result = (int64_t) p1 - p2;
	if (ra != ARM_PC) {
		result += cpu->gprs[ra];
	}
	cpu->gprs[rd] = _ARMWrapQ(cpu, result);)

// Here the two register fields are RdHi and RdLo, and there is no Q.
DEFINE_DUAL_MULTIPLY_INSTRUCTION_ARM(SMLALD,
	ARM_PC_DEST(ra);
	int64_t acc = ((int64_t) cpu->gprs[rd] << 32) | (uint32_t) cpu->gprs[ra];
	acc += (int64_t) p1 + p2;
	cpu->gprs[ra] = (int32_t) acc;
	cpu->gprs[rd] = (int32_t) (acc >> 32);)

DEFINE_DUAL_MULTIPLY_INSTRUCTION_ARM(SMLSLD,
	ARM_PC_DEST(ra);
	int64_t acc = ((int64_t) cpu->gprs[rd] << 32) | (uint32_t) cpu->gprs[ra];
	acc += (int64_t) p1 - p2;
	cpu->gprs[ra] = (int32_t) acc;
	cpu->gprs[rd] = (int32_t) (acc >> 32);)

// Most-significant-word multiplies. Bit 5 asks for the result to be
// rounded rather than truncated.
#define DEFINE_MOST_SIGNIFICANT_MULTIPLY_INSTRUCTION_ARM(NAME, BODY)           \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rd = (opcode >> 16) & 0xF;                                         \
		int ra = (opcode >> 12) & 0xF;                                         \
		ARM_PC_DEST(rd);                                                     \
		int64_t n = cpu->gprs[opcode & 0xF];                                   \
		int64_t m = cpu->gprs[(opcode >> 8) & 0xF];                            \
		int64_t round = (opcode & 0x00000020) ? 0x80000000LL : 0;              \
		BODY;)

// Ra == 15 is SMMUL. SMMLS has no such form.
DEFINE_MOST_SIGNIFICANT_MULTIPLY_INSTRUCTION_ARM(SMMLA,
	int64_t result = n * m + round;
	if (ra != ARM_PC) {
		result += (int64_t) cpu->gprs[ra] << 32;
	}
	cpu->gprs[rd] = (int32_t) (result >> 32);)

// Ra == 15 is UNPREDICTABLE for SMMLS; it is taken as SMMUL.
DEFINE_MOST_SIGNIFICANT_MULTIPLY_INSTRUCTION_ARM(SMMLS,
	int64_t result = ra == ARM_PC ? n * m + round
	                              : ((int64_t) cpu->gprs[ra] << 32) - n * m + round;
	cpu->gprs[rd] = (int32_t) (result >> 32);)

// Sum of absolute differences. Ra == 15 is USAD8, without the accumulate.
DEFINE_INSTRUCTION_ARM(USADA8,
	int rd = (opcode >> 16) & 0xF;
	ARM_PC_DEST(rd);
	int ra = (opcode >> 12) & 0xF;
	int32_t n = cpu->gprs[opcode & 0xF];
	int32_t m = cpu->gprs[(opcode >> 8) & 0xF];
	uint32_t sum = ra == ARM_PC ? 0 : (uint32_t) cpu->gprs[ra];
	int i;
	for (i = 0; i < 4; ++i) {
		int32_t diff = ARM_U8_N(n, i) - ARM_U8_N(m, i);
		sum += (uint32_t) (diff < 0 ? -diff : diff);
	}
	cpu->gprs[rd] = (int32_t) sum;)

DEFINE_INSTRUCTION_ARM(UMAAL,
	int rdHi = (opcode >> 16) & 0xF;
	int rdLo = (opcode >> 12) & 0xF;
	int rs = (opcode >> 8) & 0xF;
	int rm = opcode & 0xF;
	ARM_PC_DEST(rdHi);
	ARM_PC_DEST(rdLo);
	uint64_t d = ARM_UXT_64(cpu->gprs[rm]) * ARM_UXT_64(cpu->gprs[rs]) +
		ARM_UXT_64(cpu->gprs[rdLo]) + ARM_UXT_64(cpu->gprs[rdHi]);
	cpu->gprs[rdLo] = (int32_t) d;
	cpu->gprs[rdHi] = (int32_t) (d >> 32);)

// Synchronisation primitives. touchHLE's guest threads are scheduled onto a
// single emulated core, so nothing can steal the monitor between a load and
// its store; tracking the address is only enough to reject unpaired uses.
#define DEFINE_LOAD_EXCLUSIVE_INSTRUCTION_ARM(NAME, BODY)                      \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rn = (opcode >> 16) & 0xF;                                         \
		int rd = (opcode >> 12) & 0xF;                                         \
		uint32_t address = cpu->gprs[rn];                                      \
		cpu->exclusiveAddress = address;                                       \
		cpu->exclusiveMonitor = true;                                          \
		BODY;                                                                  \
		ARM_LOAD_POST_BODY;)

DEFINE_LOAD_EXCLUSIVE_INSTRUCTION_ARM(LDREX,
	cpu->gprs[rd] = cpu->memory.load32(cpu, address, &currentCycles);)
DEFINE_LOAD_EXCLUSIVE_INSTRUCTION_ARM(LDREXB,
	cpu->gprs[rd] = cpu->memory.load8(cpu, address, &currentCycles);)
DEFINE_LOAD_EXCLUSIVE_INSTRUCTION_ARM(LDREXH,
	cpu->gprs[rd] = cpu->memory.load16(cpu, address, &currentCycles);)
DEFINE_LOAD_EXCLUSIVE_INSTRUCTION_ARM(LDREXD,
	cpu->gprs[rd] = cpu->memory.load32(cpu, address, &currentCycles);
	if (rd + 1 < ARM_PC) {
		cpu->gprs[rd + 1] = cpu->memory.load32(cpu, address + 4,
			&currentCycles);
	})

// Rd takes 0 on success and 1 on failure, and the monitor is cleared either
// way.
#define DEFINE_STORE_EXCLUSIVE_INSTRUCTION_ARM(NAME, BODY)                     \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rn = (opcode >> 16) & 0xF;                                         \
		int rd = (opcode >> 12) & 0xF;                                         \
		int rm = opcode & 0xF;                                                 \
		ARM_ILL_IF_PC(rd);                                                     \
		uint32_t address = cpu->gprs[rn];                                      \
		int32_t d ATTRIBUTE_UNUSED = cpu->gprs[rm];                            \
		if (cpu->exclusiveMonitor && cpu->exclusiveAddress == address) {       \
			BODY;                                                              \
			cpu->gprs[rd] = 0;                                                 \
		} else {                                                               \
			cpu->gprs[rd] = 1;                                                 \
		}                                                                      \
		cpu->exclusiveMonitor = false;                                         \
		ARM_STORE_POST_BODY;)

DEFINE_STORE_EXCLUSIVE_INSTRUCTION_ARM(STREX,
	cpu->memory.store32(cpu, address, d, &currentCycles);)
DEFINE_STORE_EXCLUSIVE_INSTRUCTION_ARM(STREXB,
	cpu->memory.store8(cpu, address, d, &currentCycles);)
DEFINE_STORE_EXCLUSIVE_INSTRUCTION_ARM(STREXH,
	cpu->memory.store16(cpu, address, d, &currentCycles);)
DEFINE_STORE_EXCLUSIVE_INSTRUCTION_ARM(STREXD,
	cpu->memory.store32(cpu, address, d, &currentCycles);
	if (rm + 1 < ARM_PC) {
		cpu->memory.store32(cpu, address + 4, cpu->gprs[rm + 1],
			&currentCycles);
	})

// End ARMv6 additions

// ARMv6T2 and ARMv7 additions, ARM state. These fill slots that were ILL,
// apart from MOVW and MOVT: they take the TST and CMP immediate rows with
// the S bit clear, which ARMv6 left undefined but upstream decoded as the S
// forms. See README.md.

DEFINE_INSTRUCTION_ARM(MOVWI,
	int rd = (opcode >> 12) & 0xF;
	ARM_PC_DEST(rd);
	cpu->gprs[rd] = (int32_t) (((opcode >> 4) & 0xF000) | (opcode & 0x0FFF));)

DEFINE_INSTRUCTION_ARM(MOVTI,
	int rd = (opcode >> 12) & 0xF;
	ARM_PC_DEST(rd);
	uint32_t immediate = ((opcode >> 4) & 0xF000) | (opcode & 0x0FFF);
	cpu->gprs[rd] = (int32_t) (((uint32_t) cpu->gprs[rd] & 0xFFFF) |
		(immediate << 16));)

// Like the dual multiplies, Rd is at 19:16 and Ra at 15:12.
DEFINE_INSTRUCTION_ARM(MLS,
	int rd = (opcode >> 16) & 0xF;
	ARM_PC_DEST(rd);
	int ra = (opcode >> 12) & 0xF;
	uint32_t m = cpu->gprs[(opcode >> 8) & 0xF];
	uint32_t n = cpu->gprs[opcode & 0xF];
	cpu->gprs[rd] = (int32_t) ((uint32_t) cpu->gprs[ra] - n * m);)

// Bit field insert, or clear when Rn == 15. The field is lsb (11:7) to msb
// (20:16) inclusive. msb < lsb is unpredictable, and treated as undefined,
// which makes a bad guest fail visibly.
DEFINE_INSTRUCTION_ARM(BFI,
	int rd = (opcode >> 12) & 0xF;
	ARM_PC_DEST(rd);
	int rn = opcode & 0xF;
	unsigned lsb = (opcode >> 7) & 0x1F;
	unsigned msb = (opcode >> 16) & 0x1F;
	if (msb < lsb) {
		pcDest = false;
		ARM_ILL;
	} else {
		uint32_t mask = (0xFFFFFFFFu >> (31 - msb)) & (0xFFFFFFFFu << lsb);
		uint32_t field = rn == ARM_PC ? 0 : (uint32_t) cpu->gprs[rn] << lsb;
		cpu->gprs[rd] = (int32_t) (((uint32_t) cpu->gprs[rd] & ~mask) |
			(field & mask));
	})

// Bit field extract: width - 1 at 20:16, lsb at 11:7. A field running past
// bit 31 is unpredictable, and treated as undefined, as for BFI.
#define DEFINE_BITFIELD_EXTRACT_INSTRUCTION_ARM(NAME, BODY)                    \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int rd = (opcode >> 12) & 0xF;                                         \
		ARM_PC_DEST(rd);                                                     \
		unsigned lsb = (opcode >> 7) & 0x1F;                                   \
		unsigned width = ((opcode >> 16) & 0x1F) + 1;                          \
		if (lsb + width > 32) {                                                \
			pcDest = false;                                                    \
			ARM_ILL;                                                           \
		} else {                                                               \
			uint32_t field = (uint32_t) cpu->gprs[opcode & 0xF] >> lsb;        \
			BODY;                                                              \
		})

DEFINE_BITFIELD_EXTRACT_INSTRUCTION_ARM(SBFX,
	cpu->gprs[rd] = (int32_t) (field << (32 - width)) >> (32 - width))
DEFINE_BITFIELD_EXTRACT_INSTRUCTION_ARM(UBFX,
	cpu->gprs[rd] = (int32_t) (width == 32 ? field
	                                       : field & ((1u << width) - 1)))

DEFINE_PARALLEL_INSTRUCTION_ARM(RBIT,
	uint32_t v = (uint32_t) m;
	v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);
	v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);
	v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);
	cpu->gprs[rd] = (int32_t) __builtin_bswap32(v);)

// Integer divide is ARMv7s rather than ARMv7: the A6 (iPhone 5) has it and
// the Cortex-A8 does not, so only code built for armv7s uses it. Rd is at
// 19:16, Rm at 11:8 and Rn at 3:0. Dividing by zero gives zero, since
// nothing enables the trap, and INT32_MIN / -1 overflows to INT32_MIN.
DEFINE_INSTRUCTION_ARM(SDIV,
	int rd = (opcode >> 16) & 0xF;
	ARM_PC_DEST(rd);
	int32_t n = cpu->gprs[opcode & 0xF];
	int32_t m = cpu->gprs[(opcode >> 8) & 0xF];
	if (m == 0) {
		cpu->gprs[rd] = 0;
	} else if (n == INT32_MIN && m == -1) {
		cpu->gprs[rd] = INT32_MIN;
	} else {
		cpu->gprs[rd] = n / m;
	})

DEFINE_INSTRUCTION_ARM(UDIV,
	int rd = (opcode >> 16) & 0xF;
	ARM_PC_DEST(rd);
	uint32_t n = cpu->gprs[opcode & 0xF];
	uint32_t m = cpu->gprs[(opcode >> 8) & 0xF];
	cpu->gprs[rd] = m ? (int32_t) (n / m) : 0;)

// End ARMv7 additions

// End branch definitions

// Begin coprocessor definitions

#define DEFINE_COPROCESSOR_INSTRUCTION(NAME, BODY) \
	DEFINE_INSTRUCTION_ARM(NAME, \
		int op1 = (opcode >> 21) & 7; \
		int op2 = (opcode >> 5) & 7; \
		int rd = (opcode >> 12) & 0xF; \
		int cp = (opcode >> 8) & 0xF; \
		int crn = (opcode >> 16) & 0xF; \
		int crm = opcode & 0xF; \
		UNUSED(op1); \
		UNUSED(op2); \
		UNUSED(rd); \
		UNUSED(crn); \
		UNUSED(crm); \
		BODY;)

// See the `raw` hook in struct ARMCoprocessor: it gets first refusal on
// every coprocessor instruction, and is the only path for LDC/STC and
// MCRR/MRRC.
#define ARM_COPROCESSOR_RAW (cpu->cp[cp].raw && cpu->cp[cp].raw(cpu, opcode))

DEFINE_COPROCESSOR_INSTRUCTION(MRC,
	if (ARM_COPROCESSOR_RAW) {
		// Handled by the coprocessor.
	} else if (cpu->cp[cp].mrc) {
		cpu->gprs[rd] = cpu->cp[cp].mrc(cpu, crn, crm, op1, op2);
	} else {
		ARM_ILL;
	})

DEFINE_COPROCESSOR_INSTRUCTION(MCR,
	if (ARM_COPROCESSOR_RAW) {
		// Handled by the coprocessor.
	} else if (cpu->cp[cp].mcr) {
		cpu->cp[cp].mcr(cpu, crn, crm, op1, op2, cpu->gprs[rd]);
	} else {
		ARM_ILL;
	})

DEFINE_COPROCESSOR_INSTRUCTION(CDP,
	if (ARM_COPROCESSOR_RAW) {
		// Handled by the coprocessor.
	} else if (cpu->cp[cp].cdp) {
		cpu->cp[cp].cdp(cpu, crn, crm, rd, op1, op2);
	} else {
		ARM_ILL;
	})

// Coprocessor load/store, and the two-register transfers that share the
// 110 encoding space. Upstream stubs these out entirely.
#define DEFINE_COPROCESSOR_RAW_INSTRUCTION_ARM(NAME, FALLBACK)                 \
	DEFINE_INSTRUCTION_ARM(NAME,                                               \
		int cp = (opcode >> 8) & 0xF;                                          \
		if (!ARM_COPROCESSOR_RAW) {                                            \
			FALLBACK;                                                          \
		})

DEFINE_COPROCESSOR_RAW_INSTRUCTION_ARM(LDC, ARM_STUB)
DEFINE_COPROCESSOR_RAW_INSTRUCTION_ARM(STC, ARM_STUB)
// MCRR and MRRC differ only in bit 20, which the handler sees, so one
// emitter serves both rows.
DEFINE_COPROCESSOR_RAW_INSTRUCTION_ARM(MCRR, ARM_ILL)

// Begin miscellaneous definitions

DEFINE_INSTRUCTION_ARM(BKPT,
	cpu->irqh.bkpt32(cpu, ((opcode >> 4) & 0xFFF0) | (opcode & 0xF));
	currentCycles = 0;); // Not strictly in ARMv4T, but here for convenience
DEFINE_INSTRUCTION_ARM(ILL, ARM_ILL) // Illegal opcode

DEFINE_INSTRUCTION_ARM(MSR,
	int c = opcode & 0x00010000;
	int s = opcode & 0x00040000;
	int f = opcode & 0x00080000;
	int32_t operand = cpu->gprs[opcode & 0x0000000F];
	int32_t mask = (c ? 0x000000FF : 0) | (s ? 0x00FF0000 : 0) | (f ? 0xFF000000 : 0);
	if (mask & PSR_USER_MASK) {
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_USER_MASK) | (operand & PSR_USER_MASK);
	}
	if (mask & PSR_STATUS_MASK) {
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_STATUS_MASK) | (operand & PSR_STATUS_MASK);
	}
	if (mask & PSR_STATE_MASK) {
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_STATE_MASK) | (operand & PSR_STATE_MASK);
	}
	if (cpu->privilegeMode != MODE_USER && (mask & PSR_PRIV_MASK)) {
		ARMSetPrivilegeMode(cpu, (enum PrivilegeMode) ((operand & 0x0000000F) | 0x00000010));
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_PRIV_MASK) | (operand & PSR_PRIV_MASK);
	}
	_ARMReadCPSR(cpu);
	if (cpu->executionMode == MODE_THUMB) {
		cpu->prefetch[0] = 0x46C0; // nop
		cpu->prefetch[1] &= 0xFFFF;
		cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
	} else {
		LOAD_32(cpu->prefetch[0], (cpu->gprs[ARM_PC] - WORD_SIZE_ARM) & cpu->memory.activeMask, cpu->memory.activeRegion);
		LOAD_32(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
	})

DEFINE_INSTRUCTION_ARM(MSRR,
	int c = opcode & 0x00010000;
	int s = opcode & 0x00040000;
	int f = opcode & 0x00080000;
	int32_t operand = cpu->gprs[opcode & 0x0000000F];
	int32_t mask = (c ? 0x000000FF : 0) | (s ? 0x00FF0000 : 0) | (f ? 0xFF000000 : 0);
	mask &= PSR_USER_MASK | PSR_STATUS_MASK | PSR_PRIV_MASK | PSR_STATE_MASK;
	cpu->spsr.packed = (cpu->spsr.packed & ~mask) | (operand & mask) | 0x00000010;)

DEFINE_INSTRUCTION_ARM(MRS, \
	int rd = (opcode >> 12) & 0xF; \
	cpu->gprs[rd] = cpu->cpsr.packed;)

DEFINE_INSTRUCTION_ARM(MRSR, \
	int rd = (opcode >> 12) & 0xF; \
	cpu->gprs[rd] = cpu->spsr.packed;)

DEFINE_INSTRUCTION_ARM(MSRI,
	uint32_t c = opcode & 0x00010000;
	uint32_t s = opcode & 0x00040000;
	uint32_t f = opcode & 0x00080000;
	uint32_t rotate = (opcode & 0x00000F00) >> 7;
	uint32_t operand = ROR(opcode & 0x000000FF, rotate);
	uint32_t mask = (c ? 0x000000FF : 0) | (s ? 0x00FF0000 : 0) | (f ? 0xFF000000 : 0);
	if (mask & PSR_USER_MASK) {
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_USER_MASK) | (operand & PSR_USER_MASK);
	}
	if (mask & PSR_STATUS_MASK) {
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_STATUS_MASK) | (operand & PSR_STATUS_MASK);
	}
	if (mask & PSR_STATE_MASK) {
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_STATE_MASK) | (operand & PSR_STATE_MASK);
	}
	if (cpu->privilegeMode != MODE_USER && (mask & PSR_PRIV_MASK)) {
		ARMSetPrivilegeMode(cpu, (enum PrivilegeMode) ((operand & 0x0000000F) | 0x00000010));
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_PRIV_MASK) | (operand & PSR_PRIV_MASK);
	}
	_ARMReadCPSR(cpu);
	if (cpu->executionMode == MODE_THUMB) {
		cpu->prefetch[0] = 0x46C0; // nop
		cpu->prefetch[1] &= 0xFFFF;
		cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
	} else {
		LOAD_32(cpu->prefetch[0], (cpu->gprs[ARM_PC] - WORD_SIZE_ARM) & cpu->memory.activeMask, cpu->memory.activeRegion);
		LOAD_32(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
	})

DEFINE_INSTRUCTION_ARM(MSRRI,
	uint32_t c = opcode & 0x00010000;
	uint32_t s = opcode & 0x00040000;
	uint32_t f = opcode & 0x00080000;
	uint32_t rotate = (opcode & 0x00000F00) >> 7;
	uint32_t operand = ROR(opcode & 0x000000FF, rotate);
	uint32_t mask = (c ? 0x000000FF : 0) | (s ? 0x00FF0000 : 0) | (f ? 0xFF000000 : 0);
	mask &= PSR_USER_MASK | PSR_STATUS_MASK | PSR_PRIV_MASK | PSR_STATE_MASK;
	cpu->spsr.packed = (cpu->spsr.packed & ~mask) | (operand & mask) | 0x00000010;)

DEFINE_INSTRUCTION_ARM(SWI, cpu->irqh.swi32(cpu, opcode & 0xFFFFFF))

// The cond == 0xF encoding space is "unconditional instruction" on ARMv5,
// where ARMv4T read it as "never execute", so ARMStep routes it here before
// evaluating the condition.
void ARMStepUnconditional(struct ARMCore* cpu, uint32_t opcode) {
	if ((opcode & 0x0E000000) == 0x02000000 || (opcode & 0x0F100000) == 0x04000000) {
		// Advanced SIMD data processing (1111 001x) and element and
		// structure loads and stores (1111 0100 xxx0).
		if (cpu->advancedSimd && cpu->advancedSimd(cpu, opcode)) {
			cpu->cycles += ARM_PREFETCH_CYCLES;
		} else {
			cpu->irqh.hitIllegal(cpu, opcode);
		}
	} else if ((opcode & 0x0E000000) == 0x0A000000) {
		_ARMInstructionBLX_I(cpu, opcode);
	} else if (((opcode & 0x0D70F000) == 0x0550F000 ||
	            (opcode & 0x0D70F000) == 0x0510F000 ||
	            (opcode & 0x0D70F000) == 0x0450F000) &&
	           (opcode & 0x02000010) != 0x02000010) {
		// PLD, and ARMv7's PLDW and PLI: cache hints, and touchHLE models
		// no cache.
		cpu->cycles += ARM_PREFETCH_CYCLES;
	} else if ((opcode & 0x0D700000) == 0x04100000 && (opcode & 0x02000010) != 0x02000010) {
		// Unallocated memory hints, which ARMv7 says to treat as NOPs.
		cpu->cycles += ARM_PREFETCH_CYCLES;
	} else if ((opcode & 0x0FFFFDFF) == 0x01010000) {
		// SETEND. Only little-endian is supported: mGBA's prefetch and
		// touchHLE's memory both assume it, so SETEND BE would silently
		// mis-execute rather than merely run slowly.
		if (opcode & 0x00000200) {
			cpu->irqh.hitIllegal(cpu, opcode);
		} else {
			cpu->cycles += ARM_PREFETCH_CYCLES;
		}
	} else if ((opcode & 0x0FF1FE20) == 0x01000000) {
		// CPS, which changes the interrupt masks and privilege mode.
		// touchHLE runs guest code as an application, and models neither,
		// so this is a no-op.
		cpu->cycles += ARM_PREFETCH_CYCLES;
	} else if ((opcode & 0x0FFFFFFF) == 0x057FF01F) {
		// CLREX.
		cpu->exclusiveMonitor = false;
		cpu->cycles += ARM_PREFETCH_CYCLES;
	} else if ((opcode & 0x0FFFFFF0) == 0x057FF040 ||
	           (opcode & 0x0FFFFFF0) == 0x057FF050) {
		// ARMv7's DSB and DMB, which ARMv6 did through CP15. One in-order
		// core with no cache leaves nothing to order.
		cpu->cycles += ARM_PREFETCH_CYCLES;
	} else if ((opcode & 0x0FFFFFF0) == 0x057FF060) {
		// ISB. The two instructions after this one are already fetched, so
		// refetch them, as MSR does: code the guest has just written must
		// be what runs next.
		LOAD_32(cpu->prefetch[0], (cpu->gprs[ARM_PC] - WORD_SIZE_ARM) & cpu->memory.activeMask, cpu->memory.activeRegion);
		LOAD_32(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
		cpu->cycles += ARM_PREFETCH_CYCLES;
	} else {
		cpu->irqh.hitIllegal(cpu, opcode);
	}
}

// UNPREDICTABLE encodings: should-be-zero and should-be-one fields.
//
// The architecture leaves many encodings UNPREDICTABLE: fields marked
// should-be-zero or should-be-one holding something else, and forms that
// would do something unusual from user mode. Upstream ignored the fields,
// as the ARM7TDMI does. For those a Cortex-A8 may equally ignore the field
// or treat the instruction as undefined; this core does the latter, so that
// a bad encoding fails visibly rather than doing something arbitrary. The
// check comes after the condition, as for any other undefined instruction.
//
// Each row of the decode table gets a kind of check (0 for none), worked out
// once from the row's bits 27:20 and 7:4.

uint8_t _armStrictTable[0x1000];

static unsigned _armStrictKind(uint32_t op) {
	unsigned op1 = (op >> 20) & 0xFF; // bits 27:20
	unsigned op2 = (op >> 4) & 0xF;   // bits 7:4
	switch (op1 >> 5) {
	case 0: // 000
		if ((op2 & 0x9) == 0x9) {
			if ((op2 & 0x6) == 0) {
				if (!(op1 & 0x10)) {
					// Multiplies; MUL has an SBZ Ra.
					return (op1 & 0xE) == 0 ? STRICT_SBZ_15_12 : STRICT_NONE;
				}
				if (op1 & 0x08) {
					return (op1 & 0x01) ? STRICT_LDREX : STRICT_STREX;
				}
				return (op1 & 0x03) == 0 ? STRICT_SBZ_11_8 : STRICT_NONE; // SWP
			}
			// Extra loads and stores; bit 22 clear is the register form.
			bool reg = !(op1 & 0x04);
			bool dual = !(op1 & 0x01) && (op2 & 0x4);
			if (dual) {
				return reg ? STRICT_LDRD_REG : STRICT_LDRD_IMM;
			}
			return reg ? STRICT_SBZ_11_8 : STRICT_NONE;
		}
		if ((op1 & 0x19) == 0x10) {
			// Miscellaneous and halfword multiplies (bits 24:23 == 10, S clear).
			if (op2 & 0x8) {
				switch ((op1 >> 1) & 3) {
				case 1:
					return (op2 & 0x2) ? STRICT_SBZ_15_12 : STRICT_NONE; // SMULWy
				case 3:
					return STRICT_SBZ_15_12; // SMULxy
				default:
					return STRICT_NONE;
				}
			}
			switch (op2 & 0x7) {
			case 0:
				return (op1 & 0x2) ? STRICT_MSR_REG : STRICT_MRS;
			case 1:
				return ((op1 >> 1) & 3) == 1 ? STRICT_BX : ((op1 >> 1) & 3) == 3 ? STRICT_SBO_19_16_11_8 : STRICT_NONE;
			case 3:
				return ((op1 >> 1) & 3) == 1 ? STRICT_BX : STRICT_NONE; // BLX (register)
			case 5:
				return STRICT_SBZ_11_8; // QADD, QSUB, QDADD, QDSUB
			default:
				return STRICT_NONE;
			}
		}
		// Fall through: data processing (register).
		break;
	case 1: // 001
		if ((op1 & 0x19) == 0x10) {
			// MOVW, MOVT, and MSR (immediate) and the hints.
			return (op1 & 0x2) ? STRICT_MSR_IMM : STRICT_NONE;
		}
		break;
	case 3: // 011
		if (!(op2 & 1)) {
			return STRICT_NONE;
		}
		switch ((op1 >> 3) & 3) {
		case 0: // parallel add/subtract
			return STRICT_SBO_11_8;
		case 1:
			if ((op2 & 0xE) == 0x6) {
				return STRICT_SBZ_9_8; // extends
			}
			if ((op1 & 0x7) == 0 && (op2 & 0xE) == 0xA) {
				return STRICT_SBO_11_8; // SEL
			}
			if ((op1 & 0x3) == 2 && (op2 & 0xE) == 0x2) {
				return STRICT_SBO_11_8; // SSAT16, USAT16
			}
			if ((op1 & 0x3) == 3 && (op2 & 0x7) == 0x3) {
				return STRICT_SBO_19_16_11_8; // REV, REV16, RBIT, REVSH
			}
			return STRICT_NONE;
		case 2:
			if ((op1 & 0x5) == 1 && (op2 & 0xE) == 0) {
				return STRICT_SBO_15_12; // SDIV, UDIV
			}
			return STRICT_NONE;
		default:
			return STRICT_NONE;
		}
	case 4: // 100: LDM and STM
		return STRICT_LSM;
	default:
		return STRICT_NONE;
	}
	// Data processing, register or immediate.
	unsigned dpop = (op1 >> 1) & 0xF;
	bool s = op1 & 1;
	if (dpop >= 8 && dpop <= 11) {
		return STRICT_DP_CMP;
	}
	// In user mode SUBS and MOVS with Rd == 15 are exception returns, and
	// undefined; the other S forms set the flags and branch
	// (interworking) like the non-S ones, as this core does anyway.
	if (dpop == 13) {
		return s ? STRICT_DP_MOV_S : STRICT_DP_MOV;
	}
	if (dpop == 15) {
		return STRICT_DP_MOV;
	}
	return (s && dpop == 2) ? STRICT_DP_S : STRICT_NONE;
}

void ARMInitStrictTable(void) {
	unsigned i;
	for (i = 0; i < 0x1000; ++i) {
		_armStrictTable[i] = (uint8_t) _armStrictKind(((i & 0xFF0) << 16) | ((i & 0xF) << 4));
	}
}

const ARMInstruction _armTable[0x1000] = {
	DECLARE_ARM_EMITTER_BLOCK(_ARMInstruction)
};
