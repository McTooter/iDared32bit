/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Floating-point primitives shared by VFP (vfp.c) and Advanced SIMD
 * (neon.c): FPSCR's fields, register access, and the architecture's rules
 * for NaNs, flushing to zero and conversion to fixed point, wrapped around
 * the host's IEEE-754 arithmetic. */

#ifndef TOUCHHLE_MGBA_WRAPPER_FP_H
#define TOUCHHLE_MGBA_WRAPPER_FP_H

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <string.h>

#include "wrapper.h"

/* FPSCR fields. The comparison flags use the same encoding as the ARM
 * condition flags, which is why VMRS APSR_nzcv can copy them straight
 * across. Of the cumulative exception bits, IOC, IDC and UFC are maintained
 * for the cases detected explicitly below (NaNs and invalid operations,
 * out-of-range conversions, and flushing to zero); the rest would mean
 * reading the host's exception state after every operation, which nothing
 * in a guest looks at. */
#define FPSCR_NZCV 0xF0000000u
#define FPSCR_N (1u << 31)
#define FPSCR_Z (1u << 30)
#define FPSCR_C (1u << 29)
#define FPSCR_V (1u << 28)
#define FPSCR_IOC (1u << 0)
#define FPSCR_RMODE(FPSCR) (((FPSCR) >> 22) & 3)

/* FPSCR's control bits, beyond the rounding mode. With FZ set, denormal
 * inputs are read as zero and tiny results written as zero (setting IDC and
 * UFC); with DN set, any NaN result is the default NaN. LEN and STRIDE make
 * most data-processing instructions operate on short vectors; see
 * vfp_vector_length. */
#define FPSCR_FZ (1u << 24)
#define FPSCR_DN (1u << 25)
/* Alternative half precision: no infinities or NaNs, and one more
 * exponent's worth of range. */
#define FPSCR_AHP (1u << 26)
#define FPSCR_IDC (1u << 7)
#define FPSCR_UFC (1u << 3)
#define FPSCR_LEN(FPSCR) (((FPSCR) >> 16) & 7)
#define FPSCR_STRIDE(FPSCR) (((FPSCR) >> 20) & 3)

static inline float vfp_get_s(const struct touchHLE_Vfp *vfp, int n) {
  float v;
  memcpy(&v, &vfp->regs.s[n], sizeof(v));
  return v;
}

static inline void vfp_set_s(struct touchHLE_Vfp *vfp, int n, float v) {
  memcpy(&vfp->regs.s[n], &v, sizeof(v));
}

static inline double vfp_get_d(const struct touchHLE_Vfp *vfp, int n) {
  double v;
  memcpy(&v, &vfp->regs.d[n], sizeof(v));
  return v;
}

static inline void vfp_set_d(struct touchHLE_Vfp *vfp, int n, double v) {
  memcpy(&vfp->regs.d[n], &v, sizeof(v));
}

/* Whether |a * b|, or |a / b|, is below `min_normal` before rounding, for a
 * product or quotient that the host rounded to exactly `min_normal`. Scaled
 * by 2^128, clear of the subnormal range, and rounded toward zero, the
 * computed value is below the scaled bound exactly when the true value is.
 * The operands are normal (FZ is set, so they were flushed) and small enough
 * that the scaling cannot overflow. */
static inline bool vfp_below_before_rounding(double a, double b, bool divide,
                                             double min_normal) {
  int saved = fegetround();
  fesetround(FE_TOWARDZERO);
  volatile double scaled =
      divide ? ldexp(fabs(a), 128) / fabs(b) : ldexp(fabs(a), 128) * fabs(b);
  fesetround(saved);
  return scaled < ldexp(min_normal, 128);
}

/* The architecture's floating-point primitives -- FPUnpack's flushing of
 * denormal inputs, FPProcessNaNs, and FPRound's default NaN and flushing of
 * tiny results -- wrapped around the host's arithmetic, for each precision.
 *
 * The NaN rules matter even without DN: a NaN operand is propagated by
 * operand order (the first signalling NaN, quietened, else the first quiet
 * NaN), and an invalid operation produces the positive default NaN. Hosts
 * differ on both -- x86 produces a negative NaN -- so leaving them to the
 * host would give different results on different machines. */
