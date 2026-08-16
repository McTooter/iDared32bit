/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/arm/isa-thumb.h>

#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/arm/emitter-thumb.h>

// Instruction definitions
// Beware pre-processor insanity

// As in isa-arm.c, N, Z, C and V are set individually, leaving Q and the
// IT state in the same byte alone.
#define THUMB_ADDITION_S(M, N, D) \
	cpu->cpsr.n = ARM_SIGN(D); \
	cpu->cpsr.z = !(D); \
	cpu->cpsr.c = ARM_CARRY_FROM(M, N, D); \
	cpu->cpsr.v = ARM_V_ADDITION(M, N, D);

#define THUMB_SUBTRACTION_S(M, N, D) \
	cpu->cpsr.n = ARM_SIGN(D); \
	cpu->cpsr.z = !(D); \
	cpu->cpsr.c = ARM_BORROW_FROM(M, N, D); \
	cpu->cpsr.v = ARM_V_SUBTRACTION(M, N, D);

#define THUMB_SUBTRACTION_CARRY_S(M, N, D, C) \
	cpu->cpsr.n = ARM_SIGN(D); \
	cpu->cpsr.z = !(D); \
	cpu->cpsr.c = ARM_BORROW_FROM_CARRY(M, N, D, C); \
	cpu->cpsr.v = ARM_V_SUBTRACTION(M, N, D);

#define THUMB_NEUTRAL_S(M, N, D) \
	cpu->cpsr.n = ARM_SIGN(D); \
	cpu->cpsr.z = !(D);

#define THUMB_ADDITION(D, M, N) \
	int n = N; \
	int m = M; \
	D = M + N; \
	THUMB_ADDITION_S(m, n, D)

#define THUMB_SUBTRACTION(D, M, N) \
	int n = N; \
	int m = M; \
	D = M - N; \
	THUMB_SUBTRACTION_S(m, n, D)

#define THUMB_PREFETCH_CYCLES (1 + cpu->memory.activeSeqCycles16)

#define THUMB_LOAD_POST_BODY \
	currentCycles += cpu->memory.activeNonseqCycles16 - cpu->memory.activeSeqCycles16;

#define THUMB_STORE_POST_BODY \
	currentCycles += cpu->memory.activeNonseqCycles16 - cpu->memory.activeSeqCycles16;

#define DEFINE_INSTRUCTION_THUMB(NAME, BODY) \
	static void _ThumbInstruction ## NAME (struct ARMCore* cpu, unsigned opcode) {  \
		int currentCycles = THUMB_PREFETCH_CYCLES; \
		BODY; \
		cpu->cycles += currentCycles; \
	}

#define DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int immediate = (opcode >> 6) & 0x001F; \
		int rd = opcode & 0x0007; \
		int rm = (opcode >> 3) & 0x0007; \
		BODY;)

DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(LSL1,
	if (!immediate) {
		cpu->gprs[rd] = cpu->gprs[rm];
	} else {
		cpu->cpsr.c = (cpu->gprs[rm] >> (32 - immediate)) & 1;
		cpu->gprs[rd] = cpu->gprs[rm] << immediate;
	}
	THUMB_NEUTRAL_S( , , cpu->gprs[rd]);)

DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(LSR1,
	if (!immediate) {
		cpu->cpsr.c = ARM_SIGN(cpu->gprs[rm]);
		cpu->gprs[rd] = 0;
	} else {
		cpu->cpsr.c = (cpu->gprs[rm] >> (immediate - 1)) & 1;
		cpu->gprs[rd] = ((uint32_t) cpu->gprs[rm]) >> immediate;
	}
	THUMB_NEUTRAL_S( , , cpu->gprs[rd]);)

DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(ASR1,
	if (!immediate) {
		cpu->cpsr.c = ARM_SIGN(cpu->gprs[rm]);
		if (cpu->cpsr.c) {
			cpu->gprs[rd] = 0xFFFFFFFF;
		} else {
			cpu->gprs[rd] = 0;
		}
	} else {
		cpu->cpsr.c = (cpu->gprs[rm] >> (immediate - 1)) & 1;
		cpu->gprs[rd] = cpu->gprs[rm] >> immediate;
	}
	THUMB_NEUTRAL_S( , , cpu->gprs[rd]);)

DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(LDR1, cpu->gprs[rd] = cpu->memory.load32(cpu, cpu->gprs[rm] + immediate * 4, &currentCycles); THUMB_LOAD_POST_BODY;)
DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(LDRB1, cpu->gprs[rd] = cpu->memory.load8(cpu, cpu->gprs[rm] + immediate, &currentCycles); THUMB_LOAD_POST_BODY;)
DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(LDRH1, cpu->gprs[rd] = cpu->memory.load16(cpu, cpu->gprs[rm] + immediate * 2, &currentCycles); THUMB_LOAD_POST_BODY;)
DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(STR1, cpu->memory.store32(cpu, cpu->gprs[rm] + immediate * 4, cpu->gprs[rd], &currentCycles); THUMB_STORE_POST_BODY;)
DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(STRB1, cpu->memory.store8(cpu, cpu->gprs[rm] + immediate, cpu->gprs[rd], &currentCycles); THUMB_STORE_POST_BODY;)
DEFINE_IMMEDIATE_5_INSTRUCTION_THUMB(STRH1, cpu->memory.store16(cpu, cpu->gprs[rm] + immediate * 2, cpu->gprs[rd], &currentCycles); THUMB_STORE_POST_BODY;)

#define DEFINE_DATA_FORM_1_INSTRUCTION_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int rm = (opcode >> 6) & 0x0007; \
		int rd = opcode & 0x0007; \
		int rn = (opcode >> 3) & 0x0007; \
		BODY;)

DEFINE_DATA_FORM_1_INSTRUCTION_THUMB(ADD3, THUMB_ADDITION(cpu->gprs[rd], cpu->gprs[rn], cpu->gprs[rm]))
DEFINE_DATA_FORM_1_INSTRUCTION_THUMB(SUB3, THUMB_SUBTRACTION(cpu->gprs[rd], cpu->gprs[rn], cpu->gprs[rm]))

#define DEFINE_DATA_FORM_2_INSTRUCTION_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int immediate = (opcode >> 6) & 0x0007; \
		int rd = opcode & 0x0007; \
		int rn = (opcode >> 3) & 0x0007; \
		BODY;)

DEFINE_DATA_FORM_2_INSTRUCTION_THUMB(ADD1, THUMB_ADDITION(cpu->gprs[rd], cpu->gprs[rn], immediate))
DEFINE_DATA_FORM_2_INSTRUCTION_THUMB(SUB1, THUMB_SUBTRACTION(cpu->gprs[rd], cpu->gprs[rn], immediate))

#define DEFINE_DATA_FORM_3_INSTRUCTION_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int rd = (opcode >> 8) & 0x0007; \
		int immediate = opcode & 0x00FF; \
		BODY;)

DEFINE_DATA_FORM_3_INSTRUCTION_THUMB(ADD2, THUMB_ADDITION(cpu->gprs[rd], cpu->gprs[rd], immediate))
DEFINE_DATA_FORM_3_INSTRUCTION_THUMB(CMP1, int aluOut = cpu->gprs[rd] - immediate; THUMB_SUBTRACTION_S(cpu->gprs[rd], immediate, aluOut))
DEFINE_DATA_FORM_3_INSTRUCTION_THUMB(MOV1, cpu->gprs[rd] = immediate; THUMB_NEUTRAL_S(, , cpu->gprs[rd]))
DEFINE_DATA_FORM_3_INSTRUCTION_THUMB(SUB2, THUMB_SUBTRACTION(cpu->gprs[rd], cpu->gprs[rd], immediate))

#define DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int rd = opcode & 0x0007; \
		int rn = (opcode >> 3) & 0x0007; \
		BODY;)

DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(AND, cpu->gprs[rd] = cpu->gprs[rd] & cpu->gprs[rn]; THUMB_NEUTRAL_S( , , cpu->gprs[rd]))
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(EOR, cpu->gprs[rd] = cpu->gprs[rd] ^ cpu->gprs[rn]; THUMB_NEUTRAL_S( , , cpu->gprs[rd]))
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(LSL2,
	int rs = cpu->gprs[rn] & 0xFF;
	if (rs) {
		if (rs < 32) {
			cpu->cpsr.c = (cpu->gprs[rd] >> (32 - rs)) & 1;
			cpu->gprs[rd] <<= rs;
		} else {
			if (rs > 32) {
				cpu->cpsr.c = 0;
			} else {
				cpu->cpsr.c = cpu->gprs[rd] & 0x00000001;
			}
			cpu->gprs[rd] = 0;
		}
	}
	++currentCycles;
	THUMB_NEUTRAL_S( , , cpu->gprs[rd]))

DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(LSR2,
	int rs = cpu->gprs[rn] & 0xFF;
	if (rs) {
		if (rs < 32) {
			cpu->cpsr.c = (cpu->gprs[rd] >> (rs - 1)) & 1;
			cpu->gprs[rd] = (uint32_t) cpu->gprs[rd] >> rs;
		} else {
			if (rs > 32) {
				cpu->cpsr.c = 0;
			} else {
				cpu->cpsr.c = ARM_SIGN(cpu->gprs[rd]);
			}
			cpu->gprs[rd] = 0;
		}
	}
	++currentCycles;
	THUMB_NEUTRAL_S( , , cpu->gprs[rd]))

DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(ASR2,
	int rs = cpu->gprs[rn] & 0xFF;
	if (rs) {
		if (rs < 32) {
			cpu->cpsr.c = (cpu->gprs[rd] >> (rs - 1)) & 1;
			cpu->gprs[rd] >>= rs;
		} else {
			cpu->cpsr.c = ARM_SIGN(cpu->gprs[rd]);
			if (cpu->cpsr.c) {
				cpu->gprs[rd] = 0xFFFFFFFF;
			} else {
				cpu->gprs[rd] = 0;
			}
		}
	}
	++currentCycles;
	THUMB_NEUTRAL_S( , , cpu->gprs[rd]))

DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(ADC,
	int n = cpu->gprs[rn];
	int d = cpu->gprs[rd];
	cpu->gprs[rd] = d + n + cpu->cpsr.c;
	THUMB_ADDITION_S(d, n, cpu->gprs[rd]);)

DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(SBC,
	int n = cpu->gprs[rn];
	int d = cpu->gprs[rd];
	cpu->gprs[rd] = d - n - !cpu->cpsr.c;
	THUMB_SUBTRACTION_CARRY_S(d, n, cpu->gprs[rd], !cpu->cpsr.c);)

DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(ROR,
	int rs = cpu->gprs[rn] & 0xFF;
	if (rs) {
		int r4 = rs & 0x1F;
		if (r4 > 0) {
			cpu->cpsr.c = (cpu->gprs[rd] >> (r4 - 1)) & 1;
			cpu->gprs[rd] = ROR(cpu->gprs[rd], r4);
		} else {
			cpu->cpsr.c = ARM_SIGN(cpu->gprs[rd]);
		}
	}
	++currentCycles;
	THUMB_NEUTRAL_S( , , cpu->gprs[rd]);)
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(TST, int32_t aluOut = cpu->gprs[rd] & cpu->gprs[rn]; THUMB_NEUTRAL_S(cpu->gprs[rd], cpu->gprs[rn], aluOut))
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(NEG, THUMB_SUBTRACTION(cpu->gprs[rd], 0, cpu->gprs[rn]))
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(CMP2, int32_t aluOut = cpu->gprs[rd] - cpu->gprs[rn]; THUMB_SUBTRACTION_S(cpu->gprs[rd], cpu->gprs[rn], aluOut))
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(CMN, int32_t aluOut = cpu->gprs[rd] + cpu->gprs[rn]; THUMB_ADDITION_S(cpu->gprs[rd], cpu->gprs[rn], aluOut))
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(ORR, cpu->gprs[rd] = cpu->gprs[rd] | cpu->gprs[rn]; THUMB_NEUTRAL_S( , , cpu->gprs[rd]))
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(MUL, ARM_WAIT_SMUL(cpu->gprs[rd], 0); cpu->gprs[rd] *= cpu->gprs[rn]; THUMB_NEUTRAL_S( , , cpu->gprs[rd]); currentCycles += cpu->memory.activeNonseqCycles16 - cpu->memory.activeSeqCycles16)
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(BIC, cpu->gprs[rd] = cpu->gprs[rd] & ~cpu->gprs[rn]; THUMB_NEUTRAL_S( , , cpu->gprs[rd]))
DEFINE_DATA_FORM_5_INSTRUCTION_THUMB(MVN, cpu->gprs[rd] = ~cpu->gprs[rn]; THUMB_NEUTRAL_S( , , cpu->gprs[rd]))

