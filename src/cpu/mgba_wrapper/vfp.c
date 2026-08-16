/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* VFPv3 for the mGBA ARM core, which has no FPU of its own (the GBA's
 * ARM7TDMI had none). VFPv3-D32 is what the Cortex-A8 of the iPhone 3GS and
 * later has; the VFPv2 of the original iPhone's ARM1176 is a subset of it, so
 * armv6 code runs unchanged. This attaches to coprocessors 10 and 11 through
 * the `raw` hook the vendored core grew for the purpose, because VFP encodings
 * scatter the D, N and M register-extension bits across the word and do not
 * survive mGBA's CRn/CRm/opcode1/opcode2 decomposition.
 *
 * Arithmetic uses the host's float and double rather than a soft-float
 * implementation: every platform touchHLE targets is IEEE-754. The
 * architecture's NaN rules, flush-to-zero, rounding modes and short vectors
 * are applied around it; see "Data processing" below, and README.md for
 * what is not modelled. */

#include <fenv.h>
#include <math.h>

#include "fp.h"

/* VFPv3 does not fuse multiply-accumulate: VMLA rounds the product before
 * adding it, so contraction into an FMA would give a result one rounding
 * step too accurate. build.rs passes -ffp-contract=off for this. The
 * standard `#pragma STDC FP_CONTRACT OFF` is not used because GCC warns
 * that it ignores it. */

/* Register numbering. A single-precision register number is the 4-bit field
 * with the extension bit as its *low* bit; a double-precision one has it as
 * the *high* bit, which reaches D16 to D31. Getting these the wrong way round
 * is the classic VFP decoding bug, so they are named rather than
 * open-coded. */
#define VFP_SREG(FIELD, BIT) (((FIELD) << 1) | (BIT))
#define VFP_DREG(FIELD, BIT) ((FIELD) | ((BIT) << 4))

#define VFP_VD ((opcode >> 12) & 0xF)
#define VFP_VN ((opcode >> 16) & 0xF)
#define VFP_VM (opcode & 0xF)
#define VFP_D ((opcode >> 22) & 1)
#define VFP_N ((opcode >> 7) & 1)
#define VFP_M ((opcode >> 5) & 1)

/* Coprocessor 11 is the double-precision view, 10 the single-precision one. */
#define VFP_IS_DOUBLE (((opcode >> 8) & 0xF) == 11)

static struct touchHLE_Vfp *vfp_of(struct ARMCore *cpu) {
  return &wrapper_of(cpu)->vfp;
}

void touchHLE_vfp_reset(struct touchHLE_Vfp *vfp) {
  memset(&vfp->regs, 0, sizeof(vfp->regs));
  vfp->fpscr = TOUCHHLE_DEFAULT_FPSCR;
  vfp->fpexc = 0;
  /* The Cortex-A15's: ARM's VFPv4, with the common VFP subarchitecture v3.
   * The core implements what the armv7s A6 has, VFPv4 among it (see
   * VFP_MVFR1), so this identifies a VFPv4 part. No real VFPv4 core also
   * has short vectors, as this one does; nothing identifies such a chip.
   * User-mode code cannot read FPSID anyway (see vfp_system_reg). */
  vfp->fpsid = 0x410430F0;
}

/* The media and VFP feature registers, as VMRS can read them from user mode.
 * These describe what is implemented here, which is what the Cortex-A8 has:
 * 32 double registers, single and double precision, divide, square root,
 * short vectors and all the rounding modes; flush-to-zero and default NaN;
 * Advanced SIMD's loads and stores, integer and single-precision
 * instructions (neon.c); and, beyond the Cortex-A8, the armv7s A6's fused
 * multiply-accumulates and half-precision conversions (VFPv4 and NEONv2).
 * There is no exception trapping. */
#define VFP_MVFR0 0x11110222u
#define VFP_MVFR1 0x11111111u

/* Two-register transfers between core and VFP registers: the MCRR/MRRC
 * encoding space, which carries VMOV in its 64-bit forms.
 *
 *   cond 1100 010 L Rt2 Rt 101 sz 00 M 1 Vm
 *
 * On cp11 this is one double register; on cp10 it is a pair of consecutive
 * single registers, low word first in both cases. */
