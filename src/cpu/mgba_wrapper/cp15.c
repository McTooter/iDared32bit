/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* The slice of CP15, the system control coprocessor, that user-mode guest
 * code can reach. ARMv6 has no DMB, DSB or ISB instructions: barriers are
 * MCRs to CP15, and libstdc++ uses them for its atomics, so a guest reaches
 * this within moments of starting.
 *
 * This deliberately matches the CP15 behaviour of touchHLE's earlier CPU
 * core, which these guests are known to run on:
 *
 * - Every MCR and MRC succeeds. The registers below are modelled; any other
 *   read returns zero and any other write is ignored, and both are recorded
 *   so that src/cpu.rs can log them.
 * - An MRC with Rt == 15 copies the top four bits into the CPSR flags, and
 *   an MCR with Rt == 15 is ignored.
 * - CDP, LDC/STC and MCRR/MRRC to CP15 stay undefined.
 *
 * None of this is context-switched, which also matches the earlier core. */

#include "wrapper.h"

bool touchHLE_cp15_raw(struct ARMCore *cpu, uint32_t opcode) {
  if (((opcode >> 24) & 0xF) != 0xE || !(opcode & 0x10)) {
    return false;
  }

  struct touchHLE_MgbaWrapper *w = wrapper_of(cpu);
  bool read = (opcode >> 20) & 1;
  unsigned opc1 = (opcode >> 21) & 7;
  unsigned crn = (opcode >> 16) & 0xF;
  unsigned rt = (opcode >> 12) & 0xF;
  unsigned opc2 = (opcode >> 5) & 7;
  unsigned crm = opcode & 0xF;
  bool thread_regs = crn == 13 && opc1 == 0 && crm == 0;

  if (read) {
    uint32_t value = 0;
    if (thread_regs && opc2 == 2) {
      /* TPIDRURW, the user read/write thread ID register. */
      value = w->cp15_tpidrurw;
    } else if (thread_regs && opc2 == 3) {
      /* TPIDRURO, which only privileged code can set. touchHLE never does,
       * so it reads as zero. */
      value = 0;
    } else {
      w->unknown_cp15_opcode = opcode;
    }
    if (rt == ARM_PC) {
      cpu->cpsr.packed =
          (cpu->cpsr.packed & 0x0FFFFFFF) | (int32_t)(value & 0xF0000000u);
    } else {
      cpu->gprs[rt] = (int32_t)value;
    }
    return true;
  }

  if (rt == ARM_PC) {
    return true;
  }
  if (crn == 7 && opc1 == 0 && crm == 5 && opc2 == 4) {
    /* Flush Prefetch Buffer, ARMv6's ISB. The core has already fetched what
     * follows, so refetch it: code the guest has just written must be what
     * runs next. */
    if (cpu->executionMode == MODE_THUMB) {
      /* A Thumb-2 MCR, which runs with r15 at the next instruction (see
       * isa-thumb2.c), so branching there refetches. */
      ThumbWritePC(cpu);
    } else {
      /* ARM state, where r15 is two instructions ahead; refetch as MSR
       * does. */
      LOAD_32(cpu->prefetch[0],
              (cpu->gprs[ARM_PC] - WORD_SIZE_ARM) & cpu->memory.activeMask,
              cpu->memory.activeRegion);
      LOAD_32(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask,
              cpu->memory.activeRegion);
    }
  } else if (crn == 7 && opc1 == 0 && crm == 10 && (opc2 == 4 || opc2 == 5)) {
    /* Data Synchronization Barrier and Data Memory Barrier. One in-order
     * core with no cache leaves nothing to order. */
  } else if (thread_regs && opc2 == 2) {
    w->cp15_tpidrurw = (uint32_t)cpu->gprs[rt];
  } else {
    w->unknown_cp15_opcode = opcode;
  }
  return true;
}
