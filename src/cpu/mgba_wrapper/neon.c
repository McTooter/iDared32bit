/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Advanced SIMD (NEON) for the mGBA ARM core, as on the Cortex-A8: the
 * integer, polynomial and single-precision instructions, their element and
 * structure loads and stores, and the lane transfers to and from core
 * registers; and, from the armv7s A6's NEONv2, the fused multiply-adds and
 * the conversions to and from half precision.
 *
 * The data-processing and load/store instructions live in ARM's
 * unconditional space, so the core hands them here through its
 * advancedSimd hook, always in their ARM encoding: Thumb-2's differ only in
 * where the U bit and the top byte sit, and isa-thumb2.c converts them. The
 * lane transfers and VDUP from a core register are in coprocessor 11's
 * space, and vfp.c passes them on.
 *
 * The registers are VFP's: D0 to D31, with Qn being D2n and D2n+1. Every
 * instruction reads all of its source registers before writing any result,
 * which is what the architecture specifies where sources and destination
 * overlap (its pseudocode's `Din`).
 *
 * Floating point here ignores FPSCR's controls: it always flushes denormals
 * to zero, returns the default NaN and rounds to nearest (the
 * architecture's "standard FPSCR value"). It does set FPSCR's cumulative
 * flags, as do the saturating integer instructions, whose flag is QC. */

#include <math.h>
#include <string.h>

#include "fp.h"

/* ---- Fields, elements and registers ---- */

#define NEON_U ((opcode >> 24) & 1)
#define NEON_Q ((opcode >> 6) & 1)
#define NEON_SIZE ((opcode >> 20) & 3)
/* Register numbers, with the D, N and M extension bits as the top bit. */
#define NEON_VD ((int)(((opcode >> 18) & 0x10) | ((opcode >> 12) & 0xF)))
#define NEON_VN ((int)(((opcode >> 3) & 0x10) | ((opcode >> 16) & 0xF)))
#define NEON_VM ((int)(((opcode >> 1) & 0x10) | (opcode & 0xF)))

#define FPSCR_QC (1u << 27)
#define FPSCR_DZC (1u << 1)
/* The flags Advanced SIMD sets: QC, and VFP's cumulative exception bits. */
#define NEON_STICKY (FPSCR_QC | 0x9Fu)

static struct touchHLE_Vfp *neon_vfp(struct ARMCore *cpu) {
  return &wrapper_of(cpu)->vfp;
}

static uint64_t ones(unsigned bits) {
  return bits >= 64 ? ~0ull : (1ull << bits) - 1;
}

static int64_t sext(uint64_t v, unsigned bits) {
  return bits >= 64 ? (int64_t)v : (int64_t)(v << (64 - bits)) >> (64 - bits);
}

/* An element's value as a signed or unsigned integer. Anything but a 64-bit
 * unsigned element fits int64_t, and those are handled separately. */
static int64_t ext(uint64_t v, unsigned bits, bool is_unsigned) {
  return is_unsigned ? (int64_t)(v & ones(bits)) : sext(v, bits);
}

/* Shifts that give the mathematical answer for any count, where C's are
 * undefined from 64 up. */
static uint64_t lsl(uint64_t v, unsigned n) { return n >= 64 ? 0 : v << n; }
static uint64_t lsr(uint64_t v, unsigned n) { return n >= 64 ? 0 : v >> n; }
static int64_t asr(int64_t v, unsigned n) { return v >> (n >= 64 ? 63 : n); }

/* A vector is an array of D registers, element 0 in the low bits of the
 * first. */
static uint64_t elem(const uint64_t *v, unsigned e, unsigned esize) {
  if (esize == 64) {
    return v[e];
  }
  unsigned per = 64 / esize;
  return (v[e / per] >> ((e % per) * esize)) & ones(esize);
}

static void set_elem(uint64_t *v, unsigned e, unsigned esize, uint64_t x) {
  if (esize == 64) {
    v[e] = x;
    return;
  }
  unsigned per = 64 / esize;
  unsigned shift = (e % per) * esize;
  uint64_t mask = ones(esize) << shift;
  v[e / per] = (v[e / per] & ~mask) | ((x << shift) & mask);
}

static void read_regs(const struct touchHLE_Vfp *vfp, int first, unsigned count,
                      uint64_t *out) {
  for (unsigned i = 0; i < count; i++) {
    out[i] = vfp->regs.d[(first + i) & 31];
  }
}

static void write_regs(struct touchHLE_Vfp *vfp, int first, unsigned count,
                       const uint64_t *in) {
  for (unsigned i = 0; i < count; i++) {
    vfp->regs.d[(first + i) & 31] = in[i];
  }
}

/* ---- Saturation ---- */

static uint64_t sat_signed(struct touchHLE_Vfp *vfp, int64_t v,
                           unsigned esize) {
  int64_t max = (int64_t)ones(esize - 1);
  int64_t min = -max - 1;
  if (v > max) {
    vfp->fpscr |= FPSCR_QC;
    v = max;
  } else if (v < min) {
    vfp->fpscr |= FPSCR_QC;
    v = min;
  }
  return (uint64_t)v & ones(esize);
}

/* For a value that may be negative, as a signed source saturating to an
 * unsigned result is. */
static uint64_t sat_unsigned(struct touchHLE_Vfp *vfp, int64_t v,
                             unsigned esize) {
  if (v < 0) {
    vfp->fpscr |= FPSCR_QC;
    return 0;
  }
  if ((uint64_t)v > ones(esize)) {
    vfp->fpscr |= FPSCR_QC;
    return ones(esize);
  }
  return (uint64_t)v;
}

static uint64_t sat_unsigned_from_unsigned(struct touchHLE_Vfp *vfp, uint64_t v,
                                           unsigned esize) {
  if (v > ones(esize)) {
    vfp->fpscr |= FPSCR_QC;
    return ones(esize);
  }
  return v;
}

static uint64_t qadd(struct touchHLE_Vfp *vfp, uint64_t a, uint64_t b,
                     unsigned esize, bool is_unsigned) {
  if (esize < 64) {
    int64_t sum = ext(a, esize, is_unsigned) + ext(b, esize, is_unsigned);
    return is_unsigned ? sat_unsigned(vfp, sum, esize)
                       : sat_signed(vfp, sum, esize);
  }
  uint64_t r = a + b;
  if (is_unsigned) {
    if (r < a) {
      vfp->fpscr |= FPSCR_QC;
      return ~0ull;
    }
  } else if (!((a ^ b) >> 63) && ((a ^ r) >> 63)) {
    vfp->fpscr |= FPSCR_QC;
    return (a >> 63) ? 1ull << 63 : ~0ull >> 1;
  }
  return r;
}

static uint64_t qsub(struct touchHLE_Vfp *vfp, uint64_t a, uint64_t b,
                     unsigned esize, bool is_unsigned) {
  if (esize < 64) {
    int64_t diff = ext(a, esize, is_unsigned) - ext(b, esize, is_unsigned);
    return is_unsigned ? sat_unsigned(vfp, diff, esize)
                       : sat_signed(vfp, diff, esize);
  }
  uint64_t r = a - b;
  if (is_unsigned) {
    if (a < b) {
      vfp->fpscr |= FPSCR_QC;
      return 0;
    }
  } else if (((a ^ b) & (a ^ r)) >> 63) {
    vfp->fpscr |= FPSCR_QC;
    return (a >> 63) ? 1ull << 63 : ~0ull >> 1;
  }
  return r;
}

/* ---- Shifts ---- */

/* A right shift by n >= 1, truncating or rounding. Rounding adds half the
 * divisor before shifting, which for any n comes to adding the last bit
 * shifted out. The result always fits the element. */
static uint64_t shift_right(uint64_t x, unsigned n, unsigned esize,
                            bool is_unsigned, bool round) {
  if (is_unsigned) {
    x &= ones(esize);
    uint64_t r = lsr(x, n);
    if (round) {
      r += lsr(x, n - 1) & 1;
    }
    return r & ones(esize);
  }
  int64_t v = sext(x, esize);
  int64_t r = asr(v, n);
  if (round) {
    r += asr(v, n - 1) & 1;
  }
  return (uint64_t)r & ones(esize);
}

/* A left shift by n >= 0 that saturates. The source is unsigned or signed,
 * and so is the result; a signed source with an unsigned result is VQSHLU's
 * and VQRSHRUN's combination. */
static uint64_t qshift_left(struct touchHLE_Vfp *vfp, uint64_t x, unsigned n,
                            unsigned esize, bool src_unsigned,
                            bool dst_unsigned) {
  if (src_unsigned) {
    x &= ones(esize);
    if (x == 0) {
      return 0;
    }
    if (n >= esize || x > (ones(esize) >> n)) {
      vfp->fpscr |= FPSCR_QC;
      return ones(esize);
    }
    return x << n;
  }
  int64_t v = sext(x, esize);
  if (v == 0) {
    return 0;
  }
  if (dst_unsigned) {
    if (v < 0) {
      vfp->fpscr |= FPSCR_QC;
      return 0;
    }
    if (n >= esize || (uint64_t)v > (ones(esize) >> n)) {
      vfp->fpscr |= FPSCR_QC;
      return ones(esize);
    }
    return (uint64_t)v << n;
  }
  if (n >= esize) {
    vfp->fpscr |= FPSCR_QC;
    return v < 0 ? (1ull << (esize - 1)) : ones(esize - 1);
  }
  int64_t hi = (int64_t)(ones(esize - 1) >> n);
  if (v > hi) {
    vfp->fpscr |= FPSCR_QC;
    return ones(esize - 1);
  }
  if (v < -hi - 1) {
    vfp->fpscr |= FPSCR_QC;
    return 1ull << (esize - 1);
  }
  return ((uint64_t)v << n) & ones(esize);
}

/* VSHL, VQSHL, VRSHL and VQRSHL by register: the shift is the signed bottom
 * byte of the other operand's element, and a negative shift is a right
 * shift. Only left shifts can saturate. */
static uint64_t shift_by_register(struct touchHLE_Vfp *vfp, uint64_t x,
                                  uint64_t shift_elem, unsigned esize,
                                  bool is_unsigned, bool round, bool sat) {
  int shift = (int)sext(shift_elem & 0xFF, 8);
  if (shift >= 0) {
    if (sat) {
      return qshift_left(vfp, x, (unsigned)shift, esize, is_unsigned,
                         is_unsigned);
    }
    return lsl(x, (unsigned)shift) & ones(esize);
  }
  return shift_right(x, (unsigned)-shift, esize, is_unsigned, round);
}

/* ---- Integer arithmetic ---- */

static uint64_t poly_mul(uint64_t a, uint64_t b, unsigned bits) {
  uint64_t r = 0;
  for (unsigned i = 0; i < bits; i++) {
    if ((b >> i) & 1) {
      r ^= a << i;
    }
  }
  return r;
}

static uint64_t abs_diff(uint64_t a, uint64_t b, unsigned esize,
                         bool is_unsigned) {
  int64_t d = ext(a, esize, is_unsigned) - ext(b, esize, is_unsigned);
  return (uint64_t)(d < 0 ? -d : d);
}

/* VQDMULH and VQRDMULH: the high half of twice the product, which only
 * overflows for the most negative number squared. */
static uint64_t qdmulh(struct touchHLE_Vfp *vfp, uint64_t a, uint64_t b,
                       unsigned esize, bool round) {
  int64_t x = sext(a, esize);
  int64_t y = sext(b, esize);
  int64_t min = -(int64_t)ones(esize - 1) - 1;
  if (x == min && y == min) {
    vfp->fpscr |= FPSCR_QC;
    return ones(esize - 1);
  }
  int64_t r = 2 * (x * y) + (round ? (int64_t)1 << (esize - 1) : 0);
  return (uint64_t)(r >> esize) & ones(esize);
}

/* VQDMULL's product, twice the product at double width, saturated. */
static uint64_t qdmull(struct touchHLE_Vfp *vfp, uint64_t a, uint64_t b,
                       unsigned esize) {
  int64_t x = sext(a, esize);
  int64_t y = sext(b, esize);
  int64_t min = -(int64_t)ones(esize - 1) - 1;
  if (x == min && y == min) {
    vfp->fpscr |= FPSCR_QC;
    return ones(2 * esize - 1);
  }
  return (uint64_t)(2 * (x * y)) & ones(2 * esize);
}

enum neon_int_op {
  OP_HADD,
  OP_QADD,
  OP_RHADD,
  OP_HSUB,
  OP_QSUB,
  OP_CGT,
  OP_CGE,
  OP_SHL,
  OP_QSHL,
  OP_RSHL,
  OP_QRSHL,
  OP_MAX,
  OP_MIN,
  OP_ABD,
  OP_ABA,
  OP_ADD,
  OP_SUB,
  OP_TST,
  OP_CEQ,
  OP_MLA,
  OP_MLS,
  OP_MUL,
  OP_PMUL,
  OP_QDMULH,
  OP_QRDMULH,
};

/* One element of the integer three-register group: a from Vn, b from Vm,
 * acc the destination's old value. */
static uint64_t int_op(struct touchHLE_Vfp *vfp, enum neon_int_op op,
                       uint64_t a, uint64_t b, uint64_t acc, unsigned esize,
                       bool u) {
  uint64_t mask = ones(esize);
  switch (op) {
  case OP_HADD:
    return (uint64_t)((ext(a, esize, u) + ext(b, esize, u)) >> 1) & mask;
  case OP_RHADD:
    return (uint64_t)((ext(a, esize, u) + ext(b, esize, u) + 1) >> 1) & mask;
  case OP_HSUB:
    return (uint64_t)((ext(a, esize, u) - ext(b, esize, u)) >> 1) & mask;
  case OP_QADD:
    return qadd(vfp, a, b, esize, u);
  case OP_QSUB:
    return qsub(vfp, a, b, esize, u);
  case OP_CGT:
    return ext(a, esize, u) > ext(b, esize, u) ? mask : 0;
  case OP_CGE:
    return ext(a, esize, u) >= ext(b, esize, u) ? mask : 0;
  /* The shifts take the value from Vm and the shift from Vn. */
  case OP_SHL:
    return shift_by_register(vfp, b, a, esize, u, false, false);
  case OP_QSHL:
    return shift_by_register(vfp, b, a, esize, u, false, true);
  case OP_RSHL:
    return shift_by_register(vfp, b, a, esize, u, true, false);
  case OP_QRSHL:
    return shift_by_register(vfp, b, a, esize, u, true, true);
  case OP_MAX:
    return ext(a, esize, u) >= ext(b, esize, u) ? a : b;
  case OP_MIN:
    return ext(a, esize, u) >= ext(b, esize, u) ? b : a;
  case OP_ABD:
    return abs_diff(a, b, esize, u) & mask;
  case OP_ABA:
    return (acc + abs_diff(a, b, esize, u)) & mask;
  case OP_ADD:
    return (a + b) & mask;
  case OP_SUB:
    return (a - b) & mask;
  case OP_TST:
    return (a & b) ? mask : 0;
  case OP_CEQ:
    return a == b ? mask : 0;
  case OP_MLA:
    return (acc + a * b) & mask;
  case OP_MLS:
    return (acc - a * b) & mask;
  case OP_MUL:
    return (a * b) & mask;
  case OP_PMUL:
    return poly_mul(a, b, esize) & mask;
  case OP_QDMULH:
    return qdmulh(vfp, a, b, esize, false);
  case OP_QRDMULH:
    return qdmulh(vfp, a, b, esize, true);
  }
  return 0;
}

/* ---- Floating point ---- */

/* Advanced SIMD's floating point runs under the standard FPSCR value --
 * flush-to-zero, default NaN, round to nearest -- whatever FPSCR says, and
 * reports its exceptions in FPSCR's cumulative flags. The primitives in
 * fp.h read their controls from vfp->fpscr, so this swaps it for the
 * duration. The host is already rounding to nearest: vfp.c only changes
 * that inside a VFP instruction. */
static uint32_t fp_enter(struct touchHLE_Vfp *vfp) {
  uint32_t saved = vfp->fpscr;
  /* The standard value keeps FPSCR's AHP, which only the half-precision
   * conversions look at. */
  vfp->fpscr = FPSCR_FZ | FPSCR_DN | (saved & (NEON_STICKY | FPSCR_AHP));
  return saved;
}

static void fp_leave(struct touchHLE_Vfp *vfp, uint32_t saved) {
  vfp->fpscr = (saved & ~NEON_STICKY) | (vfp->fpscr & NEON_STICKY);
}

static float f32(uint64_t bits) { return vfp_from_bits_s((uint32_t)bits); }
static uint64_t bits32(float v) { return vfp_bits_s(v); }

enum neon_fp_cmp { CMP_EQ, CMP_GE, CMP_GT };

/* FPCompareEQ/GE/GT. A NaN makes every comparison false; it is an Invalid
 * Operation for the ordered comparisons, and for equality only if
 * signalling. */
static bool fp_compare(struct touchHLE_Vfp *vfp, float a, float b,
                       enum neon_fp_cmp cmp) {
  a = vfp_flush_in_s(vfp, a);
  b = vfp_flush_in_s(vfp, b);
  if (isnan(a) || isnan(b)) {
    if (cmp != CMP_EQ || vfp_is_snan_s(vfp_bits_s(a)) ||
        vfp_is_snan_s(vfp_bits_s(b))) {
      vfp->fpscr |= FPSCR_IOC;
    }
    return false;
  }
  switch (cmp) {
  case CMP_EQ:
    return a == b;
  case CMP_GE:
    return a >= b;
  default:
    return a > b;
  }
}

/* FPMax and FPMin: NaNs as for arithmetic, and of two zeroes the maximum is
 * +0 unless both are -0 (and conversely for the minimum). */
static float fp_max_min(struct touchHLE_Vfp *vfp, float a, float b,
                        bool is_max) {
  float nan;
  a = vfp_flush_in_s(vfp, a);
  b = vfp_flush_in_s(vfp, b);
  if (vfp_nans_s(vfp, a, b, 2, &nan)) {
    return nan;
  }
  if (a == 0 && b == 0) {
    uint32_t sa = vfp_bits_s(a), sb = vfp_bits_s(b);
    return vfp_from_bits_s(is_max ? (sa & sb) : (sa | sb));
  }
  if (is_max) {
    return a > b ? a : b;
  }
  return a < b ? a : b;
}

/* FPRecipStep and FPRSqrtStep, the Newton-Raphson steps VRECPS and VRSQRTS:
 * 2 - a*b and (3 - a*b) / 2, with infinity times zero counting as zero. */
static float fp_step(struct touchHLE_Vfp *vfp, float a, float b, bool rsqrt) {
  float nan;
  a = vfp_flush_in_s(vfp, a);
  b = vfp_flush_in_s(vfp, b);
  if (vfp_nans_s(vfp, a, b, 2, &nan)) {
    return nan;
  }
  float product;
  if ((isinf(a) && b == 0) || (a == 0 && isinf(b))) {
    product = 0.0f;
  } else {
    product = vfp_mul_s(vfp, a, b);
  }
  if (!rsqrt) {
    return vfp_sub_s(vfp, 2.0f, product);
  }
  /* Halving is exact short of the subnormal range, and 3 - product never
   * gets that small, so this is one rounding, as FPHalvedSub is. */
  return vfp_sub_s(vfp, 3.0f, product) * 0.5f;
}

/* The estimate tables, as the architecture defines them in terms of double
 * arithmetic: a in [0.5, 1) for the reciprocal and [0.25, 1) for the
 * reciprocal square root, results in [1, 2) as a multiple of 1/256. */
static double recip_estimate(double a) {
  int q = (int)(a * 512.0);
  double r = 1.0 / (((double)q + 0.5) / 512.0);
  int s = (int)(256.0 * r + 0.5);
  return (double)s / 256.0;
}

static double recip_sqrt_estimate(double a) {
  double r;
  if (a < 0.5) {
    int q0 = (int)(a * 512.0);
    r = 1.0 / sqrt(((double)q0 + 0.5) / 512.0);
  } else {
    int q1 = (int)(a * 256.0);
    r = 1.0 / sqrt(((double)q1 + 0.5) / 256.0);
  }
  int s = (int)(256.0 * r + 0.5);
  return (double)s / 256.0;
}

static uint64_t double_bits(double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

static double double_from_bits(uint64_t bits) {
  double v;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

static uint32_t unsigned_recip_estimate(uint32_t x) {
  if (!(x >> 31)) {
    return 0xFFFFFFFFu;
  }
  double a = double_from_bits(0x3FE0000000000000ull |
                              ((uint64_t)(x & 0x7FFFFFFFu) << 21));
  return 0x80000000u |
         (uint32_t)((double_bits(recip_estimate(a)) >> 21) & 0x7FFFFFFFu);
}

static uint32_t unsigned_rsqrt_estimate(uint32_t x) {
  if (!(x >> 30)) {
    return 0xFFFFFFFFu;
  }
  double a;
  if (x >> 31) {
    a = double_from_bits(0x3FE0000000000000ull |
                         ((uint64_t)(x & 0x7FFFFFFFu) << 21));
  } else {
    a = double_from_bits(0x3FD0000000000000ull |
                         ((uint64_t)(x & 0x3FFFFFFFu) << 22));
  }
  return 0x80000000u |
         (uint32_t)((double_bits(recip_sqrt_estimate(a)) >> 21) & 0x7FFFFFFFu);
}

static uint32_t fp_recip_estimate(struct touchHLE_Vfp *vfp, uint32_t x) {
  float v = vfp_flush_in_s(vfp, vfp_from_bits_s(x));
  uint32_t bits = vfp_bits_s(v);
  uint32_t sign = bits & 0x80000000u;
  float nan;
  if (vfp_nans_s(vfp, v, v, 1, &nan)) {
    return vfp_bits_s(nan);
  }
  if (isinf(v)) {
    return sign;
  }
  if (v == 0) {
    vfp->fpscr |= FPSCR_DZC;
    return sign | 0x7F800000u;
  }
  unsigned exp = (bits >> 23) & 0xFF;
  if (exp >= 253) {
    /* |x| >= 2^126: the reciprocal would be subnormal, and is flushed. */
    vfp->fpscr |= FPSCR_UFC;
    return sign;
  }
  double a = double_from_bits(0x3FE0000000000000ull |
                              ((uint64_t)(bits & 0x7FFFFFu) << 29));
  uint64_t est = double_bits(recip_estimate(a));
  return sign | ((253 - exp) << 23) | (uint32_t)((est >> 29) & 0x7FFFFFu);
}

static uint32_t fp_rsqrt_estimate(struct touchHLE_Vfp *vfp, uint32_t x) {
  float v = vfp_flush_in_s(vfp, vfp_from_bits_s(x));
  uint32_t bits = vfp_bits_s(v);
  float nan;
  if (vfp_nans_s(vfp, v, v, 1, &nan)) {
    return vfp_bits_s(nan);
  }
  if (v == 0) {
    vfp->fpscr |= FPSCR_DZC;
    return (bits & 0x80000000u) | 0x7F800000u;
  }
  if (bits >> 31) {
    vfp->fpscr |= FPSCR_IOC;
    return 0x7FC00000u;
  }
  if (isinf(v)) {
    return 0;
  }
  unsigned exp = (bits >> 23) & 0xFF;
  double a;
  if (exp & 1) {
    a = double_from_bits(0x3FD0000000000000ull |
                         ((uint64_t)(bits & 0x7FFFFFu) << 29));
  } else {
    a = double_from_bits(0x3FE0000000000000ull |
                         ((uint64_t)(bits & 0x7FFFFFu) << 29));
  }
  uint64_t est = double_bits(recip_sqrt_estimate(a));
  return (((380 - exp) / 2) << 23) | (uint32_t)((est >> 29) & 0x7FFFFFu);
}

/* VCVT between single precision and 32-bit fixed point (integers being
 * fixed point with no fraction): toward zero into fixed point, to nearest
 * out of it. */
static uint32_t fp_to_fixed(struct touchHLE_Vfp *vfp, uint32_t x, int frac_bits,
                            bool is_unsigned) {
  float v = vfp_flush_in_s(vfp, vfp_from_bits_s(x));
  return vfp_to_fixed(vfp, (double)v, frac_bits, 32, is_unsigned);
}

static uint32_t fixed_to_fp(uint32_t x, int frac_bits, bool is_unsigned) {
  double v = is_unsigned ? (double)x : (double)(int32_t)x;
  /* Exact in double, so the conversion to float is the only rounding. */
  return vfp_bits_s((float)ldexp(v, -frac_bits));
}

enum neon_fp_op {
  FOP_ADD,
  FOP_SUB,
  FOP_ABD,
  FOP_MUL,
  FOP_MLA,
  FOP_MLS,
  FOP_CEQ,
  FOP_CGE,
  FOP_CGT,
  FOP_ACGE,
  FOP_ACGT,
  FOP_MAX,
  FOP_MIN,
  FOP_RECPS,
  FOP_RSQRTS,
  FOP_FMA,
  FOP_FMS,
};

/* One element of the floating-point three-register group, as bits. */
static uint64_t fp_op(struct touchHLE_Vfp *vfp, enum neon_fp_op op, uint64_t a,
                      uint64_t b, uint64_t acc) {
  float x = f32(a), y = f32(b);
  switch (op) {
  case FOP_ADD:
    return bits32(vfp_add_s(vfp, x, y));
  case FOP_SUB:
    return bits32(vfp_sub_s(vfp, x, y));
  case FOP_ABD:
    return bits32(vfp_sub_s(vfp, x, y)) & 0x7FFFFFFFu;
  case FOP_MUL:
    return bits32(vfp_mul_s(vfp, x, y));
  case FOP_MLA:
    return bits32(vfp_arith_s(vfp, 0, f32(acc), x, y));
  case FOP_MLS:
    return bits32(vfp_arith_s(vfp, 1, f32(acc), x, y));
  case FOP_CEQ:
    return fp_compare(vfp, x, y, CMP_EQ) ? 0xFFFFFFFFu : 0;
  case FOP_CGE:
    return fp_compare(vfp, x, y, CMP_GE) ? 0xFFFFFFFFu : 0;
  case FOP_CGT:
    return fp_compare(vfp, x, y, CMP_GT) ? 0xFFFFFFFFu : 0;
  case FOP_ACGE:
    return fp_compare(vfp, f32(a & 0x7FFFFFFFu), f32(b & 0x7FFFFFFFu), CMP_GE)
               ? 0xFFFFFFFFu
               : 0;
  case FOP_ACGT:
    return fp_compare(vfp, f32(a & 0x7FFFFFFFu), f32(b & 0x7FFFFFFFu), CMP_GT)
               ? 0xFFFFFFFFu
               : 0;
  case FOP_MAX:
    return bits32(fp_max_min(vfp, x, y, true));
  case FOP_MIN:
    return bits32(fp_max_min(vfp, x, y, false));
  case FOP_RECPS:
    return bits32(fp_step(vfp, x, y, false));
  case FOP_RSQRTS:
    return bits32(fp_step(vfp, x, y, true));
  case FOP_FMA:
    return bits32(vfp_fused_s(vfp, f32(acc), x, y));
  case FOP_FMS:
    return bits32(vfp_fused_s(vfp, f32(acc), vfp_neg_s(x), y));
  }
  return 0;
}

/* ---- Three registers of the same length ----
 *
 *   1111 001U 0 D size Vn Vd A(4) N Q M B Vm
 */

static bool neon_three_same(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  unsigned a = (opcode >> 8) & 0xF;
  bool b = (opcode >> 4) & 1;
  bool u = NEON_U;
  unsigned size = NEON_SIZE;
  bool q = NEON_Q;
  int d = NEON_VD, n = NEON_VN, m = NEON_VM;
  unsigned regs = q ? 2 : 1;
  unsigned esize = 8u << size;

  if (q && ((d | n | m) & 1)) {
    return false;
  }

  uint64_t vn[2], vm[2], vd[2];
  read_regs(vfp, n, regs, vn);
  read_regs(vfp, m, regs, vm);
  read_regs(vfp, d, regs, vd);

  /* The bitwise group, on whole registers. */
  if (a == 1 && b) {
    for (unsigned r = 0; r < regs; r++) {
      uint64_t x = vn[r], y = vm[r], z = vd[r];
      switch ((u << 2) | size) {
      case 0: /* VAND */
        vd[r] = x & y;
        break;
      case 1: /* VBIC */
        vd[r] = x & ~y;
        break;
      case 2: /* VORR, and VMOV when Vn == Vm */
        vd[r] = x | y;
        break;
      case 3: /* VORN */
        vd[r] = x | ~y;
        break;
      case 4: /* VEOR */
        vd[r] = x ^ y;
        break;
      case 5: /* VBSL */
        vd[r] = (z & x) | (~z & y);
        break;
      case 6: /* VBIT */
        vd[r] = (x & y) | (z & ~y);
        break;
      default: /* VBIF */
        vd[r] = (z & y) | (x & ~y);
        break;
      }
    }
    write_regs(vfp, d, regs, vd);
    return true;
  }

  /* Floating point: size<0> would be half precision, which is ARMv8's.
   * VFMA and VFMS, 1100 with B set and U clear, are the armv7s A6's
   * (VFPv4). */
  if (a >= 0xD || (a == 0xC && b && !u)) {
    enum neon_fp_op op;
    bool pairwise = false;
    bool hi = size & 2;
    if (size & 1) {
      return false;
    }
    switch ((a << 2) | (b << 1) | u) {
    case (0xD << 2) | 0:
      op = hi ? FOP_SUB : FOP_ADD;
      break;
    case (0xD << 2) | 1:
      op = hi ? FOP_ABD : FOP_ADD;
      pairwise = !hi;
      break;
    case (0xD << 2) | 2:
      op = hi ? FOP_MLS : FOP_MLA;
      break;
    case (0xD << 2) | 3:
      if (hi) {
        return false;
      }
      op = FOP_MUL;
      break;
    case (0xE << 2) | 0:
      if (hi) {
        return false;
      }
      op = FOP_CEQ;
      break;
    case (0xE << 2) | 1:
      op = hi ? FOP_CGT : FOP_CGE;
      break;
    case (0xE << 2) | 3:
      op = hi ? FOP_ACGT : FOP_ACGE;
      break;
    case (0xF << 2) | 0:
      op = hi ? FOP_MIN : FOP_MAX;
      break;
    case (0xF << 2) | 1:
      op = hi ? FOP_MIN : FOP_MAX;
      pairwise = true;
      break;
    case (0xF << 2) | 2:
      op = hi ? FOP_RSQRTS : FOP_RECPS;
      break;
    case (0xC << 2) | 2:
      op = hi ? FOP_FMS : FOP_FMA;
      break;
    default:
      return false;
    }
    if (pairwise && q) {
      return false;
    }
    uint32_t saved = fp_enter(vfp);
    uint64_t out[2] = {vd[0], vd[1]};
    if (pairwise) {
      /* Pairs from Vn give the low half of the result, from Vm the high. */
      out[0] = fp_op(vfp, op, vn[0] & 0xFFFFFFFFu, vn[0] >> 32, 0) |
               (fp_op(vfp, op, vm[0] & 0xFFFFFFFFu, vm[0] >> 32, 0) << 32);
    } else {
      for (unsigned e = 0; e < regs * 2; e++) {
        set_elem(
            out, e, 32,
            fp_op(vfp, op, elem(vn, e, 32), elem(vm, e, 32), elem(vd, e, 32)));
      }
    }
    fp_leave(vfp, saved);
    write_regs(vfp, d, regs, out);
    return true;
  }

  enum neon_int_op op;
  bool pairwise = false;
  /* Most of the group has no 64-bit form. */
  bool allows_64 = false;
  switch ((a << 1) | b) {
  case 0x0:
    op = OP_HADD;
    break;
  case 0x1:
    op = OP_QADD;
    allows_64 = true;
    break;
  case 0x2:
    op = OP_RHADD;
    break;
  case 0x4:
    op = OP_HSUB;
    break;
  case 0x5:
    op = OP_QSUB;
    allows_64 = true;
    break;
  case 0x6:
    op = OP_CGT;
    break;
  case 0x7:
    op = OP_CGE;
    break;
  case 0x8:
    op = OP_SHL;
    allows_64 = true;
    break;
  case 0x9:
    op = OP_QSHL;
    allows_64 = true;
    break;
  case 0xA:
    op = OP_RSHL;
    allows_64 = true;
    break;
  case 0xB:
    op = OP_QRSHL;
    allows_64 = true;
    break;
  case 0xC:
    op = OP_MAX;
    break;
  case 0xD:
    op = OP_MIN;
    break;
  case 0xE:
    op = OP_ABD;
    break;
  case 0xF:
    op = OP_ABA;
    break;
  case 0x10:
    op = u ? OP_SUB : OP_ADD;
    allows_64 = true;
    break;
  case 0x11:
    op = u ? OP_CEQ : OP_TST;
    break;
  case 0x12:
    op = u ? OP_MLS : OP_MLA;
    break;
  case 0x13:
    if (u && size != 0) {
      return false;
    }
    op = u ? OP_PMUL : OP_MUL;
    break;
  case 0x14:
    op = OP_MAX;
    pairwise = true;
    break;
  case 0x15:
    op = OP_MIN;
    pairwise = true;
    break;
  case 0x16:
    if (size == 0) {
      return false;
    }
    op = u ? OP_QRDMULH : OP_QDMULH;
    break;
  case 0x17:
    if (u) {
      return false;
    }
    op = OP_ADD;
    pairwise = true;
    break;
  default:
    return false;
  }
  if (size == 3 && !allows_64) {
    return false;
  }
  if (pairwise && q) {
    return false;
  }

  uint64_t out[2] = {vd[0], vd[1]};
  unsigned elements = regs * 64 / esize;
  if (pairwise) {
    unsigned half = elements / 2;
    for (unsigned e = 0; e < half; e++) {
      set_elem(out, e, esize,
               int_op(vfp, op, elem(vn, 2 * e, esize),
                      elem(vn, 2 * e + 1, esize), 0, esize, u));
      set_elem(out, half + e, esize,
               int_op(vfp, op, elem(vm, 2 * e, esize),
                      elem(vm, 2 * e + 1, esize), 0, esize, u));
    }
  } else {
    for (unsigned e = 0; e < elements; e++) {
      set_elem(out, e, esize,
               int_op(vfp, op, elem(vn, e, esize), elem(vm, e, esize),
                      elem(vd, e, esize), esize, u));
    }
  }
  write_regs(vfp, d, regs, out);
  return true;
}

/* ---- Three registers of different lengths ----
 *
 *   1111 001U 1 D size Vn Vd A(4) N 0 M 0 Vm
 *
 * Long forms widen both operands, wide forms only the second, and narrow
 * forms keep the high half of a double-width result. */

static bool neon_three_different(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  unsigned a = (opcode >> 8) & 0xF;
  bool u = NEON_U;
  unsigned size = NEON_SIZE;
  int d = NEON_VD, n = NEON_VN, m = NEON_VM;
  unsigned esize = 8u << size;
  unsigned wide = 2 * esize;
  unsigned elements = 64 / esize;
  uint64_t vn[2], vm[2], vd[2], out[2] = {0, 0};

  if (a == 0xF) {
    return false;
  }

  /* The narrowing high-half forms: Q operands, D result. */
  if (a == 4 || a == 6) {
    if ((n | m) & 1) {
      return false;
    }
    read_regs(vfp, n, 2, vn);
    read_regs(vfp, m, 2, vm);
    out[0] = 0;
    for (unsigned e = 0; e < elements; e++) {
      uint64_t x = elem(vn, e, wide), y = elem(vm, e, wide);
      uint64_t r = a == 4 ? x + y : x - y;
      if (u) {
        r += 1ull << (esize - 1);
      }
      set_elem(out, e, esize, (r & ones(wide)) >> esize);
    }
    vfp->regs.d[d] = out[0];
    return true;
  }

  if (d & 1) {
    return false;
  }
  bool wide_n = a == 1 || a == 3;
  if (wide_n && (n & 1)) {
    return false;
  }
  if ((a == 9 || a == 0xB || a == 0xD) && (u || size == 0)) {
    return false;
  }
  if (a == 0xE && (u || size != 0)) {
    return false;
  }

  read_regs(vfp, n, wide_n ? 2 : 1, vn);
  read_regs(vfp, m, 1, vm);
  read_regs(vfp, d, 2, vd);
  for (unsigned e = 0; e < elements; e++) {
    int64_t x =
        wide_n ? (int64_t)elem(vn, e, wide) : ext(elem(vn, e, esize), esize, u);
    int64_t y = ext(elem(vm, e, esize), esize, u);
    uint64_t acc = elem(vd, e, wide);
    uint64_t r;
    switch (a) {
    case 0: /* VADDL */
    case 1: /* VADDW */
      r = (uint64_t)x + (uint64_t)y;
      break;
    case 2: /* VSUBL */
    case 3: /* VSUBW */
      r = (uint64_t)x - (uint64_t)y;
      break;
    case 5: /* VABAL */
      r = acc + (uint64_t)(x > y ? x - y : y - x);
      break;
    case 7: /* VABDL */
      r = (uint64_t)(x > y ? x - y : y - x);
      break;
    case 8: /* VMLAL */
      r = acc + (uint64_t)x * (uint64_t)y;
      break;
    case 0xA: /* VMLSL */
      r = acc - (uint64_t)x * (uint64_t)y;
      break;
    case 0xC: /* VMULL */
      r = (uint64_t)x * (uint64_t)y;
      break;
    case 9: /* VQDMLAL */
      r = qadd(vfp, acc,
               qdmull(vfp, elem(vn, e, esize), elem(vm, e, esize), esize), wide,
               false);
      break;
    case 0xB: /* VQDMLSL */
      r = qsub(vfp, acc,
               qdmull(vfp, elem(vn, e, esize), elem(vm, e, esize), esize), wide,
               false);
      break;
    case 0xD: /* VQDMULL */
      r = qdmull(vfp, elem(vn, e, esize), elem(vm, e, esize), esize);
      break;
    default: /* VMULL.P8 */
      r = poly_mul(elem(vn, e, esize), elem(vm, e, esize), esize);
      break;
    }
    set_elem(out, e, wide, r & ones(wide));
  }
  write_regs(vfp, d, 2, out);
  return true;
}

/* ---- Two registers and a scalar ----
 *
 *   1111 001Q 1 D size Vn Vd A(4) N 1 M 0 Vm
 *
 * Vm names the scalar: D0-D7 and a two-bit index for 16-bit elements,
 * D0-D15 and a one-bit index for 32-bit ones. */

static bool neon_scalar(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  unsigned a = (opcode >> 8) & 0xF;
  bool u = NEON_U;
  unsigned size = NEON_SIZE;
  int d = NEON_VD, n = NEON_VN;
  unsigned esize = 8u << size;
  bool is_float = (a & 0xB) == 1 || a == 9;
  bool is_long = a == 2 || a == 3 || a == 6 || a == 7 || a == 0xA || a == 0xB;

  if (size == 0 || a >= 0xE || (is_float && size != 2)) {
    return false;
  }
  if ((a == 3 || a == 7 || a == 0xB) && u) {
    return false;
  }

  int m;
  unsigned index;
  if (size == 1) {
    m = (int)(opcode & 7);
    index = (((opcode >> 5) & 1) << 1) | ((opcode >> 3) & 1);
  } else {
    m = (int)(opcode & 0xF);
    index = (opcode >> 5) & 1;
  }
  uint64_t scalar = elem(&vfp->regs.d[m], index, esize);

  if (is_long) {
    if (d & 1) {
      return false;
    }
    uint64_t vn[1], vd[2], out[2] = {0, 0};
    unsigned wide = 2 * esize;
    read_regs(vfp, n, 1, vn);
    read_regs(vfp, d, 2, vd);
    for (unsigned e = 0; e < 64 / esize; e++) {
      uint64_t x = elem(vn, e, esize);
      uint64_t acc = elem(vd, e, wide);
      uint64_t product =
          (uint64_t)ext(x, esize, u) * (uint64_t)ext(scalar, esize, u);
      uint64_t r;
      switch (a) {
      case 2: /* VMLAL */
        r = acc + product;
        break;
      case 6: /* VMLSL */
        r = acc - product;
        break;
      case 0xA: /* VMULL */
        r = product;
        break;
      case 3: /* VQDMLAL */
        r = qadd(vfp, acc, qdmull(vfp, x, scalar, esize), wide, false);
        break;
      case 7: /* VQDMLSL */
        r = qsub(vfp, acc, qdmull(vfp, x, scalar, esize), wide, false);
        break;
      default: /* VQDMULL */
        r = qdmull(vfp, x, scalar, esize);
        break;
      }
      set_elem(out, e, wide, r & ones(wide));
    }
    write_regs(vfp, d, 2, out);
    return true;
  }

  bool q = u;
  unsigned regs = q ? 2 : 1;
  if (q && ((d | n) & 1)) {
    return false;
  }
  uint64_t vn[2], vd[2], out[2] = {0, 0};
  read_regs(vfp, n, regs, vn);
  read_regs(vfp, d, regs, vd);
  unsigned elements = regs * 64 / esize;
  if (is_float) {
    enum neon_fp_op op = a == 1 ? FOP_MLA : a == 5 ? FOP_MLS : FOP_MUL;
    uint32_t saved = fp_enter(vfp);
    for (unsigned e = 0; e < elements; e++) {
      set_elem(out, e, 32,
               fp_op(vfp, op, elem(vn, e, 32), scalar, elem(vd, e, 32)));
    }
    fp_leave(vfp, saved);
  } else {
    enum neon_int_op op = a == 0     ? OP_MLA
                          : a == 4   ? OP_MLS
                          : a == 8   ? OP_MUL
                          : a == 0xC ? OP_QDMULH
                                     : OP_QRDMULH;
    for (unsigned e = 0; e < elements; e++) {
      set_elem(out, e, esize,
               int_op(vfp, op, elem(vn, e, esize), scalar, elem(vd, e, esize),
                      esize, false));
    }
  }
  write_regs(vfp, d, regs, out);
  return true;
}

/* ---- Two registers and a shift amount ----
 *
 *   1111 001U 1 D imm6 Vd A(4) L Q M 1 Vm
 *
 * The element size is given by the top set bit of L:imm6, and the shift by
 * the bits below it: a right shift counts down from the element size, a
 * left shift up from zero. */

static bool neon_shift_imm(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  unsigned a = (opcode >> 8) & 0xF;
  bool u = NEON_U;
  bool l = (opcode >> 7) & 1;
  bool q = NEON_Q;
  unsigned imm6 = (opcode >> 16) & 0x3F;
  int d = NEON_VD, m = NEON_VM;
  unsigned esize;

  if (l) {
    esize = 64;
  } else if (imm6 & 0x20) {
    esize = 32;
  } else if (imm6 & 0x10) {
    esize = 16;
  } else {
    esize = 8;
  }
  unsigned right = (esize == 64 ? 64 : 2 * esize) - imm6;
  unsigned left = imm6 - (esize == 64 ? 0 : esize);

  /* The narrowing and lengthening forms, and conversions to and from fixed
   * point, have no 64-bit form. */
  if (a >= 8) {
    if (l) {
      return false;
    }
    if (a >= 0xE) {
      /* VCVT between single precision and fixed point. */
      unsigned regs = q ? 2 : 1;
      uint64_t vm[2], out[2] = {0, 0};
      if (!(imm6 & 0x20) || (q && ((d | m) & 1))) {
        return false;
      }
      int frac_bits = (int)(64 - imm6);
      read_regs(vfp, m, regs, vm);
      uint32_t saved = fp_enter(vfp);
      for (unsigned e = 0; e < regs * 2; e++) {
        uint32_t x = (uint32_t)elem(vm, e, 32);
        set_elem(out, e, 32,
                 a == 0xF ? fp_to_fixed(vfp, x, frac_bits, u)
                          : fixed_to_fp(x, frac_bits, u));
      }
      fp_leave(vfp, saved);
      write_regs(vfp, d, regs, out);
      return true;
    }
    if (a == 0xA) {
      /* VSHLL, of which VMOVL is the zero shift. */
      uint64_t vm[1], out[2] = {0, 0};
      if (q || (d & 1)) {
        return false;
      }
      read_regs(vfp, m, 1, vm);
      for (unsigned e = 0; e < 64 / esize; e++) {
        set_elem(out, e, 2 * esize,
                 (uint64_t)ext(elem(vm, e, esize), esize, u) << left);
      }
      write_regs(vfp, d, 2, out);
      return true;
    }
    if (a > 9) {
      return false;
    }
    /* VSHRN, VRSHRN, VQSHRN, VQRSHRN, VQSHRUN, VQRSHRUN: a Q register of
     * double-width elements shifted right into a D register. Q is the
     * rounding bit here. */
    uint64_t vm[2], out[1] = {0};
    if (m & 1) {
      return false;
    }
    bool round = q;
    bool src_unsigned = a == 9 && u;
    bool saturate = a == 9 || u;
    bool dst_unsigned = u;
    read_regs(vfp, m, 2, vm);
    for (unsigned e = 0; e < 64 / esize; e++) {
      uint64_t r = shift_right(elem(vm, e, 2 * esize), right, 2 * esize,
                               src_unsigned, round);
      if (!saturate) {
        r &= ones(esize);
      } else if (src_unsigned) {
        r = sat_unsigned_from_unsigned(vfp, r, esize);
      } else if (dst_unsigned) {
        r = sat_unsigned(vfp, sext(r, 2 * esize), esize);
      } else {
        r = sat_signed(vfp, sext(r, 2 * esize), esize);
      }
      set_elem(out, e, esize, r);
    }
    vfp->regs.d[d] = out[0];
    return true;
  }

  unsigned regs = q ? 2 : 1;
  if (q && ((d | m) & 1)) {
    return false;
  }
  if ((a == 4 && !u) || (a == 6 && !u)) {
    return false;
  }
  uint64_t vm[2], vd[2], out[2] = {0, 0};
  read_regs(vfp, m, regs, vm);
  read_regs(vfp, d, regs, vd);
  unsigned elements = regs * 64 / esize;
  for (unsigned e = 0; e < elements; e++) {
    uint64_t x = elem(vm, e, esize);
    uint64_t acc = elem(vd, e, esize);
    uint64_t r;
    switch (a) {
    case 0: /* VSHR */
      r = shift_right(x, right, esize, u, false);
      break;
    case 1: /* VSRA */
      r = acc + shift_right(x, right, esize, u, false);
      break;
    case 2: /* VRSHR */
      r = shift_right(x, right, esize, u, true);
      break;
    case 3: /* VRSRA */
      r = acc + shift_right(x, right, esize, u, true);
      break;
    case 4: { /* VSRI */
      uint64_t mask = lsr(ones(esize), right);
      r = (acc & ~mask) | lsr(x, right);
      break;
    }
    case 5:
      if (u) { /* VSLI */
        uint64_t mask = ones(esize) << left;
        r = (acc & ~mask) | (x << left);
      } else { /* VSHL */
        r = x << left;
      }
      break;
    case 6: /* VQSHLU */
      r = qshift_left(vfp, x, left, esize, false, true);
      break;
    default: /* VQSHL */
      r = qshift_left(vfp, x, left, esize, u, u);
      break;
    }
    set_elem(out, e, esize, r & ones(esize));
  }
  write_regs(vfp, d, regs, out);
  return true;
}

/* ---- One register and a modified immediate ----
 *
 *   1111 001i 1 D 000 imm3 Vd cmode 0 Q op 1 imm4
 */

/* AdvSIMDExpandImm, to 64 bits. */
static bool expand_imm(unsigned op, unsigned cmode, unsigned imm8,
                       uint64_t *out) {
  uint64_t imm = imm8;
  uint64_t r;
  switch (cmode >> 1) {
  case 0:
    r = imm;
    break;
  case 1:
    r = imm << 8;
    break;
  case 2:
    r = imm << 16;
    break;
  case 3:
    r = imm << 24;
    break;
  case 4:
    r = imm | (imm << 16);
    *out = r | (r << 32);
    return true;
  case 5:
    r = (imm << 8) | (imm << 24);
    *out = r | (r << 32);
    return true;
  case 6:
    r = (cmode & 1) ? (imm << 16) | 0xFFFF : (imm << 8) | 0xFF;
    break;
  default:
    if (!(cmode & 1)) {
      if (!op) {
        *out = imm * 0x0101010101010101ull;
        return true;
      }
      r = 0;
      for (unsigned i = 0; i < 8; i++) {
        if ((imm8 >> i) & 1) {
          r |= 0xFFull << (8 * i);
        }
      }
      *out = r;
      return true;
    }
    if (op) {
      return false;
    }
    /* A single-precision constant, as VFP's VMOV immediate has. */
    r = ((imm & 0x80) << 24) | (((imm >> 6) & 1) ? 0x3E000000u : 0x40000000u) |
        ((imm & 0x3F) << 19);
    break;
  }
  *out = r | (r << 32);
  return true;
}

static bool neon_modified_imm(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  unsigned cmode = (opcode >> 8) & 0xF;
  unsigned op = (opcode >> 5) & 1;
  bool q = NEON_Q;
  int d = NEON_VD;
  unsigned imm8 = (((opcode >> 24) & 1) << 7) | (((opcode >> 16) & 7) << 4) |
                  (opcode & 0xF);
  unsigned regs = q ? 2 : 1;
  uint64_t imm;

  if (q && (d & 1)) {
    return false;
  }
  if (!expand_imm(op, cmode, imm8, &imm)) {
    return false;
  }
  /* VORR and VBIC for the odd cmodes below 12; otherwise VMOV and VMVN.
   * cmode 1110 is a VMOV either way: op there picks the byte-mask form. */
  bool logical = (cmode & 1) && cmode < 12;
  bool invert = op && cmode != 0xE;
  for (unsigned r = 0; r < regs; r++) {
    uint64_t *reg = &vfp->regs.d[d + r];
    if (logical) {
      *reg = op ? *reg & ~imm : *reg | imm;
    } else {
      *reg = invert ? ~imm : imm;
    }
  }
  return true;
}

/* ---- Two registers, miscellaneous ----
 *
 *   1111 0011 1 D 11 size A(2) Vd 0 B(5) M 0 Vm
 *
 * with Q as the bottom bit of B. */

static unsigned count_leading_zeroes(uint64_t x, unsigned esize) {
  unsigned n = 0;
  for (int i = (int)esize - 1; i >= 0 && !((x >> i) & 1); i--) {
    n++;
  }
  return n;
}

static bool neon_misc(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  unsigned a = (opcode >> 16) & 3;
  unsigned b = (opcode >> 7) & 0xF;
  bool q = NEON_Q;
  unsigned size = (opcode >> 18) & 3;
  int d = NEON_VD, m = NEON_VM;
  unsigned esize = 8u << size;
  unsigned regs = q ? 2 : 1;
  unsigned elements = regs * 64 / esize;
  uint64_t vm[2], vd[2], out[2] = {0, 0};

  /* The narrowing moves and the widest VSHLL have mixed register widths,
   * so check them before the uniform ones. */
  if (a == 2 && (b == 4 || b == 5)) {
    /* VMOVN (B = 01000), VQMOVUN (01001), VQMOVN (0101x). */
    unsigned op = ((b & 1) << 1) | q;
    if (size == 3 || (m & 1)) {
      return false;
    }
    read_regs(vfp, m, 2, vm);
    out[0] = 0;
    for (unsigned e = 0; e < 64 / esize; e++) {
      uint64_t x = elem(vm, e, 2 * esize);
      uint64_t r;
      switch (op) {
      case 0:
        r = x & ones(esize);
        break;
      case 1:
        r = sat_unsigned(vfp, sext(x, 2 * esize), esize);
        break;
      case 2:
        r = sat_signed(vfp, sext(x, 2 * esize), esize);
        break;
      default:
        r = sat_unsigned_from_unsigned(vfp, x, esize);
        break;
      }
      set_elem(out, e, esize, r);
    }
    vfp->regs.d[d] = out[0];
    return true;
  }
  if (a == 2 && (b == 0xC || b == 0xE)) {
    /* VCVT between single and half precision (B = 1100x narrowing, 1110x
     * widening, with the bottom bit clear), as the armv7s A6 has: four
     * lanes, a Q register of singles and a D register of halves. */
    bool to_single = b == 0xE;
    if (q || size != 1 || (to_single ? (d & 1) : (m & 1))) {
      return false;
    }
    uint32_t saved = fp_enter(vfp);
    if (to_single) {
      read_regs(vfp, m, 1, vm);
      for (unsigned e = 0; e < 4; e++) {
        set_elem(out, e, 32,
                 vfp_half_to_single(vfp, (uint32_t)elem(vm, e, 16)));
      }
      fp_leave(vfp, saved);
      write_regs(vfp, d, 2, out);
    } else {
      read_regs(vfp, m, 2, vm);
      out[0] = 0;
      for (unsigned e = 0; e < 4; e++) {
        float v = vfp_flush_in_s(vfp, f32(elem(vm, e, 32)));
        set_elem(out, e, 16, vfp_single_to_half(vfp, vfp_bits_s(v), 0));
      }
      fp_leave(vfp, saved);
      vfp->regs.d[d] = out[0];
    }
    return true;
  }
  if (a == 2 && b == 6) {
    /* VSHLL by the element size. */
    if (q || size == 3 || (d & 1)) {
      return false;
    }
    read_regs(vfp, m, 1, vm);
    for (unsigned e = 0; e < 64 / esize; e++) {
      set_elem(out, e, 2 * esize, elem(vm, e, esize) << esize);
    }
    write_regs(vfp, d, 2, out);
    return true;
  }

  if (q && ((d | m) & 1)) {
    return false;
  }
  read_regs(vfp, m, regs, vm);
  read_regs(vfp, d, regs, vd);

  switch (a) {
  case 0:
    switch (b) {
    case 0:
    case 1:
    case 2: {
      /* VREV64, VREV32, VREV16: reverse the elements in each group. */
      unsigned group = 64 >> b;
      if (esize >= group) {
        return false;
      }
      unsigned per = group / esize;
      for (unsigned e = 0; e < elements; e++) {
        unsigned base = e - e % per;
        set_elem(out, e, esize, elem(vm, base + (per - 1 - e % per), esize));
      }
      break;
    }
    case 4:
    case 5:
    case 0xC:
    case 0xD: {
      /* VPADDL and VPADAL: adjacent pairs, added at double width. */
      bool u = b & 1;
      bool accumulate = b >= 0xC;
      if (size == 3) {
        return false;
      }
      for (unsigned e = 0; e < elements / 2; e++) {
        uint64_t r = (uint64_t)ext(elem(vm, 2 * e, esize), esize, u) +
                     (uint64_t)ext(elem(vm, 2 * e + 1, esize), esize, u);
        if (accumulate) {
          r += elem(vd, e, 2 * esize);
        }
        set_elem(out, e, 2 * esize, r & ones(2 * esize));
      }
      break;
    }
    case 8:
    case 9:
      /* VCLS, VCLZ. */
      if (size == 3) {
        return false;
      }
      for (unsigned e = 0; e < elements; e++) {
        uint64_t x = elem(vm, e, esize);
        uint64_t r;
        if (b == 9) {
          r = count_leading_zeroes(x, esize);
        } else {
          /* The sign bit itself does not count. */
          uint64_t y = (x >> (esize - 1)) ? ~x & ones(esize) : x;
          r = count_leading_zeroes(y, esize) - 1;
        }
        set_elem(out, e, esize, r);
      }
      break;
    case 0xA:
    case 0xB:
      /* VCNT, VMVN. */
      if (size != 0) {
        return false;
      }
      for (unsigned r = 0; r < regs; r++) {
        if (b == 0xB) {
          out[r] = ~vm[r];
          continue;
        }
        out[r] = 0;
        for (unsigned e = 0; e < 8; e++) {
          uint64_t x = (vm[r] >> (8 * e)) & 0xFF;
          uint64_t count = 0;
          for (; x; x &= x - 1) {
            count++;
          }
          out[r] |= count << (8 * e);
        }
      }
      break;
    case 0xE:
    case 0xF:
      /* VQABS, VQNEG. */
      if (size == 3) {
        return false;
      }
      for (unsigned e = 0; e < elements; e++) {
        int64_t x = sext(elem(vm, e, esize), esize);
        int64_t r = (b == 0xF || x < 0) ? -x : x;
        set_elem(out, e, esize, sat_signed(vfp, r, esize));
      }
      break;
    default:
      return false;
    }
    break;

  case 1: {
    /* Comparisons with zero, VABS and VNEG; B<3> is the float bit. */
    bool is_float = b & 8;
    unsigned op = b & 7;
    if (op == 5 || size == 3 || (is_float && size != 2)) {
      return false;
    }
    uint32_t saved = 0;
    if (is_float) {
      saved = fp_enter(vfp);
    }
    for (unsigned e = 0; e < elements; e++) {
      uint64_t x = elem(vm, e, esize);
      uint64_t r;
      if (is_float) {
        float v = f32(x);
        switch (op) {
        case 0:
          r = fp_compare(vfp, v, 0.0f, CMP_GT);
          break;
        case 1:
          r = fp_compare(vfp, v, 0.0f, CMP_GE);
          break;
        case 2:
          r = fp_compare(vfp, v, 0.0f, CMP_EQ);
          break;
        case 3:
          r = fp_compare(vfp, 0.0f, v, CMP_GE);
          break;
        case 4:
          r = fp_compare(vfp, 0.0f, v, CMP_GT);
          break;
        case 6:
          r = x & 0x7FFFFFFFu;
          break;
        default:
          r = x ^ 0x80000000u;
          break;
        }
        if (op < 5) {
          r = r ? 0xFFFFFFFFu : 0;
        }
      } else {
        int64_t v = sext(x, esize);
        switch (op) {
        case 0:
          r = v > 0;
          break;
        case 1:
          r = v >= 0;
          break;
        case 2:
          r = v == 0;
          break;
        case 3:
          r = v <= 0;
          break;
        case 4:
          r = v < 0;
          break;
        case 6:
          r = (uint64_t)(v < 0 ? -v : v);
          break;
        default:
          r = (uint64_t)-v;
          break;
        }
        if (op < 5) {
          r = r ? ones(esize) : 0;
        }
      }
      set_elem(out, e, esize, r & ones(esize));
    }
    if (is_float) {
      fp_leave(vfp, saved);
    }
    break;
  }

  case 2: {
    /* Permutations, on the destination and Vm together. */
    if (b > 3) {
      return false;
    }
    if (b == 0) {
      /* VSWP */
      if (size != 0) {
        return false;
      }
      write_regs(vfp, d, regs, vm);
      write_regs(vfp, m, regs, vd);
      return true;
    }
    if (size == 3 || (b != 1 && !q && size == 2)) {
      return false;
    }
    if (b == 1 && size == 2) {
      /* VTRN.32 swaps Dd[1] with Dm[0] in each pair of registers. Done in
       * place, in this order, which also fixes the result when Vd == Vm,
       * which is UNKNOWN. */
      for (unsigned r = 0; r < regs; r++) {
        uint64_t *rd = &vfp->regs.d[d + (int)r];
        uint64_t *rm = &vfp->regs.d[m + (int)r];
        uint64_t hi_d = *rd >> 32;
        uint64_t lo_m = *rm & 0xFFFFFFFFu;
        *rm = (*rm & ~0xFFFFFFFFull) | hi_d;
        *rd = (*rd & 0xFFFFFFFFull) | (lo_m << 32);
      }
      return true;
    }
    uint64_t out_m[2] = {0, 0};
    for (unsigned e = 0; e < elements; e++) {
      /* The destination's elements, then Vm's, as one sequence. */
      unsigned lo = 2 * e, hi = 2 * e + 1;
      switch (b) {
      case 1: /* VTRN */
        if (e % 2 == 0) {
          set_elem(out, e, esize, elem(vd, e, esize));
          set_elem(out_m, e, esize, elem(vd, e + 1, esize));
        } else {
          set_elem(out, e, esize, elem(vm, e - 1, esize));
          set_elem(out_m, e, esize, elem(vm, e, esize));
        }
        break;
      case 2: /* VUZP: the even elements, then the odd. */
        set_elem(out, e, esize,
                 lo < elements ? elem(vd, lo, esize)
                               : elem(vm, lo - elements, esize));
        set_elem(out_m, e, esize,
                 hi < elements ? elem(vd, hi, esize)
                               : elem(vm, hi - elements, esize));
        break;
      default: { /* VZIP: interleaved. */
        unsigned z0 = e, z1 = elements + e;
        set_elem(out, e, esize,
                 (z0 & 1) ? elem(vm, z0 / 2, esize) : elem(vd, z0 / 2, esize));
        set_elem(out_m, e, esize,
                 (z1 & 1) ? elem(vm, z1 / 2, esize) : elem(vd, z1 / 2, esize));
        break;
      }
      }
    }
    /* Vm first, so Vd's result is kept when Vd == Vm, which is UNKNOWN. */
    write_regs(vfp, m, regs, out_m);
    write_regs(vfp, d, regs, out);
    return true;
  }

  default:
    if (size != 2) {
      return false;
    }
    if ((b & 0xC) == 0x8) {
      /* VRECPE and VRSQRTE, B<1> being the float bit. */
      bool is_float = b & 2;
      bool rsqrt = b & 1;
      uint32_t saved = fp_enter(vfp);
      for (unsigned e = 0; e < elements; e++) {
        uint32_t x = (uint32_t)elem(vm, e, 32);
        uint32_t r;
        if (is_float) {
          r = rsqrt ? fp_rsqrt_estimate(vfp, x) : fp_recip_estimate(vfp, x);
        } else {
          r = rsqrt ? unsigned_rsqrt_estimate(x) : unsigned_recip_estimate(x);
        }
        set_elem(out, e, 32, r);
      }
      fp_leave(vfp, saved);
    } else if ((b & 0xC) == 0xC) {
      /* VCVT between single precision and 32-bit integers. */
      bool to_int = b & 2;
      bool is_unsigned = b & 1;
      uint32_t saved = fp_enter(vfp);
      for (unsigned e = 0; e < elements; e++) {
        uint32_t x = (uint32_t)elem(vm, e, 32);
        set_elem(out, e, 32,
                 to_int ? fp_to_fixed(vfp, x, 0, is_unsigned)
                        : fixed_to_fp(x, 0, is_unsigned));
      }
      fp_leave(vfp, saved);
    } else {
      return false;
    }
    break;
  }

  write_regs(vfp, d, regs, out);
  return true;
}

/* ---- VEXT, VTBL, VTBX and VDUP (scalar) ---- */

/* VEXT: bytes from the pair Vm:Vn, starting imm4 bytes into Vn.
 *
 *   1111 0010 1 D 11 Vn Vd imm4 N Q M 0 Vm */
static bool neon_ext(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  bool q = NEON_Q;
  int d = NEON_VD, n = NEON_VN, m = NEON_VM;
  unsigned imm4 = (opcode >> 8) & 0xF;
  unsigned regs = q ? 2 : 1;
  unsigned bytes = regs * 8;
  uint64_t v[4], out[2] = {0, 0};

  if (q ? ((d | n | m) & 1) : (imm4 & 8)) {
    return false;
  }
  read_regs(vfp, n, regs, v);
  read_regs(vfp, m, regs, v + regs);
  for (unsigned i = 0; i < bytes; i++) {
    set_elem(out, i, 8, elem(v, i + imm4, 8));
  }
  write_regs(vfp, d, regs, out);
  return true;
}

/* VTBL and VTBX: each byte of Vm indexes a table of one to four
 * consecutive D registers starting at Vn. Out of range gives zero for
 * VTBL and leaves the destination byte alone for VTBX.
 *
 *   1111 0011 1 D 11 Vn Vd 10 len N op M 0 Vm */
static bool neon_table(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  int d = NEON_VD, n = NEON_VN, m = NEON_VM;
  unsigned length = ((opcode >> 8) & 3) + 1;
  bool extension = (opcode >> 6) & 1;
  uint64_t table[4];
  uint64_t index = vfp->regs.d[m];
  uint64_t out = vfp->regs.d[d];

  if (n + length > 32) {
    return false;
  }
  read_regs(vfp, n, length, table);
  for (unsigned i = 0; i < 8; i++) {
    unsigned x = (index >> (8 * i)) & 0xFF;
    if (x < 8 * length) {
      set_elem(&out, i, 8, elem(table, x, 8));
    } else if (!extension) {
      set_elem(&out, i, 8, 0);
    }
  }
  vfp->regs.d[d] = out;
  return true;
}

/* VDUP (scalar): imm4 gives the size and index.
 *
 *   1111 0011 1 D 11 imm4 Vd 1100 0 Q M 0 Vm */
static bool neon_dup_scalar(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  bool q = NEON_Q;
  int d = NEON_VD, m = NEON_VM;
  unsigned imm4 = (opcode >> 16) & 0xF;
  unsigned regs = q ? 2 : 1;
  unsigned esize, index;

  if (imm4 & 1) {
    esize = 8;
    index = imm4 >> 1;
  } else if (imm4 & 2) {
    esize = 16;
    index = imm4 >> 2;
  } else if (imm4 & 4) {
    esize = 32;
    index = imm4 >> 3;
  } else {
    return false;
  }
  if (q && (d & 1)) {
    return false;
  }
  uint64_t x = elem(&vfp->regs.d[m], index, esize);
  uint64_t out = 0;
  for (unsigned e = 0; e < 64 / esize; e++) {
    set_elem(&out, e, esize, x);
  }
  for (unsigned r = 0; r < regs; r++) {
    vfp->regs.d[d + r] = out;
  }
  return true;
}

/* ---- Data-processing dispatch ---- */

static bool neon_data_processing(struct touchHLE_Vfp *vfp, uint32_t opcode) {
  if (!(opcode & (1u << 23))) {
    return neon_three_same(vfp, opcode);
  }
  if (opcode & 0x10) {
    if (!(opcode & 0x80) && !(opcode & 0x00380000)) {
      return neon_modified_imm(vfp, opcode);
    }
    return neon_shift_imm(vfp, opcode);
  }
  if (NEON_SIZE != 3) {
    return (opcode & 0x40) ? neon_scalar(vfp, opcode)
                           : neon_three_different(vfp, opcode);
  }
  if (!NEON_U) {
    return neon_ext(vfp, opcode);
  }
  if (!(opcode & 0x800)) {
    return neon_misc(vfp, opcode);
  }
  if ((opcode & 0xC00) == 0x800) {
    return neon_table(vfp, opcode);
  }
  if ((opcode & 0xF80) == 0xC00) {
    return neon_dup_scalar(vfp, opcode);
  }
  return false;
}

/* ---- Element and structure loads and stores ----
 *
 *   1111 0100 A D L 0 Rn Vd B(4) xxxx Rm
 *
 * A clear is the multiple-structure form, which fills whole registers; A
 * set is one lane, or with B<3:2> == 11 (loads only) one structure
 * replicated to all lanes. Rm == 15 means no writeback, Rm == 13
 * writeback by the size transferred, and anything else writeback by Rm.
 * Alignment is not checked: touchHLE's memory has no alignment faults. */

static uint64_t load_elem(struct ARMCore *cpu, uint32_t address,
                          unsigned ebytes) {
  int cycles = 0;
  switch (ebytes) {
  case 1:
    return (uint64_t)(cpu->memory.load8(cpu, address, &cycles) & 0xFF);
  case 2:
    return (uint64_t)(cpu->memory.load16(cpu, address, &cycles) & 0xFFFF);
  case 4:
    return (uint64_t)(uint32_t)cpu->memory.load32(cpu, address, &cycles);
  default: {
    uint64_t lo = (uint32_t)cpu->memory.load32(cpu, address, &cycles);
    uint64_t hi = (uint32_t)cpu->memory.load32(cpu, address + 4, &cycles);
    return lo | (hi << 32);
  }
  }
}

static void store_elem(struct ARMCore *cpu, uint32_t address, unsigned ebytes,
                       uint64_t v) {
  int cycles = 0;
  switch (ebytes) {
  case 1:
    cpu->memory.store8(cpu, address, (int8_t)v, &cycles);
    break;
  case 2:
    cpu->memory.store16(cpu, address, (int16_t)v, &cycles);
    break;
  case 4:
    cpu->memory.store32(cpu, address, (int32_t)(uint32_t)v, &cycles);
    break;
  default:
    cpu->memory.store32(cpu, address, (int32_t)(uint32_t)v, &cycles);
    cpu->memory.store32(cpu, address + 4, (int32_t)(uint32_t)(v >> 32),
                        &cycles);
    break;
  }
}

/* D register n for the loads and stores. A register list running past D31
 * is UNPREDICTABLE; it is carried out, the registers past D31 being ones of
 * this core's own that no other instruction can see. */
static uint64_t *neon_dreg(struct touchHLE_Vfp *vfp, int n) {
  return n < 32 ? &vfp->regs.d[n] : &vfp->phantom[(n - 32) & 7];
}

static void transfer_elem(struct ARMCore *cpu, struct touchHLE_Vfp *vfp,
                          bool load, int reg, unsigned index, unsigned ebytes,
                          uint32_t address) {
  if (load) {
    set_elem(neon_dreg(vfp, reg), index, ebytes * 8,
             load_elem(cpu, address, ebytes));
  } else {
    store_elem(cpu, address, ebytes,
               elem(neon_dreg(vfp, reg), index, ebytes * 8));
  }
}

static bool neon_load_store(struct ARMCore *cpu, uint32_t opcode) {
  struct touchHLE_Vfp *vfp = neon_vfp(cpu);
  bool load = (opcode >> 21) & 1;
  int rn = (int)((opcode >> 16) & 0xF);
  int rm = (int)(opcode & 0xF);
  int d = NEON_VD;
  unsigned b = (opcode >> 8) & 0xF;
  uint32_t base = (uint32_t)cpu->gprs[rn];
  uint32_t address = base;
  unsigned transferred;

  if (!(opcode & (1u << 23))) {
    /* VLDn/VSTn (multiple structures). */
    unsigned size = (opcode >> 6) & 3;
    unsigned align = (opcode >> 4) & 3;
    unsigned n, regs = 1, inc = 1;
    switch (b) {
    case 7:
    case 0xA:
    case 6:
    case 2:
      n = 1;
      regs = b == 7 ? 1 : b == 0xA ? 2 : b == 6 ? 3 : 4;
      if ((regs == 1 || regs == 3) && (align & 2)) {
        return false;
      }
      if (regs == 2 && align == 3) {
        return false;
      }
      break;
    case 8:
    case 9:
    case 3:
      n = 2;
      inc = b == 8 ? 1 : 2;
      regs = b == 3 ? 2 : 1;
      if (size == 3 || (regs == 1 && align == 3)) {
        return false;
      }
      break;
    case 4:
    case 5:
      n = 3;
      inc = b == 4 ? 1 : 2;
      if (size == 3 || (align & 2)) {
        return false;
      }
      break;
    case 0:
    case 1:
      n = 4;
      inc = b == 0 ? 1 : 2;
      if (size == 3) {
        return false;
      }
      break;
    default:
      return false;
    }
    unsigned ebytes = 1u << size;
    unsigned elements = 8 / ebytes;
    for (unsigned r = 0; r < regs; r++) {
      for (unsigned e = 0; e < elements; e++) {
        for (unsigned i = 0; i < n; i++) {
          transfer_elem(cpu, vfp, load, d + (int)(i * inc + r), e, ebytes,
                        address);
          address += ebytes;
        }
      }
    }
    transferred = 8 * regs * n;
  } else if ((b & 0xC) == 0xC) {
    /* VLDn (single structure to all lanes). */
    unsigned size = (opcode >> 6) & 3;
    bool t = (opcode >> 5) & 1;
    bool a = (opcode >> 4) & 1;
    unsigned n = (b & 3) + 1;
    unsigned regs = 1, inc = 1;
    if (!load) {
      return false;
    }
    switch (n) {
    case 1:
      if (size == 3 || (size == 0 && a)) {
        return false;
      }
      regs = t ? 2 : 1;
      break;
    case 2:
      if (size == 3) {
        return false;
      }
      inc = t ? 2 : 1;
      break;
    case 3:
      if (size == 3 || a) {
        return false;
      }
      inc = t ? 2 : 1;
      break;
    default:
      if (size == 3) {
        if (!a) {
          return false;
        }
        size = 2;
      }
      inc = t ? 2 : 1;
      break;
    }
    unsigned ebytes = 1u << size;
    for (unsigned i = 0; i < n; i++) {
      uint64_t x = load_elem(cpu, address, ebytes);
      uint64_t v = 0;
      for (unsigned e = 0; e < 8 / ebytes; e++) {
        set_elem(&v, e, ebytes * 8, x);
      }
      for (unsigned r = 0; r < regs; r++) {
        *neon_dreg(vfp, d + (int)(i * inc + r)) = v;
      }
      address += ebytes;
    }
    transferred = n * ebytes;
  } else {
    /* VLDn/VSTn (single structure to one lane). */
    unsigned size = b >> 2;
    unsigned n = (b & 3) + 1;
    unsigned ia = (opcode >> 4) & 0xF;
    unsigned index = ia >> (size + 1);
    unsigned inc = 1;
    switch (n) {
    case 1:
      if ((size == 0 && (ia & 1)) || (size == 1 && (ia & 2)) ||
          (size == 2 && ((ia & 4) || ((ia & 3) != 0 && (ia & 3) != 3)))) {
        return false;
      }
      break;
    case 2:
      if (size == 2 && (ia & 2)) {
        return false;
      }
      break;
    case 3:
      if ((size != 2 && (ia & 1)) || (size == 2 && (ia & 3))) {
        return false;
      }
      break;
    default:
      if (size == 2 && (ia & 3) == 3) {
        return false;
      }
      break;
    }
    if (size == 1 && n != 1 && (ia & 2)) {
      inc = 2;
    }
    if (size == 2 && n != 1 && (ia & 4)) {
      inc = 2;
    }
    unsigned ebytes = 1u << size;
    /* Unlike the other forms (see neon_dreg), this is undefined. */
    if (d + (int)((n - 1) * inc) > 31) {
      return false;
    }
    for (unsigned i = 0; i < n; i++) {
      transfer_elem(cpu, vfp, load, d + (int)(i * inc), index, ebytes, address);
      address += ebytes;
    }
    transferred = n * ebytes;
  }

  if (rm != ARM_PC) {
    uint32_t updated =
        base + (rm == ARM_SP ? transferred : (uint32_t)cpu->gprs[rm]);
    /* Rn == 15 is UNPREDICTABLE: the PC is read as usual, and written back
     * as a plain branch. */
    touchHLE_write_core_reg(cpu, rn, updated);
  }
  return true;
}

/* ---- Entry points ---- */

bool touchHLE_neon_raw(struct ARMCore *cpu, uint32_t opcode) {
  if ((opcode & 0xFE000000u) == 0xF2000000u) {
    return neon_data_processing(neon_vfp(cpu), opcode);
  }
  if ((opcode & 0xFF100000u) == 0xF4000000u) {
    return neon_load_store(cpu, opcode);
  }
  return false;
}

/* The transfers between core registers and D register lanes in
 * coprocessor 11's space, which VFP shares for its 32-bit forms:
 *
 *   cond 1110 0 opc1 0 Vd Rt 1011 D opc2 1 0000   VMOV Dd[x], Rt
 *   cond 1110 U opc1 1 Vn Rt 1011 N opc2 1 0000   VMOV Rt, Dn[x]
 *   cond 1110 1 B Q 0 Vd Rt 1011 D 0 E 1 0000     VDUP Qd/Dd, Rt
 *
 * opc1:opc2 give the element size and index. */
bool touchHLE_neon_transfer(struct ARMCore *cpu, uint32_t opcode) {
  struct touchHLE_Vfp *vfp = neon_vfp(cpu);
  bool to_core = (opcode >> 20) & 1;
  bool bit23 = (opcode >> 23) & 1;
  int rt = (int)((opcode >> 12) & 0xF);
  int reg = NEON_VN;

  if (opcode & 0xF) {
    return false;
  }

  if (!to_core && bit23) {
    /* VDUP (core register). */
    unsigned be = (((opcode >> 22) & 1) << 1) | ((opcode >> 5) & 1);
    bool q = (opcode >> 21) & 1;
    unsigned esize = be == 0 ? 32 : be == 1 ? 16 : be == 2 ? 8 : 0;
    if (!esize || (opcode & 0x40) || (q && (reg & 1))) {
      return false;
    }
    uint64_t v = 0;
    for (unsigned e = 0; e < 64 / esize; e++) {
      set_elem(&v, e, esize, (uint32_t)cpu->gprs[rt]);
    }
    vfp->regs.d[reg] = v;
    if (q) {
      vfp->regs.d[reg + 1] = v;
    }
    return true;
  }

  unsigned opc = (((opcode >> 21) & 3) << 2) | ((opcode >> 5) & 3);
  unsigned esize, index;
  if (opc & 8) {
    esize = 8;
    index = opc & 7;
  } else if (opc & 1) {
    esize = 16;
    index = (opc >> 1) & 3;
  } else if (!(opc & 2)) {
    esize = 32;
    index = (opc >> 2) & 1;
  } else {
    return false;
  }
  if (to_core) {
    bool is_unsigned = bit23;
    if (esize == 32 && is_unsigned) {
      return false;
    }
    uint64_t x = elem(&vfp->regs.d[reg], index, esize);
    touchHLE_write_core_reg(cpu, rt, (uint32_t)ext(x, esize, is_unsigned));
  } else {
    set_elem(&vfp->regs.d[reg], index, esize, (uint32_t)cpu->gprs[rt]);
  }
  return true;
}