static bool vfp_transfer_two(struct ARMCore *cpu, uint32_t opcode) {
  struct touchHLE_Vfp *vfp = vfp_of(cpu);
  bool to_core = (opcode >> 20) & 1;
  int rt = (opcode >> 12) & 0xF;
  int rt2 = (opcode >> 16) & 0xF;

  /* Bits 7:6 are zero and bit 4 one; anything else is not a VMOV. */
  if ((opcode & 0xD0) != 0x10) {
    return false;
  }

  if (VFP_IS_DOUBLE) {
    int dm = VFP_DREG(VFP_VM, VFP_M);
    if (to_core) {
      uint64_t v = vfp->regs.d[dm];
      touchHLE_write_core_reg(cpu, rt, (uint32_t)v);
      touchHLE_write_core_reg(cpu, rt2, (uint32_t)(v >> 32));
    } else {
      vfp->regs.d[dm] =
          ((uint64_t)(uint32_t)cpu->gprs[rt2] << 32) | (uint32_t)cpu->gprs[rt];
    }
  } else {
    int sm = VFP_SREG(VFP_VM, VFP_M);
    /* The second register is Sm+1, which must exist. */
    if (sm + 1 >= 32) {
      return false;
    }
    if (to_core) {
      uint32_t lo = vfp->regs.s[sm], hi = vfp->regs.s[sm + 1];
      touchHLE_write_core_reg(cpu, rt, lo);
      touchHLE_write_core_reg(cpu, rt2, hi);
    } else {
      vfp->regs.s[sm] = (uint32_t)cpu->gprs[rt];
      vfp->regs.s[sm + 1] = (uint32_t)cpu->gprs[rt2];
    }
  }
  return true;
}

/* VMOV between a core register and a single-precision register:
 *
 *   cond 1110 000 L Vn Rt 1010 N 001 0000
 */
static bool vfp_transfer_one(struct ARMCore *cpu, uint32_t opcode) {
  struct touchHLE_Vfp *vfp = vfp_of(cpu);
  bool to_core = (opcode >> 20) & 1;
  int rt = (opcode >> 12) & 0xF;
  int sn = VFP_SREG(VFP_VN, VFP_N);

  if (to_core && rt == ARM_PC) {
    /* UNPREDICTABLE. Taken as VMRS APSR_nzcv does, copying the top four
     * bits to the condition flags. */
    cpu->cpsr.packed = (int32_t)(((uint32_t)cpu->cpsr.packed & ~FPSCR_NZCV) |
                                 (vfp->regs.s[sn] & FPSCR_NZCV));
  } else if (to_core) {
    touchHLE_write_core_reg(cpu, rt, vfp->regs.s[sn]);
  } else {
    vfp->regs.s[sn] = (uint32_t)cpu->gprs[rt];
  }
  return true;
}

/* VMSR and VMRS:
 *
 *   cond 1110 111 L reg Rt 1010 0001 0000
 *
 * Rt == 15 on a VMRS of FPSCR is `VMRS APSR_nzcv, FPSCR`, which moves the
 * comparison flags into CPSR rather than writing a register. That is how
 * every guest float comparison reaches a conditional branch.
 *
 * FPSCR is the only one of these registers user-mode code may access; the
 * others (FPSID, MVFR0, MVFR1 and FPEXC) are privileged, and undefined from
 * user mode, which is where touchHLE's guests run. So on a real device an app
 * reading FPSID gets SIGILL, and so does a guest here. */
static bool vfp_system_reg(struct ARMCore *cpu, uint32_t opcode) {
  struct touchHLE_Vfp *vfp = vfp_of(cpu);
  bool read = (opcode >> 20) & 1;
  int reg = (opcode >> 16) & 0xF;
  int rt = (opcode >> 12) & 0xF;

  if (reg != 1 && cpu->cpsr.priv == MODE_USER) {
    return false;
  }

  if (read) {
    if (rt == ARM_PC) {
      if (reg != 1) {
        return false;
      }
      cpu->cpsr.packed =
          (cpu->cpsr.packed & ~(int32_t)FPSCR_NZCV) | (vfp->fpscr & FPSCR_NZCV);
      return true;
    }
    switch (reg) {
    case 0:
      cpu->gprs[rt] = (int32_t)vfp->fpsid;
      return true;
    case 1:
      cpu->gprs[rt] = (int32_t)vfp->fpscr;
      return true;
    case 6:
      cpu->gprs[rt] = (int32_t)VFP_MVFR1;
      return true;
    case 7:
      cpu->gprs[rt] = (int32_t)VFP_MVFR0;
      return true;
    case 8:
      cpu->gprs[rt] = (int32_t)vfp->fpexc;
      return true;
    default:
      return false;
    }
  }

  if (rt == ARM_PC) {
    return false;
  }
  switch (reg) {
  case 0:
    /* FPSID is read-only. */
    return true;
  case 1:
    vfp->fpscr = (uint32_t)cpu->gprs[rt];
    return true;
  case 8:
    vfp->fpexc = (uint32_t)cpu->gprs[rt];
    return true;
  default:
    return false;
  }
}

