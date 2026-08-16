/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/arm/arm.h>

#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/arm/isa-thumb.h>

void ARMSetPrivilegeMode(struct ARMCore* cpu, enum PrivilegeMode mode) {
	if (mode == cpu->privilegeMode) {
		// Not switching modes after all
		return;
	}

	enum RegisterBank newBank = ARMSelectBank(mode);
	enum RegisterBank oldBank = ARMSelectBank(cpu->privilegeMode);
	if (newBank != oldBank) {
		// Switch banked registers
		if (mode == MODE_FIQ || cpu->privilegeMode == MODE_FIQ) {
			int oldFIQBank = oldBank == BANK_FIQ;
			int newFIQBank = newBank == BANK_FIQ;
			cpu->bankedRegisters[oldFIQBank][2] = cpu->gprs[8];
			cpu->bankedRegisters[oldFIQBank][3] = cpu->gprs[9];
			cpu->bankedRegisters[oldFIQBank][4] = cpu->gprs[10];
			cpu->bankedRegisters[oldFIQBank][5] = cpu->gprs[11];
			cpu->bankedRegisters[oldFIQBank][6] = cpu->gprs[12];
			cpu->gprs[8] = cpu->bankedRegisters[newFIQBank][2];
			cpu->gprs[9] = cpu->bankedRegisters[newFIQBank][3];
			cpu->gprs[10] = cpu->bankedRegisters[newFIQBank][4];
			cpu->gprs[11] = cpu->bankedRegisters[newFIQBank][5];
			cpu->gprs[12] = cpu->bankedRegisters[newFIQBank][6];
		}
		cpu->bankedRegisters[oldBank][0] = cpu->gprs[ARM_SP];
		cpu->bankedRegisters[oldBank][1] = cpu->gprs[ARM_LR];
		cpu->gprs[ARM_SP] = cpu->bankedRegisters[newBank][0];
		cpu->gprs[ARM_LR] = cpu->bankedRegisters[newBank][1];

		cpu->bankedSPSRs[oldBank] = cpu->spsr.packed;
		cpu->spsr.packed = cpu->bankedSPSRs[newBank];
	}
	cpu->privilegeMode = mode;
}

void ARMInit(struct ARMCore* cpu) {
	memset(cpu->cp, 0, sizeof(cpu->cp));
	cpu->master->init(cpu, cpu->master);
	size_t i;
	for (i = 0; i < cpu->numComponents; ++i) {
		if (cpu->components[i] && cpu->components[i]->init) {
			cpu->components[i]->init(cpu, cpu->components[i]);
		}
	}
}

void ARMDeinit(struct ARMCore* cpu) {
	if (cpu->master->deinit) {
		cpu->master->deinit(cpu->master);
	}
	size_t i;
	for (i = 0; i < cpu->numComponents; ++i) {
		if (cpu->components[i] && cpu->components[i]->deinit) {
			cpu->components[i]->deinit(cpu->components[i]);
		}
	}
}

void ARMSetComponents(struct ARMCore* cpu, struct mCPUComponent* master, int extra, struct mCPUComponent** extras) {
	cpu->master = master;
	cpu->numComponents = extra;
	cpu->components = extras;
}

void ARMHotplugAttach(struct ARMCore* cpu, size_t slot) {
	if (slot >= cpu->numComponents) {
		return;
	}
	cpu->components[slot]->init(cpu, cpu->components[slot]);
}

void ARMHotplugDetach(struct ARMCore* cpu, size_t slot) {
	if (slot >= cpu->numComponents) {
		return;
	}
	cpu->components[slot]->deinit(cpu->components[slot]);
}

void ARMReset(struct ARMCore* cpu) {
	int i;
	ARMInitStrictTable();
	for (i = 0; i < 16; ++i) {
		cpu->gprs[i] = 0;
	}
	for (i = 0; i < 6; ++i) {
		cpu->bankedRegisters[i][0] = 0;
		cpu->bankedRegisters[i][1] = 0;
		cpu->bankedRegisters[i][2] = 0;
		cpu->bankedRegisters[i][3] = 0;
		cpu->bankedRegisters[i][4] = 0;
		cpu->bankedRegisters[i][5] = 0;
		cpu->bankedRegisters[i][6] = 0;
		cpu->bankedSPSRs[i] = 0;
	}

	cpu->privilegeMode = MODE_SYSTEM;
	cpu->cpsr.packed = MODE_SYSTEM;
	cpu->spsr.packed = 0;

	cpu->shifterOperand = 0;
	cpu->shifterCarryOut = 0;

	cpu->executionMode = MODE_THUMB;
	_ARMSetMode(cpu, MODE_ARM);
	ARMWritePC(cpu);

	cpu->cycles = 0;
	cpu->nextEvent = 0;
	cpu->halted = 0;

	cpu->exclusiveAddress = 0;
	cpu->exclusiveMonitor = false;

	cpu->irqh.reset(cpu);
}

