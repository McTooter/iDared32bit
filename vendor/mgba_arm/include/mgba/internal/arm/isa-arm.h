/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ISA_ARM_H
#define ISA_ARM_H

#include <mgba-util/common.h>

CXX_GUARD_START

#define ARM_PREFETCH_CYCLES (1 + cpu->memory.activeSeqCycles32)

struct ARMCore;

typedef void (*ARMInstruction)(struct ARMCore*, uint32_t opcode);
void ARMStepUnconditional(struct ARMCore* cpu, uint32_t opcode);

extern const ARMInstruction _armTable[0x1000];

// Per-row checks of should-be-zero and should-be-one fields, which upstream
// does not make; see
// ARMInitStrictTable in isa-arm.c, and ARMStrictCheck in arm.c. Indexed like
// _armTable.
enum ARMStrictKind {
	STRICT_NONE,
	STRICT_DP_S,        // SUBS: Rd == 15 is an exception return
	STRICT_DP_CMP,      // TST/TEQ/CMP/CMN: Rd is SBZ
	STRICT_DP_MOV,      // MOV/MVN: Rn is SBZ
	STRICT_DP_MOV_S,    // both of the above
	STRICT_SBZ_15_12,   // MUL, SMULxy, SMULWy
	STRICT_SBZ_11_8,    // QADD etc., SWP, and register-offset LDRH etc.
	STRICT_LDRD_REG,    // bits 11:8 SBZ, Rt even, and not P == 0 with W
	STRICT_LDRD_IMM,    // Rt even, and not P == 0 with W
	STRICT_SBO_11_8,    // parallel add/subtract, SEL, SSAT16, USAT16
	STRICT_SBO_19_16_11_8, // REV and friends, CLZ
	STRICT_SBZ_9_8,     // extends
	STRICT_SBO_15_12,   // SDIV/UDIV, MSR (immediate) and the hints
	STRICT_STREX,       // bits 11:8 SBO, and the register rules below
	STRICT_LDREX,       // bits 11:8 and 3:0 SBO, and the register rules
	STRICT_BX,          // bits 19:8 SBO
	STRICT_MRS,         // bits 19:16 SBO, 11:0 SBZ; no SPSR from user mode
	STRICT_MSR_REG,     // bits 15:12 SBO, 11:8 SBZ; no SPSR from user mode
	STRICT_MSR_IMM,     // bits 15:12 SBO; no SPSR from user mode
	STRICT_LSM,         // LDM/STM: see ARMStrictCheck
};

extern uint8_t _armStrictTable[0x1000];
void ARMInitStrictTable(void);

CXX_GUARD_END

#endif