/* Coprocessor load/store, which is VLDR, VSTR, VLDM, VSTM, VPUSH and VPOP:
 *
 *   cond 110 P U D W L Rn Vd 101 sz imm8
 *
 * A single transfer is P set with W clear; everything else is a multiple.
 * (P, U, W) == (0, 0, 0) is the MCRR/MRRC space and never reaches here. */
static bool vfp_load_store(struct ARMCore *cpu, uint32_t opcode) {
  struct touchHLE_Vfp *vfp = vfp_of(cpu);
  bool p = (opcode >> 24) & 1;
  bool u = (opcode >> 23) & 1;
  bool w = (opcode >> 21) & 1;
  bool load = (opcode >> 20) & 1;
  int rn = (opcode >> 16) & 0xF;
  unsigned imm8 = opcode & 0xFF;
  bool is_double = VFP_IS_DOUBLE;
  int cycles = 0;

  if (p && u && w) {
    return false;
  }
  /* A list form is only IA (P clear, U set) or DB (P set, U clear), and
   * writeback onto the PC is not a thing. */
  if (!(p && !w) && (p == u)) {
    return false;
  }
  if (w && rn == ARM_PC) {
    return false;
  }

  uint32_t base = (uint32_t)cpu->gprs[rn];
  if (rn == ARM_PC) {
    /* PC reads as the instruction address plus 8 in ARM state and plus 4
     * in Thumb, and a literal pool reference is word-aligned. */
    base &= ~3u;
  }

  if (p && !w) {
    /* VLDR/VSTR: an 8-bit offset scaled by 4, added or subtracted. */
    uint32_t address = u ? base + imm8 * 4 : base - imm8 * 4;
    if (is_double) {
      int dd = VFP_DREG(VFP_VD, VFP_D);
      if (load) {
        uint32_t lo = cpu->memory.load32(cpu, address, &cycles);
        uint32_t hi = cpu->memory.load32(cpu, address + 4, &cycles);
        vfp->regs.d[dd] = ((uint64_t)hi << 32) | lo;
      } else {
        cpu->memory.store32(cpu, address, (int32_t)(uint32_t)vfp->regs.d[dd],
                            &cycles);
        cpu->memory.store32(cpu, address + 4,
                            (int32_t)(uint32_t)(vfp->regs.d[dd] >> 32),
                            &cycles);
      }
    } else {
      int sd = VFP_SREG(VFP_VD, VFP_D);
      if (load) {
        vfp->regs.s[sd] = cpu->memory.load32(cpu, address, &cycles);
      } else {
        cpu->memory.store32(cpu, address, (int32_t)vfp->regs.s[sd], &cycles);
      }
    }
    return true;
  }

  /* VLDM/VSTM. imm8 counts words transferred, so a double-precision list is
   * half as long. An odd count on cp11 is FLDMX/FSTMX, which transfers one
   * further word of implementation-defined format after the registers; the
   * toolchains of the era emit it to save VFP state. That word needs no
   * handling -- the writeback below counts it either way, and a matching
   * FSTMX/FLDMX pair round-trips the registers regardless of what is in it
   * -- so the only difference is that the register count rounds down. */
  unsigned count = is_double ? imm8 / 2 : imm8;
  if (count == 0) {
    return false;
  }

  int first = is_double ? VFP_DREG(VFP_VD, VFP_D) : VFP_SREG(VFP_VD, VFP_D);
  if (first + count > 32 || (is_double && count > 16)) {
    return false;
  }

  /* Decreasing lists start below the base; the transfer itself always runs
   * upwards from the lowest address. */
  uint32_t address = u ? base : base - imm8 * 4;
  for (unsigned i = 0; i < count; ++i) {
    if (is_double) {
      if (load) {
        uint32_t lo = cpu->memory.load32(cpu, address, &cycles);
        uint32_t hi = cpu->memory.load32(cpu, address + 4, &cycles);
        vfp->regs.d[first + i] = ((uint64_t)hi << 32) | lo;
      } else {
        cpu->memory.store32(cpu, address,
                            (int32_t)(uint32_t)vfp->regs.d[first + i], &cycles);
        cpu->memory.store32(cpu, address + 4,
                            (int32_t)(uint32_t)(vfp->regs.d[first + i] >> 32),
                            &cycles);
      }
      address += 8;
    } else {
      if (load) {
        vfp->regs.s[first + i] = cpu->memory.load32(cpu, address, &cycles);
      } else {
        cpu->memory.store32(cpu, address, (int32_t)vfp->regs.s[first + i],
                            &cycles);
      }
      address += 4;
    }
  }

  if (w) {
    cpu->gprs[rn] = (int32_t)(u ? base + imm8 * 4 : base - imm8 * 4);
  }
  return true;
}