#define DEFINE_INSTRUCTION_WITH_HIGH_EX_THUMB(NAME, H1, H2, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int rd = (opcode & 0x0007) | H1; \
		int rm = ((opcode >> 3) & 0x0007) | H2; \
		BODY;)

#define DEFINE_INSTRUCTION_WITH_HIGH_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_WITH_HIGH_EX_THUMB(NAME ## 00, 0, 0, BODY) \
	DEFINE_INSTRUCTION_WITH_HIGH_EX_THUMB(NAME ## 01, 0, 8, BODY) \
	DEFINE_INSTRUCTION_WITH_HIGH_EX_THUMB(NAME ## 10, 8, 0, BODY) \
	DEFINE_INSTRUCTION_WITH_HIGH_EX_THUMB(NAME ## 11, 8, 8, BODY)

DEFINE_INSTRUCTION_WITH_HIGH_THUMB(ADD4,
	cpu->gprs[rd] += cpu->gprs[rm];
	if (rd == ARM_PC) {
		currentCycles += ThumbWritePC(cpu);
	})

DEFINE_INSTRUCTION_WITH_HIGH_THUMB(CMP3, int32_t aluOut = cpu->gprs[rd] - cpu->gprs[rm]; THUMB_SUBTRACTION_S(cpu->gprs[rd], cpu->gprs[rm], aluOut))
DEFINE_INSTRUCTION_WITH_HIGH_THUMB(MOV3,
	cpu->gprs[rd] = cpu->gprs[rm];
	if (rd == ARM_PC) {
		currentCycles += ThumbWritePC(cpu);
	})

#define DEFINE_IMMEDIATE_WITH_REGISTER_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int rd = (opcode >> 8) & 0x0007; \
		int immediate = (opcode & 0x00FF) << 2; \
		BODY;)

DEFINE_IMMEDIATE_WITH_REGISTER_THUMB(LDR3, cpu->gprs[rd] = cpu->memory.load32(cpu, (cpu->gprs[ARM_PC] & 0xFFFFFFFC) + immediate, &currentCycles); THUMB_LOAD_POST_BODY;)
DEFINE_IMMEDIATE_WITH_REGISTER_THUMB(LDR4, cpu->gprs[rd] = cpu->memory.load32(cpu, cpu->gprs[ARM_SP] + immediate, &currentCycles); THUMB_LOAD_POST_BODY;)
DEFINE_IMMEDIATE_WITH_REGISTER_THUMB(STR3, cpu->memory.store32(cpu, cpu->gprs[ARM_SP] + immediate, cpu->gprs[rd], &currentCycles); THUMB_STORE_POST_BODY;)

DEFINE_IMMEDIATE_WITH_REGISTER_THUMB(ADD5, cpu->gprs[rd] = (cpu->gprs[ARM_PC] & 0xFFFFFFFC) + immediate)
DEFINE_IMMEDIATE_WITH_REGISTER_THUMB(ADD6, cpu->gprs[rd] = cpu->gprs[ARM_SP] + immediate)

#define DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int rm = (opcode >> 6) & 0x0007; \
		int rd = opcode & 0x0007; \
		int rn = (opcode >> 3) & 0x0007; \
		BODY;)

DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(LDR2, cpu->gprs[rd] = cpu->memory.load32(cpu, cpu->gprs[rn] + cpu->gprs[rm], &currentCycles); THUMB_LOAD_POST_BODY;)
DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(LDRB2, cpu->gprs[rd] = cpu->memory.load8(cpu, cpu->gprs[rn] + cpu->gprs[rm], &currentCycles); THUMB_LOAD_POST_BODY;)
DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(LDRH2, cpu->gprs[rd] = cpu->memory.load16(cpu, cpu->gprs[rn] + cpu->gprs[rm], &currentCycles); THUMB_LOAD_POST_BODY;)
DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(LDRSB, cpu->gprs[rd] = ARM_SXT_8(cpu->memory.load8(cpu, cpu->gprs[rn] + cpu->gprs[rm], &currentCycles)); THUMB_LOAD_POST_BODY;)
// Upstream sign-extends a byte when the address is odd, which is ARM7TDMI's
// unaligned behaviour; ARMv6 loads the halfword at the address as given, as
// in isa-arm.c.
DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(LDRSH, cpu->gprs[rd] = ARM_SXT_16(cpu->memory.load16(cpu, cpu->gprs[rn] + cpu->gprs[rm], &currentCycles)); THUMB_LOAD_POST_BODY;)
DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(STR2, cpu->memory.store32(cpu, cpu->gprs[rn] + cpu->gprs[rm], cpu->gprs[rd], &currentCycles); THUMB_STORE_POST_BODY;)
DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(STRB2, cpu->memory.store8(cpu, cpu->gprs[rn] + cpu->gprs[rm], cpu->gprs[rd], &currentCycles); THUMB_STORE_POST_BODY;)
DEFINE_LOAD_STORE_WITH_REGISTER_THUMB(STRH2, cpu->memory.store16(cpu, cpu->gprs[rn] + cpu->gprs[rm], cpu->gprs[rd], &currentCycles); THUMB_STORE_POST_BODY;)

#define DEFINE_LOAD_STORE_MULTIPLE_THUMB(NAME, RN, LS, DIRECTION, PRE_BODY, WRITEBACK) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int rn = RN; \
		UNUSED(rn); \
		int rs = opcode & 0xFF; \
		int32_t address = cpu->gprs[RN]; \
		PRE_BODY; \
		/* An empty list is UNPREDICTABLE, and undefined here. */ \
		if (!rs) { \
			ARM_ILL; \
			return; \
		} \
		address = cpu->memory. LS ## Multiple(cpu, address, rs, LSM_ ## DIRECTION, &currentCycles); \
		WRITEBACK;)