void ARMRaiseIRQ(struct ARMCore* cpu) {
	if (cpu->cpsr.i) {
		return;
	}
	union PSR cpsr = cpu->cpsr;
	int instructionWidth;
	if (cpu->executionMode == MODE_THUMB) {
		instructionWidth = WORD_SIZE_THUMB;
	} else {
		instructionWidth = WORD_SIZE_ARM;
	}
	ARMSetPrivilegeMode(cpu, MODE_IRQ);
	cpu->cpsr.priv = MODE_IRQ;
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC] - instructionWidth + WORD_SIZE_ARM;
	cpu->gprs[ARM_PC] = BASE_IRQ;
	_ARMSetMode(cpu, MODE_ARM);
	cpu->cycles += ARMWritePC(cpu);
	cpu->spsr = cpsr;
	cpu->cpsr.i = 1;
	cpu->halted = 0;
}

void ARMRaiseSWI(struct ARMCore* cpu) {
	union PSR cpsr = cpu->cpsr;
	int instructionWidth;
	if (cpu->executionMode == MODE_THUMB) {
		instructionWidth = WORD_SIZE_THUMB;
	} else {
		instructionWidth = WORD_SIZE_ARM;
	}
	ARMSetPrivilegeMode(cpu, MODE_SUPERVISOR);
	cpu->cpsr.priv = MODE_SUPERVISOR;
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC] - instructionWidth;
	cpu->gprs[ARM_PC] = BASE_SWI;
	_ARMSetMode(cpu, MODE_ARM);
	cpu->cycles += ARMWritePC(cpu);
	cpu->spsr = cpsr;
	cpu->cpsr.i = 1;
}

void ARMRaiseUndefined(struct ARMCore* cpu) {
	union PSR cpsr = cpu->cpsr;
	int instructionWidth;
	if (cpu->executionMode == MODE_THUMB) {
		instructionWidth = WORD_SIZE_THUMB;
	} else {
		instructionWidth = WORD_SIZE_ARM;
	}
	ARMSetPrivilegeMode(cpu, MODE_UNDEFINED);
	cpu->cpsr.priv = MODE_UNDEFINED;
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC] - instructionWidth;
	cpu->gprs[ARM_PC] = BASE_UNDEF;
	_ARMSetMode(cpu, MODE_ARM);
	cpu->cycles += ARMWritePC(cpu);
	cpu->spsr = cpsr;
	cpu->cpsr.i = 1;
}

static const uint16_t conditionLut[16] = {
	0xF0F0, // EQ [-Z--]
	0x0F0F, // NE [-z--]
	0xCCCC, // CS [--C-]
	0x3333, // CC [--c-]
	0xFF00, // MI [N---]
	0x00FF, // PL [n---]
	0xAAAA, // VS [---V]
	0x5555, // VC [---v]
	0x0C0C, // HI [-zC-]
	0xF3F3, // LS [-Z--] || [--c-]
	0xAA55, // GE [N--V] || [n--v]
	0x55AA, // LT [N--v] || [n--V]
	0x0A05, // GT [Nz-V] || [nz-v]
	0xF5FA, // LE [-Z--] || [Nz-v] || [nz-V]
	0xFFFF, // AL [----]
	0x0000 // NV
};

#define STRICT_FIELD(HI, LO) ((opcode >> (LO)) & ((1u << ((HI) - (LO) + 1)) - 1))
#define STRICT_ONES(HI, LO) ((1u << ((HI) - (LO) + 1)) - 1)