/* ---- Data processing ---- */

/* FPSCR's rounding mode governs every operation that rounds, except the
 * conversions to integer and fixed point that always round toward zero, and
 * VCVTR, which does its own (see vfp_round). The host's own rounding mode is
 * set to match for the duration of the instruction and put back afterwards,
 * so it is only touched when the mode is not round-to-nearest, the reset
 * state and what nearly all code uses. build.rs passes -frounding-math, which
 * stops the compiler assuming the mode is always the default. */
static int vfp_enter_rounding(const struct touchHLE_Vfp *vfp) {
  static const int host_modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD,
                                    FE_TOWARDZERO};
  unsigned mode = FPSCR_RMODE(vfp->fpscr);
  if (!mode) {
    return -1;
  }
  int saved = fegetround();
  fesetround(host_modes[mode]);
  return saved;
}

static void vfp_leave_rounding(int saved) {
  if (saved >= 0) {
    fesetround(saved);
  }
}

/* Short vectors. With FPSCR's LEN nonzero, the data-processing instructions
 * that have a vector form work on LEN+1 registers, stepping by the stride
 * (1, or 2 when STRIDE is 0b11) and wrapping within a bank: eight single
 * registers or four double registers. The first bank is the scalar bank. A
 * destination there makes the whole operation scalar, as it is when LEN is
 * zero; an Rm there is used as a scalar against a vector Rn and Rd. The
 * ARM11-era "vfpmathlibrary" used this for 4x4 matrix work.
 *
 * Double registers are banked on their low four bits, so D16 to D19 also
 * count as a scalar bank, which ARM11 code cannot notice, having no D16 to
 * D31 to use. */
static bool vfp_in_scalar_bank(int reg, bool is_double) {
  return is_double ? !(reg & 0xC) : !(reg & 0x18);
}

static unsigned vfp_vector_length(const struct touchHLE_Vfp *vfp, int vd,
                                  bool is_double) {
  return vfp_in_scalar_bank(vd, is_double) ? 1 : FPSCR_LEN(vfp->fpscr) + 1;
}

static int vfp_vector_next(const struct touchHLE_Vfp *vfp, int reg,
                           bool is_double) {
  int stride = FPSCR_STRIDE(vfp->fpscr) == 3 ? 2 : 1;
  int mask = is_double ? 3 : 7;
  return (reg & ~mask) | ((reg + stride) & mask);
}

static void vfp_compare(struct touchHLE_Vfp *vfp, double a, double b,
                        bool unordered) {
  uint32_t nzcv;
  if (unordered) {
    nzcv = FPSCR_C | FPSCR_V;
  } else if (a == b) {
    nzcv = FPSCR_Z | FPSCR_C;
  } else if (a < b) {
    nzcv = FPSCR_N;
  } else {
    nzcv = FPSCR_C;
  }
  vfp->fpscr = (vfp->fpscr & ~FPSCR_NZCV) | nzcv;
}

/* VCVT rounds toward zero; VCVTR uses FPSCR's rounding mode, applied here
 * directly rather than through the host's (so nearbyint sees the host's
 * default of round-to-nearest-even, which is VFP's mode 0). */
static double vfp_round(const struct touchHLE_Vfp *vfp, double v,
                        bool to_zero) {
  if (to_zero) {
    return trunc(v);
  }
  switch (FPSCR_RMODE(vfp->fpscr)) {
  case 0:
    return nearbyint(v);
  case 1:
    return ceil(v);
  case 2:
    return floor(v);
  default:
    return trunc(v);
  }
}

/* VFPExpandImm: VMOV's 8-bit immediate is a sign, a 3-bit exponent and a
 * 4-bit fraction, which covers the small constants compilers want most. */