DEFINE_LOAD_STORE_MULTIPLE_THUMB(LDMIA,
	(opcode >> 8) & 0x0007,
	load,
	IA,
	,
	THUMB_LOAD_POST_BODY;
	if (!rs) {
		currentCycles += ThumbWritePC(cpu);
	}
	if (!((1 << rn) & rs)) {
		cpu->gprs[rn] = address;
	})

DEFINE_LOAD_STORE_MULTIPLE_THUMB(STMIA,
	(opcode >> 8) & 0x0007,
	store,
	IA,
	,
	THUMB_STORE_POST_BODY;
	cpu->gprs[rn] = address;)

#define DEFINE_CONDITIONAL_BRANCH_THUMB(COND) \
	DEFINE_INSTRUCTION_THUMB(B ## COND, \
		if (ARM_COND_ ## COND) { \
			int8_t immediate = opcode; \
			cpu->gprs[ARM_PC] += (int32_t) immediate << 1; \
			currentCycles += ThumbWritePC(cpu); \
		})

DEFINE_CONDITIONAL_BRANCH_THUMB(EQ)
DEFINE_CONDITIONAL_BRANCH_THUMB(NE)
DEFINE_CONDITIONAL_BRANCH_THUMB(CS)
DEFINE_CONDITIONAL_BRANCH_THUMB(CC)
DEFINE_CONDITIONAL_BRANCH_THUMB(MI)
DEFINE_CONDITIONAL_BRANCH_THUMB(PL)
DEFINE_CONDITIONAL_BRANCH_THUMB(VS)
DEFINE_CONDITIONAL_BRANCH_THUMB(VC)
DEFINE_CONDITIONAL_BRANCH_THUMB(LS)
DEFINE_CONDITIONAL_BRANCH_THUMB(HI)
DEFINE_CONDITIONAL_BRANCH_THUMB(GE)
DEFINE_CONDITIONAL_BRANCH_THUMB(LT)
DEFINE_CONDITIONAL_BRANCH_THUMB(GT)
DEFINE_CONDITIONAL_BRANCH_THUMB(LE)