// Whether an instruction whose row has check `kind` passes it.
static inline bool ARMStrictCheck(struct ARMCore* cpu, uint32_t opcode, unsigned kind) {
	bool user = !_ARMModeHasSPSR(cpu->cpsr.priv);
	switch (kind) {
	case STRICT_DP_S:
		return !(user && STRICT_FIELD(15, 12) == ARM_PC);
	case STRICT_DP_CMP:
		return STRICT_FIELD(15, 12) == 0;
	case STRICT_DP_MOV:
		return STRICT_FIELD(19, 16) == 0;
	case STRICT_DP_MOV_S:
		return STRICT_FIELD(19, 16) == 0 && !(user && STRICT_FIELD(15, 12) == ARM_PC);
	case STRICT_SBZ_15_12:
		return STRICT_FIELD(15, 12) == 0;
	case STRICT_SBZ_11_8:
		return STRICT_FIELD(11, 8) == 0;
	case STRICT_LDRD_REG:
		if (STRICT_FIELD(11, 8) != 0) {
			return false;
		}
		// Fall through.
	case STRICT_LDRD_IMM:
		return !(opcode & 0x1000) && !((opcode & 0x01200000) == 0x00200000);
	case STRICT_SBO_11_8:
		return STRICT_FIELD(11, 8) == STRICT_ONES(11, 8);
	case STRICT_STREX: {
		// No PC, and the status register must differ from the others; the
		// doubleword form takes an even pair short of the PC.
		unsigned rd = STRICT_FIELD(15, 12), rt = STRICT_FIELD(3, 0), rn = STRICT_FIELD(19, 16);
		bool dual = STRICT_FIELD(22, 21) == 1;
		if (STRICT_FIELD(11, 8) != STRICT_ONES(11, 8) || rd == ARM_PC || rt == ARM_PC || rn == ARM_PC ||
		    rd == rn || rd == rt) {
			return false;
		}
		return !dual || (!(rt & 1) && rt != ARM_LR && rd != rt + 1);
	}
	case STRICT_SBO_19_16_11_8:
		return STRICT_FIELD(19, 16) == STRICT_ONES(19, 16) && STRICT_FIELD(11, 8) == STRICT_ONES(11, 8);
	case STRICT_SBZ_9_8:
		return STRICT_FIELD(9, 8) == 0;
	case STRICT_SBO_15_12:
		return STRICT_FIELD(15, 12) == STRICT_ONES(15, 12);
	case STRICT_LDREX: {
		unsigned rt = STRICT_FIELD(15, 12), rn = STRICT_FIELD(19, 16);
		bool dual = STRICT_FIELD(22, 21) == 1;
		if (STRICT_FIELD(11, 8) != STRICT_ONES(11, 8) || STRICT_FIELD(3, 0) != STRICT_ONES(3, 0) ||
		    rt == ARM_PC || rn == ARM_PC) {
			return false;
		}
		return !dual || (!(rt & 1) && rt != ARM_LR);
	}
	case STRICT_BX:
		return STRICT_FIELD(19, 8) == STRICT_ONES(19, 8);
	case STRICT_MRS:
		return STRICT_FIELD(19, 16) == STRICT_ONES(19, 16) && STRICT_FIELD(11, 0) == 0 &&
		    !(user && (opcode & 0x00400000));
	case STRICT_MSR_REG:
		return STRICT_FIELD(15, 12) == STRICT_ONES(15, 12) && STRICT_FIELD(11, 8) == 0 &&
		    !(user && (opcode & 0x00400000));
	case STRICT_MSR_IMM:
		return STRICT_FIELD(15, 12) == STRICT_ONES(15, 12) && !(user && (opcode & 0x00400000));
	case STRICT_LSM: {
		// Not based on the PC, not empty, not ^ (the user registers, or an
		// exception return) from user mode, and for LDM with writeback, not
		// loading the base.
		unsigned rn = STRICT_FIELD(19, 16);
		bool load = opcode & 0x00100000;
		bool writeback = opcode & 0x00200000;
		if (rn == ARM_PC || !(opcode & 0xFFFF) || ((opcode & 0x00400000) && user)) {
			return false;
		}
		return !(load && writeback && ((opcode >> rn) & 1));
	}
	default:
		return true;
	}
}