static uint64_t vfp_expand_imm(unsigned imm8, bool is_double) {
  uint64_t sign = imm8 >> 7;
  uint64_t b6 = (imm8 >> 6) & 1;
  uint64_t low_exp = (imm8 >> 4) & 3;
  uint64_t frac = imm8 & 0xF;
  if (is_double) {
    uint64_t exp = ((b6 ^ 1) << 10) | (b6 ? 0xFFull << 2 : 0) | low_exp;
    return (sign << 63) | (exp << 52) | (frac << 48);
  }
  uint64_t exp = ((b6 ^ 1) << 7) | (b6 ? 0x1Full << 2 : 0) | low_exp;
  return (sign << 31) | (exp << 23) | (frac << 19);
}

/* Out-of-range conversions saturate and raise Invalid Operation, where C
 * would leave the result undefined. */
static int32_t vfp_to_s32(struct touchHLE_Vfp *vfp, double v, bool to_zero) {
  if (isnan(v)) {
    vfp->fpscr |= FPSCR_IOC;
    return 0;
  }
  double r = vfp_round(vfp, v, to_zero);
  if (r >= 2147483648.0) {
    vfp->fpscr |= FPSCR_IOC;
    return INT32_MAX;
  }
  if (r < -2147483648.0) {
    vfp->fpscr |= FPSCR_IOC;
    return INT32_MIN;
  }
  return (int32_t)r;
}

static uint32_t vfp_to_u32(struct touchHLE_Vfp *vfp, double v, bool to_zero) {
  if (isnan(v)) {
    vfp->fpscr |= FPSCR_IOC;
    return 0;
  }
  double r = vfp_round(vfp, v, to_zero);
  if (r >= 4294967296.0) {
    vfp->fpscr |= FPSCR_IOC;
    return UINT32_MAX;
  }
  if (r < 0.0) {
    vfp->fpscr |= FPSCR_IOC;
    return 0;
  }
  return (uint32_t)r;
}

/* A source for the integer and fixed-point conversions: flushed if FZ is
 * set, and widened to double, which is exact. */
static double vfp_source(struct touchHLE_Vfp *vfp, int reg, bool is_double) {
  return is_double ? vfp_flush_in_d(vfp, vfp_get_d(vfp, reg))
                   : (double)vfp_flush_in_s(vfp, vfp_get_s(vfp, reg));
}

/* VCVT between the precisions, following FPConvert: a NaN is quietened (or
 * replaced, with DN), keeping the top of its payload as the host's
 * conversion does, and the narrowing direction rounds and may be flushed. */
static void vfp_convert_precision(struct touchHLE_Vfp *vfp, uint32_t opcode,
                                  bool from_double) {
  if (from_double) {
    double v = vfp_flush_in_d(vfp, vfp_get_d(vfp, VFP_DREG(VFP_VM, VFP_M)));
    uint64_t bits = vfp_bits_d(v);
    float r;
    if (vfp_is_nan_d(bits)) {
      vfp->fpscr |= vfp_is_snan_d(bits) ? FPSCR_IOC : 0;
      r = (vfp->fpscr & FPSCR_DN)
              ? vfp_from_bits_s(0x7FC00000u)
              : vfp_from_bits_s((uint32_t)((bits >> 63) << 31) | 0x7FC00000u |
                                (uint32_t)((bits >> 29) & 0x003FFFFFu));
    } else if ((vfp->fpscr & FPSCR_FZ) && v != 0 && fabs(v) < FLT_MIN) {
      /* Tiny before rounding, which (float)v may have rounded up. */
      vfp->fpscr |= FPSCR_UFC;
      r = vfp_from_bits_s((uint32_t)(bits >> 63) << 31);
    } else {
      r = vfp_result_s(vfp, (float)v);
    }
    vfp_set_s(vfp, VFP_SREG(VFP_VD, VFP_D), r);
  } else {
    float v = vfp_flush_in_s(vfp, vfp_get_s(vfp, VFP_SREG(VFP_VM, VFP_M)));
    uint32_t bits = vfp_bits_s(v);
    double r;
    if (vfp_is_nan_s(bits)) {
      vfp->fpscr |= vfp_is_snan_s(bits) ? FPSCR_IOC : 0;
      r = (vfp->fpscr & FPSCR_DN)
              ? vfp_from_bits_d(0x7FF8000000000000ull)
              : vfp_from_bits_d(((uint64_t)(bits >> 31) << 63) |
                                0x7FF8000000000000ull |
                                ((uint64_t)(bits & 0x003FFFFFu) << 29));
    } else {
      r = (double)v;
    }
    vfp_set_d(vfp, VFP_DREG(VFP_VD, VFP_D), r);
  }
}