#define DEFINE_VFP_PRECISION(SUFFIX, FLOAT, UINT, EXP_MASK, FRAC_MASK,         \
                             QUIET_BIT, SIGN_BIT, DEFAULT_NAN, MIN_NORMAL,     \
                             SQRT)                                             \
  static inline FLOAT vfp_from_bits_##SUFFIX(UINT bits) {                      \
    FLOAT v;                                                                   \
    memcpy(&v, &bits, sizeof(v));                                              \
    return v;                                                                  \
  }                                                                            \
                                                                               \
  static inline UINT vfp_bits_##SUFFIX(FLOAT v) {                              \
    UINT bits;                                                                 \
    memcpy(&bits, &v, sizeof(bits));                                           \
    return bits;                                                               \
  }                                                                            \
                                                                               \
  static inline bool vfp_is_nan_##SUFFIX(UINT bits) {                          \
    return (bits & (EXP_MASK)) == (EXP_MASK) && (bits & (FRAC_MASK)) != 0;     \
  }                                                                            \
                                                                               \
  /* A signalling NaN has the fraction's top bit clear. */                     \
  static inline bool vfp_is_snan_##SUFFIX(UINT bits) {                         \
    return vfp_is_nan_##SUFFIX(bits) && !(bits & (QUIET_BIT));                 \
  }                                                                            \
                                                                               \
  static inline FLOAT vfp_neg_##SUFFIX(FLOAT v) {                              \
    return vfp_from_bits_##SUFFIX(vfp_bits_##SUFFIX(v) ^ (SIGN_BIT));          \
  }                                                                            \
                                                                               \
  static inline FLOAT vfp_flush_in_##SUFFIX(struct touchHLE_Vfp *vfp,          \
                                            FLOAT v) {                         \
    if ((vfp->fpscr & FPSCR_FZ) && fpclassify(v) == FP_SUBNORMAL) {            \
      vfp->fpscr |= FPSCR_IDC;                                                 \
      return vfp_from_bits_##SUFFIX(vfp_bits_##SUFFIX(v) & (SIGN_BIT));        \
    }                                                                          \
    return v;                                                                  \
  }                                                                            \
                                                                               \
  /* FPProcessNaNs, over one or two operands. */                               \
  static inline bool vfp_nans_##SUFFIX(struct touchHLE_Vfp *vfp, FLOAT a,      \
                                       FLOAT b, int count, FLOAT *out) {       \
    UINT ops[2] = {vfp_bits_##SUFFIX(a), vfp_bits_##SUFFIX(b)};                \
    int pick = -1;                                                             \
    for (int i = 0; i < count && pick < 0; i++) {                              \
      if (vfp_is_snan_##SUFFIX(ops[i])) {                                      \
        vfp->fpscr |= FPSCR_IOC;                                               \
        pick = i;                                                              \
      }                                                                        \
    }                                                                          \
    for (int i = 0; i < count && pick < 0; i++) {                              \
      if (vfp_is_nan_##SUFFIX(ops[i])) {                                       \
        pick = i;                                                              \
      }                                                                        \
    }                                                                          \
    if (pick < 0) {                                                            \
      return false;                                                            \
    }                                                                          \
    UINT result =                                                              \
        (vfp->fpscr & FPSCR_DN) ? (DEFAULT_NAN) : ops[pick] | (QUIET_BIT);     \
    *out = vfp_from_bits_##SUFFIX(result);                                     \
    return true;                                                               \
  }                                                                            \
                                                                               \
  /* The result of an operation none of whose inputs was a NaN: a NaN here     \
   * means an invalid operation. */                                            \
  static inline FLOAT vfp_result_##SUFFIX(struct touchHLE_Vfp *vfp, FLOAT r) { \
    if (isnan(r)) {                                                            \
      vfp->fpscr |= FPSCR_IOC;                                                 \
      return vfp_from_bits_##SUFFIX(DEFAULT_NAN);                              \
    }                                                                          \
    if ((vfp->fpscr & FPSCR_FZ) && fpclassify(r) == FP_SUBNORMAL) {            \
      vfp->fpscr |= FPSCR_UFC;                                                 \
      return vfp_from_bits_##SUFFIX(vfp_bits_##SUFFIX(r) & (SIGN_BIT));        \
    }                                                                          \
    return r;                                                                  \
  }                                                                            \
                                                                               \
  /* The result of a multiplication or division. FZ flushes a result that was  \
   * tiny before rounding, as ARM detects tininess; the host's result shows    \
   * that unless it rounded to zero or up to the smallest normal number.       \
   * Sums and differences need none of this: in the subnormal range they are   \
   * exact. */                                                                 \
  static inline FLOAT vfp_scaled_result_##SUFFIX(                              \
      struct touchHLE_Vfp *vfp, FLOAT r, FLOAT a, FLOAT b, bool divide) {      \
    if ((vfp->fpscr & FPSCR_FZ) && !isnan(r)) {                                \
      bool tiny =                                                              \
          r == 0 ? a != 0 && b != 0 && isfinite(a) && isfinite(b)              \
                 : fabs(r) == (MIN_NORMAL) &&                                  \
                       vfp_below_before_rounding(a, b, divide, (MIN_NORMAL));  \
      if (tiny) {                                                              \
        vfp->fpscr |= FPSCR_UFC;                                               \
        return vfp_from_bits_##SUFFIX(vfp_bits_##SUFFIX(r) & (SIGN_BIT));      \
      }                                                                        \
    }                                                                          \
    return vfp_result_##SUFFIX(vfp, r);                                        \
  }                                                                            \
                                                                               \
  static inline FLOAT vfp_add_##SUFFIX(struct touchHLE_Vfp *vfp, FLOAT a,      \
                                       FLOAT b) {                              \
    FLOAT nan;                                                                 \
    a = vfp_flush_in_##SUFFIX(vfp, a);                                         \
    b = vfp_flush_in_##SUFFIX(vfp, b);                                         \
    if (vfp_nans_##SUFFIX(vfp, a, b, 2, &nan)) {                               \
      return nan;                                                              \
    }                                                                          \
    return vfp_result_##SUFFIX(vfp, a + b);                                    \
  }                                                                            \
                                                                               \
  static inline FLOAT vfp_sub_##SUFFIX(struct touchHLE_Vfp *vfp, FLOAT a,      \
                                       FLOAT b) {                              \
    FLOAT nan;                                                                 \
    a = vfp_flush_in_##SUFFIX(vfp, a);                                         \
    b = vfp_flush_in_##SUFFIX(vfp, b);                                         \
    if (vfp_nans_##SUFFIX(vfp, a, b, 2, &nan)) {                               \
      return nan;                                                              \
    }                                                                          \
    return vfp_result_##SUFFIX(vfp, a - b);                                    \
  }                                                                            \
                                                                               \
  static inline FLOAT vfp_mul_##SUFFIX(struct touchHLE_Vfp *vfp, FLOAT a,      \
                                       FLOAT b) {                              \
    FLOAT nan;                                                                 \
    a = vfp_flush_in_##SUFFIX(vfp, a);                                         \
    b = vfp_flush_in_##SUFFIX(vfp, b);                                         \
    if (vfp_nans_##SUFFIX(vfp, a, b, 2, &nan)) {                               \
      return nan;                                                              \
    }                                                                          \
    return vfp_scaled_result_##SUFFIX(vfp, a * b, a, b, false);                \
  }                                                                            \
                                                                               \
  static inline FLOAT vfp_div_##SUFFIX(struct touchHLE_Vfp *vfp, FLOAT a,      \
                                       FLOAT b) {                              \
    FLOAT nan;                                                                 \
    a = vfp_flush_in_##SUFFIX(vfp, a);                                         \
    b = vfp_flush_in_##SUFFIX(vfp, b);                                         \
    if (vfp_nans_##SUFFIX(vfp, a, b, 2, &nan)) {                               \
      return nan;                                                              \
    }                                                                          \
    return vfp_scaled_result_##SUFFIX(vfp, a / b, a, b, true);                 \
  }                                                                            \
                                                                               \
  static inline FLOAT vfp_sqrt_##SUFFIX(struct touchHLE_Vfp *vfp, FLOAT a) {   \
    FLOAT nan;                                                                 \
    a = vfp_flush_in_##SUFFIX(vfp, a);                                         \
    if (vfp_nans_##SUFFIX(vfp, a, a, 1, &nan)) {                               \
      return nan;                                                              \
    }                                                                          \
    return vfp_result_##SUFFIX(vfp, SQRT(a));                                  \
  }                                                                            \
                                                                               \
  /* One element of the three-operand group. The accumulating forms are a      \
   * rounded product and a separate add, the way the architecture defines      \
   * them -- VMLS adds the negated product rather than subtracting it, which   \
   * differs for signed zeroes and NaNs. */                                    \
  static inline FLOAT vfp_arith_##SUFFIX(                                      \
      struct touchHLE_Vfp *vfp, unsigned op, FLOAT d, FLOAT a, FLOAT b) {      \
    switch (op) {                                                              \
    case 0: /* VMLA */                                                         \
      return vfp_add_##SUFFIX(vfp, d, vfp_mul_##SUFFIX(vfp, a, b));            \
    case 1: /* VMLS */                                                         \
      return vfp_add_##SUFFIX(vfp, d,                                          \
                              vfp_neg_##SUFFIX(vfp_mul_##SUFFIX(vfp, a, b)));  \
    case 2: /* VNMLS */                                                        \
      return vfp_add_##SUFFIX(vfp, vfp_neg_##SUFFIX(d),                        \
                              vfp_mul_##SUFFIX(vfp, a, b));                    \
    case 3: /* VNMLA */                                                        \
      return vfp_add_##SUFFIX(vfp, vfp_neg_##SUFFIX(d),                        \
                              vfp_neg_##SUFFIX(vfp_mul_##SUFFIX(vfp, a, b)));  \
    case 4: /* VMUL */                                                         \
      return vfp_mul_##SUFFIX(vfp, a, b);                                      \
    case 5: /* VNMUL */                                                        \
      return vfp_neg_##SUFFIX(vfp_mul_##SUFFIX(vfp, a, b));                    \
    case 6: /* VADD */                                                         \
      return vfp_add_##SUFFIX(vfp, a, b);                                      \
    case 7: /* VSUB */                                                         \
      return vfp_sub_##SUFFIX(vfp, a, b);                                      \
    default: /* VDIV */                                                        \
      return vfp_div_##SUFFIX(vfp, a, b);                                      \
    }                                                                          \
  }