DEFINE_INSTRUCTION_THUMB(ADD7, cpu->gprs[ARM_SP] += (opcode & 0x7F) << 2)
DEFINE_INSTRUCTION_THUMB(SUB4, cpu->gprs[ARM_SP] -= (opcode & 0x7F) << 2)

DEFINE_LOAD_STORE_MULTIPLE_THUMB(POP,
	ARM_SP,
	load,
	IA,
	,
	THUMB_LOAD_POST_BODY;
	cpu->gprs[ARM_SP] = address)

DEFINE_LOAD_STORE_MULTIPLE_THUMB(POPR,
	ARM_SP,
	load,
	IA,
	rs |= 1 << ARM_PC,
	THUMB_LOAD_POST_BODY;
	cpu->gprs[ARM_SP] = address;
	// Interworks from ARMv5T, which is how a Thumb function returns to an
	// ARM caller.
	currentCycles += ARMInterworkWritePC(cpu);)

DEFINE_LOAD_STORE_MULTIPLE_THUMB(PUSH,
	ARM_SP,
	store,
	DB,
	,
	THUMB_STORE_POST_BODY;
	cpu->gprs[ARM_SP] = address)

DEFINE_LOAD_STORE_MULTIPLE_THUMB(PUSHR,
	ARM_SP,
	store,
	DB,
	rs |= 1 << ARM_LR,
	THUMB_STORE_POST_BODY;
	cpu->gprs[ARM_SP] = address)

DEFINE_INSTRUCTION_THUMB(ILL, ARM_ILL)
DEFINE_INSTRUCTION_THUMB(BKPT,
	cpu->irqh.bkpt16(cpu, opcode & 0xFF);
	currentCycles = 0;) // Not strictly in ARMv4T, but here for convenience
DEFINE_INSTRUCTION_THUMB(B,
	int16_t immediate = (opcode & 0x07FF) << 5;
	cpu->gprs[ARM_PC] += (((int32_t) immediate) >> 4);
	currentCycles += ThumbWritePC(cpu);)

// BL and BLX (immediate), which Thumb-1 splits into two 16-bit halves, are
// one 32-bit instruction to Thumb-2, and decoded as such in isa-thumb2.c.
// The results are the same for a pair executed in order, which is how
// compilers emit them.

// ARMv6T2. Compare and branch forward on (non)zero; bit 11 picks CBNZ, and
// the offset is i:imm5:'0', from bit 9 and bits 7:3.
DEFINE_INSTRUCTION_THUMB(CBZ,
	int rn = opcode & 0x7;
	uint32_t offset = ((opcode >> 2) & 0x3E) | ((opcode >> 3) & 0x40);
	if ((cpu->gprs[rn] != 0) == ((opcode & 0x0800) != 0)) {
		cpu->gprs[ARM_PC] += offset;
		currentCycles += ThumbWritePC(cpu);
	})

// ARMv6T2. IT sets up the block that ThumbStep then applies (see arm.c); a
// zero mask makes it one of the hints NOP, YIELD, WFE, WFI and SEV, which are
// no-ops with a single core and no interrupts.
DEFINE_INSTRUCTION_THUMB(IT,
	if (opcode & 0xF) {
		_ThumbSetITState(cpu, opcode & 0xFF);
	})