/* The single-operand group, where the Vn field is an opcode rather than a
 * register:
 *
 *   cond 11101 D 11 opc2 Vd 101 sz opc3 M 0 Vm
 *
 * VMOV, VABS, VNEG and VSQRT have vector forms; the comparisons and
 * conversions are always scalar. Note that VCVT between the two precisions,
 * and to and from integers, has a source and destination of different
 * sizes, so those register numbers are worked out per case. */
static bool vfp_other(struct ARMCore *cpu, uint32_t opcode, bool is_double) {
  struct touchHLE_Vfp *vfp = vfp_of(cpu);
  unsigned opc2 = (opcode >> 16) & 0xF;
  bool high = (opcode >> 7) & 1;
  int vd = is_double ? VFP_DREG(VFP_VD, VFP_D) : VFP_SREG(VFP_VD, VFP_D);
  int vm = is_double ? VFP_DREG(VFP_VM, VFP_M) : VFP_SREG(VFP_VM, VFP_M);

  if (!((opcode >> 6) & 1)) {
    /* VMOV immediate, with imm4H in the opc2 field. Bits 7, 5 and 4 are
     * should-be-zero. */
    if (opcode & 0xB0) {
      return false;
    }
    uint64_t imm = vfp_expand_imm((opc2 << 4) | (opcode & 0xF), is_double);
    unsigned len = vfp_vector_length(vfp, vd, is_double);
    for (unsigned i = 0; i < len; i++) {
      if (is_double) {
        vfp->regs.d[vd] = imm;
      } else {
        vfp->regs.s[vd] = (uint32_t)imm;
      }
      vd = vfp_vector_next(vfp, vd, is_double);
    }
    return true;
  }

  if (opc2 <= 1) {
    /* VMOV (register) and VABS, VNEG and VSQRT. The first three are bit
     * operations, so a NaN keeps its payload and FZ does not apply. */
    unsigned len = vfp_vector_length(vfp, vd, is_double);
    bool m_vector = !vfp_in_scalar_bank(vm, is_double);
    for (unsigned i = 0; i < len; i++) {
      if (opc2 == 1 && high) {
        if (is_double) {
          vfp_set_d(vfp, vd, vfp_sqrt_d(vfp, vfp_get_d(vfp, vm)));
        } else {
          vfp_set_s(vfp, vd, vfp_sqrt_s(vfp, vfp_get_s(vfp, vm)));
        }
      } else {
        uint64_t sign = is_double ? 1ull << 63 : 1ull << 31;
        uint64_t v = is_double ? vfp->regs.d[vm] : vfp->regs.s[vm];
        if (opc2 == 1) {
          v ^= sign;
        } else if (high) {
          v &= ~sign;
        }
        if (is_double) {
          vfp->regs.d[vd] = v;
        } else {
          vfp->regs.s[vd] = (uint32_t)v;
        }
      }
      vd = vfp_vector_next(vfp, vd, is_double);
      if (m_vector) {
        vm = vfp_vector_next(vfp, vm, is_double);
      }
    }
    return true;
  }

  switch (opc2) {
  case 0x4:
  case 0x5: {
    /* VCMP and VCMPE, against Vm or against zero. Only VCMPE raises
     * Invalid Operation for a quiet NaN; both do for a signalling one. */
    bool to_zero = opc2 == 0x5;
    double a, b;
    bool snan;
    if (is_double) {
      a = vfp_flush_in_d(vfp, vfp_get_d(vfp, vd));
      b = to_zero ? 0.0 : vfp_flush_in_d(vfp, vfp_get_d(vfp, vm));
      snan = vfp_is_snan_d(vfp->regs.d[vd]) ||
             (!to_zero && vfp_is_snan_d(vfp->regs.d[vm]));
    } else {
      a = vfp_flush_in_s(vfp, vfp_get_s(vfp, vd));
      b = to_zero ? 0.0 : vfp_flush_in_s(vfp, vfp_get_s(vfp, vm));
      snan = vfp_is_snan_s(vfp->regs.s[vd]) ||
             (!to_zero && vfp_is_snan_s(vfp->regs.s[vm]));
    }
    bool unordered = isnan(a) || isnan(b);
    vfp_compare(vfp, a, b, unordered);
    if (snan || (high && unordered)) {
      vfp->fpscr |= FPSCR_IOC;
    }
    return true;
  }

  case 0x7:
    /* VCVT between the precisions. sz gives the *source*, so both the
     * register numbering and the direction flip with it. */
    if (!high) {
      return false;
    }
    vfp_convert_precision(vfp, opcode, is_double);
    return true;

  case 0x8: {
    /* VCVT from an integer, which always arrives in a single-precision
     * register; sz gives the destination. The conversion to single
     * precision rounds, in the host's mode, which vfp_enter_rounding set. */
    uint32_t bits = vfp->regs.s[VFP_SREG(VFP_VM, VFP_M)];
    if (is_double) {
      vfp_set_d(vfp, VFP_DREG(VFP_VD, VFP_D),
                high ? (double)(int32_t)bits : (double)bits);
    } else {
      vfp_set_s(vfp, VFP_SREG(VFP_VD, VFP_D),
                high ? (float)(int32_t)bits : (float)bits);
    }
    return true;
  }

  case 0xC:
  case 0xD: {
    /* VCVT and VCVTR to an integer, which always lands in a
     * single-precision register; sz gives the source. */
    double v = vfp_source(vfp, vm, is_double);
    int sd = VFP_SREG(VFP_VD, VFP_D);
    vfp->regs.s[sd] = opc2 == 0xD ? (uint32_t)vfp_to_s32(vfp, v, high)
                                  : vfp_to_u32(vfp, v, high);
    return true;
  }

  case 0xA:
  case 0xB:
  case 0xE:
  case 0xF: {
    /* VCVT between floating point and fixed point, in place in Vd:
     *
     *   cond 1110 1D11 1 op 1 U Vd 101 sf sx 1 i 0 imm4
     *
     * op is the direction, U unsigned, and sx picks a 16- or 32-bit fixed
     * point value, held in the low bits of the register. The fraction bit
     * count is the size less imm4:i. */
    bool to_fixed = opc2 & 4;
    bool is_unsigned = opc2 & 1;
    unsigned size = high ? 32 : 16;
    int frac_bits =
        (int)size - (int)(((opcode & 0xF) << 1) | ((opcode >> 5) & 1));
    /* imm4:i above the size is UNPREDICTABLE. It scales by the negative
     * count, which is what the code below does anyway. */
    if (to_fixed) {
      double v = vfp_source(vfp, vd, is_double);
      uint32_t result = vfp_to_fixed(vfp, v, frac_bits, size, is_unsigned);
      if (is_double) {
        vfp->regs.d[vd] =
            is_unsigned ? (uint64_t)result : (uint64_t)(int64_t)(int32_t)result;
      } else {
        vfp->regs.s[vd] = result;
      }
    } else {
      uint32_t bits = is_double ? (uint32_t)vfp->regs.d[vd] : vfp->regs.s[vd];
      int64_t value;
      if (size == 16) {
        value = is_unsigned ? (int64_t)(bits & 0xFFFF) : (int64_t)(int16_t)bits;
      } else {
        value = is_unsigned ? (int64_t)bits : (int64_t)(int32_t)bits;
      }
      /* Exact in a double, so the only rounding is the final one. */
      double scaled = ldexp((double)value, -frac_bits);
      if (is_double) {
        vfp_set_d(vfp, vd, scaled);
      } else {
        vfp_set_s(vfp, vd, (float)scaled);
      }
    }
    return true;
  }

  case 0x2:
  case 0x3: {
    /* VCVTB and VCVTT, between single precision and the half-precision
     * value in the bottom or top half of a single register (T at bit 7),
     * as the armv7s A6 has. There is no double-precision form before
     * ARMv8. */
    int sd = VFP_SREG(VFP_VD, VFP_D);
    int sm = VFP_SREG(VFP_VM, VFP_M);
    if (is_double) {
      return false;
    }
    if (opc2 == 0x2) {
      uint32_t h = high ? vfp->regs.s[sm] >> 16 : vfp->regs.s[sm] & 0xFFFF;
      vfp->regs.s[sd] = vfp_half_to_single(vfp, h);
    } else {
      float v = vfp_flush_in_s(vfp, vfp_get_s(vfp, sm));
      uint32_t h =
          vfp_single_to_half(vfp, vfp_bits_s(v), FPSCR_RMODE(vfp->fpscr));
      vfp->regs.s[sd] = high ? (vfp->regs.s[sd] & 0xFFFF) | (h << 16)
                             : (vfp->regs.s[sd] & 0xFFFF0000u) | h;
    }
    return true;
  }

  default:
    return false;
  }
}