DEFINE_VFP_PRECISION(s, float, uint32_t, 0x7F800000u, 0x007FFFFFu, 0x00400000u,
                     0x80000000u, 0x7FC00000u, FLT_MIN, sqrtf)
DEFINE_VFP_PRECISION(d, double, uint64_t, 0x7FF0000000000000ull,
                     0x000FFFFFFFFFFFFFull, 0x0008000000000000ull,
                     0x8000000000000000ull, 0x7FF8000000000000ull, DBL_MIN,
                     sqrt)

/* VFPv4's fused multiply-add, FPMulAdd: addend + a * b with a single
 * rounding, which the host's fma does. Around it are the architecture's
 * rules: NaNs are picked in the order addend, a, b, and a quiet NaN addend
 * does not save infinity times zero from being an Invalid Operation. With
 * FZ, a result that is tiny before rounding is flushed; the host's result
 * shows that directly except when it is zero, or the smallest normal, which
 * are rare enough to recompute (see vfp_below_before_rounding for the
 * reasoning behind rounding toward zero). */
#define DEFINE_VFP_FUSED(SUFFIX, FLOAT, UINT, QUIET_BIT, SIGN_BIT,             \
                         DEFAULT_NAN, MIN_NORMAL, FMA)                         \
  static inline bool vfp_nans3_##SUFFIX(struct touchHLE_Vfp *vfp, FLOAT x,     \
                                        FLOAT y, FLOAT z, FLOAT *out) {        \
    UINT ops[3] = {vfp_bits_##SUFFIX(x), vfp_bits_##SUFFIX(y),                 \
                   vfp_bits_##SUFFIX(z)};                                      \
    int pick = -1;                                                             \
    for (int i = 0; i < 3 && pick < 0; i++) {                                  \
      if (vfp_is_snan_##SUFFIX(ops[i])) {                                      \
        vfp->fpscr |= FPSCR_IOC;                                               \
        pick = i;                                                              \
      }                                                                        \
    }                                                                          \
    for (int i = 0; i < 3 && pick < 0; i++) {                                  \
      if (vfp_is_nan_##SUFFIX(ops[i])) {                                       \
        pick = i;                                                              \
      }                                                                        \
    }                                                                          \
    if (pick < 0) {                                                            \
      return false;                                                            \
    }                                                                          \
    UINT result =                                                              \
        (vfp->fpscr & FPSCR_DN) ? (DEFAULT_NAN) : ops[pick] | (QUIET_BIT);     \
    *out = vfp_from_bits_##SUFFIX(result);                                     \
    return true;                                                               \
  }                                                                            \
                                                                               \
  static inline FLOAT vfp_fused_##SUFFIX(struct touchHLE_Vfp *vfp,             \
                                         FLOAT addend, FLOAT a, FLOAT b) {     \
    FLOAT nan;                                                                 \
    addend = vfp_flush_in_##SUFFIX(vfp, addend);                               \
    a = vfp_flush_in_##SUFFIX(vfp, a);                                         \
    b = vfp_flush_in_##SUFFIX(vfp, b);                                         \
    bool inf_zero = (isinf(a) && b == 0) || (a == 0 && isinf(b));              \
    UINT addend_bits = vfp_bits_##SUFFIX(addend);                              \
    if (inf_zero && vfp_is_nan_##SUFFIX(addend_bits) &&                        \
        !vfp_is_snan_##SUFFIX(addend_bits)) {                                  \
      vfp->fpscr |= FPSCR_IOC;                                                 \
      return vfp_from_bits_##SUFFIX(DEFAULT_NAN);                              \
    }                                                                          \
    if (vfp_nans3_##SUFFIX(vfp, addend, a, b, &nan)) {                         \
      return nan;                                                              \
    }                                                                          \
    FLOAT r = FMA(a, b, addend);                                               \
    if ((vfp->fpscr & FPSCR_FZ) && isfinite(r) &&                              \
        fpclassify(r) != FP_SUBNORMAL) {                                       \
      bool tiny = false;                                                       \
      if (r == 0 && a != 0 && b != 0) {                                        \
        /* Zero from nonzero terms: exact cancellation, or underflow, which    \
         * is inexact. */                                                      \
        feclearexcept(FE_INEXACT);                                             \
        volatile FLOAT again = FMA(a, b, addend);                              \
        (void)again;                                                           \
        tiny = fetestexcept(FE_INEXACT) != 0;                                  \
      } else if (fabs(r) == (MIN_NORMAL)) {                                    \
        int saved = fegetround();                                              \
        fesetround(FE_TOWARDZERO);                                             \
        volatile FLOAT toward_zero = FMA(a, b, addend);                        \
        fesetround(saved);                                                     \
        tiny = fabs(toward_zero) < (MIN_NORMAL);                               \
      }                                                                        \
      if (tiny) {                                                              \
        vfp->fpscr |= FPSCR_UFC;                                               \
        return vfp_from_bits_##SUFFIX(vfp_bits_##SUFFIX(r) & (SIGN_BIT));      \
      }                                                                        \
    }                                                                          \
    return vfp_result_##SUFFIX(vfp, r);                                        \
  }