static inline void ARMStep(struct ARMCore* cpu) {
	uint32_t opcode = cpu->prefetch[0];
	cpu->prefetch[0] = cpu->prefetch[1];
	cpu->gprs[ARM_PC] += WORD_SIZE_ARM;
	LOAD_32(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);

	unsigned condition = opcode >> 28;
	if (condition == 0xF) {
		ARMStepUnconditional(cpu, opcode);
		return;
	}
	if (condition != 0xE) {
		unsigned flags = cpu->cpsr.flags >> 4;
		bool conditionMet = conditionLut[condition] & (1 << flags);
		if (!conditionMet) {
			cpu->cycles += ARM_PREFETCH_CYCLES;
			return;
		}
	}
	unsigned index = ((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0x00F);
	unsigned strict = _armStrictTable[index];
	if (UNLIKELY(strict) && !ARMStrictCheck(cpu, opcode, strict)) {
		cpu->irqh.hitIllegal(cpu, opcode);
		return;
	}
	ARMInstruction instruction = _armTable[index];
	instruction(cpu, opcode);
}

// Whether a 16-bit Thumb instruction sets the flags outside an IT block but
// not inside one: the data-processing encodings other than CMP, CMN and TST,
// which set them regardless. Everything below 0x4400 besides those.
static bool _ThumbSetsFlagsOutsideIT(unsigned opcode) {
	if (opcode >= 0x4400) {
		return false;
	}
	if (opcode >= 0x4000) {
		unsigned op = (opcode >> 6) & 0xF;
		return op != 0x8 && op != 0xA && op != 0xB;
	}
	return opcode < 0x2800 || opcode >= 0x3000;
}

// One instruction of an IT block. Its condition comes from the IT state; if
// it fails, the instruction is skipped, including the second halfword of a
// 32-bit one. 16-bit instructions that set the flags outside an IT block do
// not inside one, so the flags are put back afterwards. Then the state
// advances, reaching zero after the last instruction.
static void ThumbStepIT(struct ARMCore* cpu, unsigned opcode) {
	unsigned state = _ThumbITState(cpu);
	if (!ARMTestCondition(cpu, state >> 4)) {
		if (opcode >= 0xE800) {
			cpu->prefetch[0] = cpu->prefetch[1];
			cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
			LOAD_16(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
		}
		cpu->cycles += 1 + cpu->memory.activeSeqCycles16;
	} else if (_ThumbSetsFlagsOutsideIT(opcode)) {
		uint32_t flags = (uint32_t) cpu->cpsr.packed & 0xF0000000;
		_thumbTable[opcode >> 6](cpu, opcode);
		cpu->cpsr.packed = (int32_t) (((uint32_t) cpu->cpsr.packed & 0x0FFFFFFF) | flags);
	} else {
		_thumbTable[opcode >> 6](cpu, opcode);
	}
	_ThumbSetITState(cpu, (state & 0x7) ? (state & 0xE0) | ((state << 1) & 0x1F) : 0);
}

static inline void ThumbStep(struct ARMCore* cpu) {
	uint32_t opcode = cpu->prefetch[0];
	cpu->prefetch[0] = cpu->prefetch[1];
	cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
	LOAD_16(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
	if (UNLIKELY(cpu->cpsr.packed & ARM_IT_MASK)) {
		ThumbStepIT(cpu, opcode);
		return;
	}
	ThumbInstruction instruction = _thumbTable[opcode >> 6];
	instruction(cpu, opcode);
}

void ARMRun(struct ARMCore* cpu) {
	while (cpu->cycles >= cpu->nextEvent) {
		cpu->irqh.processEvents(cpu);
	}
	if (cpu->executionMode == MODE_THUMB) {
		ThumbStep(cpu);
	} else {
		ARMStep(cpu);
	}
	while (cpu->cycles >= cpu->nextEvent) {
		cpu->irqh.processEvents(cpu);
	}
}

void ARMRunLoop(struct ARMCore* cpu) {
	if (cpu->executionMode == MODE_THUMB) {
		while (cpu->cycles < cpu->nextEvent) {
			ThumbStep(cpu);
		}
	} else {
		while (cpu->cycles < cpu->nextEvent) {
			ARMStep(cpu);
		}
	}
	cpu->irqh.processEvents(cpu);
}

void ARMRunFake(struct ARMCore* cpu, uint32_t opcode) {
	if (cpu->executionMode == MODE_ARM) {
		cpu->gprs[ARM_PC] -= WORD_SIZE_ARM;
	} else {
		cpu->gprs[ARM_PC] -= WORD_SIZE_THUMB;
	}
	cpu->prefetch[1] = cpu->prefetch[0];
	cpu->prefetch[0] = opcode;
}