/* The three-operand group, all of which have vector forms:
 *
 *   cond 1110 o1 D o2 Vn Vd 101 sz N o3 M 0 Vm
 */
static bool vfp_data_processing(struct ARMCore *cpu, uint32_t opcode) {
  struct touchHLE_Vfp *vfp = vfp_of(cpu);
  bool is_double = VFP_IS_DOUBLE;
  unsigned o1 = (opcode >> 23) & 1;
  unsigned o2 = (opcode >> 20) & 3;
  unsigned o3 = (opcode >> 6) & 1;

  if (o1 && o2 == 3) {
    return vfp_other(cpu, opcode, is_double);
  }

  unsigned op = (o1 << 3) | (o2 << 1) | o3;
  int vd = is_double ? VFP_DREG(VFP_VD, VFP_D) : VFP_SREG(VFP_VD, VFP_D);
  int vn = is_double ? VFP_DREG(VFP_VN, VFP_N) : VFP_SREG(VFP_VN, VFP_N);
  int vm = is_double ? VFP_DREG(VFP_VM, VFP_M) : VFP_SREG(VFP_VM, VFP_M);

  if (op >= 10 && op <= 13) {
    /* VFPv4's fused multiply-accumulates, which the armv7s A6 has:
     * VFNMS (10), VFNMA (11), VFMA (12) and VFMS (13). They have no short
     * vector form; with LEN or STRIDE set they are UNPREDICTABLE, and
     * undefined here. */
    if (FPSCR_LEN(vfp->fpscr) || FPSCR_STRIDE(vfp->fpscr)) {
      return false;
    }
    bool negate_d = op <= 11;
    bool negate_n = op == 11 || op == 13;
    if (is_double) {
      double d = vfp_get_d(vfp, vd), n = vfp_get_d(vfp, vn);
      vfp_set_d(vfp, vd,
                vfp_fused_d(vfp, negate_d ? vfp_neg_d(d) : d,
                            negate_n ? vfp_neg_d(n) : n, vfp_get_d(vfp, vm)));
    } else {
      float d = vfp_get_s(vfp, vd), n = vfp_get_s(vfp, vn);
      vfp_set_s(vfp, vd,
                vfp_fused_s(vfp, negate_d ? vfp_neg_s(d) : d,
                            negate_n ? vfp_neg_s(n) : n, vfp_get_s(vfp, vm)));
    }
    return true;
  }
  if (op > 8) {
    return false;
  }

  unsigned len = vfp_vector_length(vfp, vd, is_double);
  bool m_vector = !vfp_in_scalar_bank(vm, is_double);

  for (unsigned i = 0; i < len; i++) {
    if (is_double) {
      vfp_set_d(vfp, vd,
                vfp_arith_d(vfp, op, vfp_get_d(vfp, vd), vfp_get_d(vfp, vn),
                            vfp_get_d(vfp, vm)));
    } else {
      vfp_set_s(vfp, vd,
                vfp_arith_s(vfp, op, vfp_get_s(vfp, vd), vfp_get_s(vfp, vn),
                            vfp_get_s(vfp, vm)));
    }
    vd = vfp_vector_next(vfp, vd, is_double);
    vn = vfp_vector_next(vfp, vn, is_double);
    if (m_vector) {
      vm = vfp_vector_next(vfp, vm, is_double);
    }
  }
  return true;
}