DEFINE_VFP_FUSED(s, float, uint32_t, 0x00400000u, 0x80000000u, 0x7FC00000u,
                 FLT_MIN, fmaf)
DEFINE_VFP_FUSED(d, double, uint64_t, 0x0008000000000000ull,
                 0x8000000000000000ull, 0x7FF8000000000000ull, DBL_MIN, fma)

/* FPHalfToSingle: exact, so only NaNs need care. FZ does not apply to half
 * precision; AHP makes the top exponent an ordinary one. */
static inline uint32_t vfp_half_to_single(struct touchHLE_Vfp *vfp,
                                          uint32_t h) {
  uint32_t sign = ((h >> 15) & 1) << 31;
  unsigned exp = (h >> 10) & 0x1F;
  uint32_t frac = h & 0x3FF;
  if (exp == 0x1F && !(vfp->fpscr & FPSCR_AHP)) {
    if (!frac) {
      return sign | 0x7F800000u;
    }
    if (!(frac & 0x200)) {
      vfp->fpscr |= FPSCR_IOC;
    }
    if (vfp->fpscr & FPSCR_DN) {
      return 0x7FC00000u;
    }
    return sign | 0x7FC00000u | ((frac & 0x1FF) << 13);
  }
  if (!exp) {
    if (!frac) {
      return sign;
    }
    int e = -14;
    while (!(frac & 0x400)) {
      frac <<= 1;
      e--;
    }
    return sign | ((uint32_t)(e + 127) << 23) | ((frac & 0x3FF) << 13);
  }
  return sign | ((uint32_t)(exp - 15 + 127) << 23) | (frac << 13);
}