// ARMv5T. Rm is read before LR is written, so BLX LR works.
// BX and BLX have three should-be-zero bits; this core treats the
// instruction as undefined if any is set.
DEFINE_INSTRUCTION_THUMB(BLX_R,
	if (opcode & 7) {
		ARM_ILL;
		return;
	}
	int rm = (opcode >> 3) & 0xF;
	uint32_t target = cpu->gprs[rm];
	cpu->gprs[ARM_LR] = (cpu->gprs[ARM_PC] - WORD_SIZE_THUMB) | 1;
	cpu->gprs[ARM_PC] = target;
	currentCycles += ARMInterworkWritePC(cpu);)

DEFINE_INSTRUCTION_THUMB(BX,
	if (opcode & 7) {
		ARM_ILL;
		return;
	}
	int rm = (opcode >> 3) & 0xF;
	_ARMSetMode(cpu, cpu->gprs[rm] & 0x00000001);
	cpu->gprs[ARM_PC] = cpu->gprs[rm] & 0xFFFFFFFE;
	if (cpu->executionMode == MODE_THUMB) {
		currentCycles += ThumbWritePC(cpu);
	} else {
		currentCycles += ARMWritePC(cpu);
	})

DEFINE_INSTRUCTION_THUMB(SWI, cpu->irqh.swi16(cpu, opcode & 0xFF))

// ARMv6 additions (mGBA targets ARM7TDMI/ARMv4T); see README.md. CPY needs
// nothing: it is the ARMv6 mnemonic for the MOV(3) encoding above.

// The extend and byte-reverse groups share a layout: Rd at 2:0, Rm at 5:3,
// and, unlike their ARM counterparts, no rotate field.
#define DEFINE_RD_RM_INSTRUCTION_THUMB(NAME, BODY) \
	DEFINE_INSTRUCTION_THUMB(NAME, \
		int rd = opcode & 0x0007; \
		int rm = (opcode >> 3) & 0x0007; \
		uint32_t m = cpu->gprs[rm]; \
		BODY;)

DEFINE_RD_RM_INSTRUCTION_THUMB(SXTH,
	cpu->gprs[rd] = (int32_t) (int16_t) (uint16_t) m;)
DEFINE_RD_RM_INSTRUCTION_THUMB(SXTB,
	cpu->gprs[rd] = (int32_t) (int8_t) (uint8_t) m;)
DEFINE_RD_RM_INSTRUCTION_THUMB(UXTH, cpu->gprs[rd] = (int32_t) (m & 0xFFFF);)
DEFINE_RD_RM_INSTRUCTION_THUMB(UXTB, cpu->gprs[rd] = (int32_t) (m & 0xFF);)

DEFINE_RD_RM_INSTRUCTION_THUMB(REV,
	cpu->gprs[rd] = (int32_t) __builtin_bswap32(m);)
DEFINE_RD_RM_INSTRUCTION_THUMB(REV16,
	cpu->gprs[rd] =
		(int32_t) (((m & 0x00FF00FF) << 8) | ((m >> 8) & 0x00FF00FF));)
DEFINE_RD_RM_INSTRUCTION_THUMB(REVSH,
	cpu->gprs[rd] =
		(int32_t) (int16_t) (uint16_t) (((m & 0xFF) << 8) | ((m >> 8) & 0xFF));)

// SETEND and CPS share a table slot (0xB640..0xB67F), so they are picked
// apart here. Both behave as they do in ARM state: CPS is a no-op, since
// touchHLE models neither the interrupt masks nor privilege changes, and
// SETEND BE is rejected rather than ignored because the prefetch and
// touchHLE's memory both assume a little-endian guest.
DEFINE_INSTRUCTION_THUMB(SETEND_CPS,
	if ((opcode & 0xFFF7) == 0xB650) {
		if (opcode & 0x0008) {
			ARM_ILL;
		}
	} else if ((opcode & 0xFFE8) != 0xB660) {
		ARM_ILL;
	})

// End ARMv6 additions

const ThumbInstruction _thumbTable[0x400] = {
	DECLARE_THUMB_EMITTER_BLOCK(_ThumbInstruction)
};