bool touchHLE_vfp_raw(struct ARMCore *cpu, uint32_t opcode) {
  switch ((opcode >> 24) & 0xF) {
  case 0xC:
  case 0xD:
    /* The 110 space: coprocessor load/store, plus the two-register
     * transfers that (P, U, W) == (0, 0, 0) carves out of it. */
    if (((opcode >> 21) & 0x7F) == 0x62) {
      return vfp_transfer_two(cpu, opcode);
    }
    return vfp_load_store(cpu, opcode);
  case 0xE:
    if (!(opcode & 0x10)) {
      int saved = vfp_enter_rounding(vfp_of(cpu));
      bool handled = vfp_data_processing(cpu, opcode);
      vfp_leave_rounding(saved);
      return handled;
    }
    /* On cp11, moves between core registers and D register lanes, and
     * VDUP; VFP has the 32-bit lanes, Advanced SIMD the rest, and neon.c
     * does them all. */
    if (VFP_IS_DOUBLE) {
      return touchHLE_neon_transfer(cpu, opcode);
    }
    switch ((opcode >> 21) & 7) {
    case 0:
      return vfp_transfer_one(cpu, opcode);
    case 7:
      return vfp_system_reg(cpu, opcode);
    default:
      return false;
    }
  default:
    return false;
  }
}