/* FPSingleToHalf, with FPRound's 16-bit case written out: the given rounding
 * mode (FPSCR's encoding), no flushing of the result, Underflow (UFC) for a
 * result that is tiny before rounding and inexact, and with AHP saturation
 * instead of infinities. The input should already have been flushed if FZ
 * applies to it. */
static inline uint32_t vfp_single_to_half(struct touchHLE_Vfp *vfp, uint32_t x,
                                          unsigned rmode) {
  uint32_t sign = ((x >> 31) & 1) << 15;
  unsigned exp = (x >> 23) & 0xFF;
  uint32_t mant = x & 0x7FFFFF;
  bool ahp = vfp->fpscr & FPSCR_AHP;
  if (exp == 0xFF) {
    if (mant) {
      if (ahp) {
        /* A signed zero, as ARMv8's pseudocode has it (ARMv7's says
         * +0). */
        vfp->fpscr |= FPSCR_IOC;
        return sign;
      }
      if (!(mant & 0x400000)) {
        vfp->fpscr |= FPSCR_IOC;
      }
      if (vfp->fpscr & FPSCR_DN) {
        return 0x7E00;
      }
      return sign | 0x7E00 | ((mant >> 13) & 0x1FF);
    }
    if (ahp) {
      vfp->fpscr |= FPSCR_IOC;
      return sign | 0x7FFF;
    }
    return sign | 0x7C00;
  }
  if (!exp && !mant) {
    return sign;
  }
  /* value = m * 2^e, and its exponent is that of m's top bit plus e. */
  uint64_t m = exp ? (mant | 0x800000) : mant;
  int e = exp ? (int)exp - 127 - 23 : -126 - 23;
  int top = 23;
  while (!((m >> top) & 1)) {
    top--;
  }
  int exponent = top + e;
  int biased = exponent + 15 > 0 ? exponent + 15 : 0;
  /* The unit of the last place of the result, 2^(unit_exp). */
  int unit_exp = (biased ? exponent : -14) - 10;
  int shift = unit_exp - e;
  uint64_t int_mant, rem = 0, half = 0;
  if (shift <= 0) {
    int_mant = m << -shift;
  } else if (shift < 60) {
    int_mant = m >> shift;
    rem = m & ((1ull << shift) - 1);
    half = 1ull << (shift - 1);
  } else {
    int_mant = 0;
    rem = m;
    half = ~0ull;
  }
  if (!biased && rem) {
    vfp->fpscr |= FPSCR_UFC;
  }
  bool negative = sign != 0;
  bool round_up, overflow_to_inf;
  switch (rmode) {
  case 0:
    round_up = rem > half || (rem == half && rem && (int_mant & 1));
    overflow_to_inf = true;
    break;
  case 1:
    round_up = rem && !negative;
    overflow_to_inf = !negative;
    break;
  case 2:
    round_up = rem && negative;
    overflow_to_inf = negative;
    break;
  default:
    round_up = false;
    overflow_to_inf = false;
    break;
  }
  if (round_up) {
    int_mant++;
    if (int_mant == 0x400) {
      biased = 1;
    }
    if (int_mant == 0x800) {
      biased++;
      int_mant >>= 1;
    }
  }
  if (!ahp) {
    if (biased >= 31) {
      return sign | (overflow_to_inf ? 0x7C00 : 0x7BFF);
    }
  } else if (biased >= 32) {
    vfp->fpscr |= FPSCR_IOC;
    return sign | 0x7FFF;
  }
  return sign | ((uint32_t)biased << 10) | (uint32_t)(int_mant & 0x3FF);
}

/* FPToFixed, for VCVT to fixed point: scale by 2^frac_bits, round toward
 * zero, and saturate to `size` bits, raising Invalid Operation if that
 * changed anything. The result comes back sign- or zero-extended to 32
 * bits, as the register write wants it. */
static inline uint32_t vfp_to_fixed(struct touchHLE_Vfp *vfp, double v,
                                    int frac_bits, unsigned size,
                                    bool is_unsigned) {
  if (isnan(v)) {
    vfp->fpscr |= FPSCR_IOC;
    return 0;
  }
  /* Exact: scaling by a power of two only changes the exponent. */
  double r = trunc(ldexp(v, frac_bits));
  double max = is_unsigned ? ldexp(1.0, size) - 1 : ldexp(1.0, size - 1) - 1;
  double min = is_unsigned ? 0.0 : -ldexp(1.0, size - 1);
  if (r > max) {
    vfp->fpscr |= FPSCR_IOC;
    r = max;
  } else if (r < min) {
    vfp->fpscr |= FPSCR_IOC;
    r = min;
  }
  return (uint32_t)(int64_t)r;
}

#endif
