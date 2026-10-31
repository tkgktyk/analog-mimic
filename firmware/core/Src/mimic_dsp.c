/**
 ******************************************************************************
 * @file    mimic_dsp.c
 * @author  TAKAGI Katsuyuki
 * @brief   Implementation of the Core Emulation Engines, Q15 Fixed-Point DSP 
 *          Routines, and Deterministic Operational Mode Execution Blocks.
 *-----------------------------------------------------------------------------
 * Copyright (C) 2026 TAKAGI Katsuyuki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 ******************************************************************************
 */

#include "mimic_dsp.h"
#include "py32f0xx_hal.h"
#include "mimic_device.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// =========================================================
// Deterministic Output Slot (Shared between ISR and DMA)
// =========================================================
static volatile uint32_t mimic_output_slot = MIMIC_ADC_MID_VALUE;

// =========================================================
// Fixed-Point Arithmetic Configurations
// =========================================================
// Number formats (see also mimic_registers.h)
//   _raw          : ADC / DAC code, 0 = 0 V, 4096 = VDD (integer, 0 - 4095).
//                   Used only where the hardware is touched (ADC read, DAC write).
//   _raw_fx3      : raw << 3, 1 LSB = 8. Every signal and voltage inside the DSP:
//                   the DSP input, the mode outputs, thresholds and Vref.
//   _raw_vref_fx3 : raw_fx3 - Vref, relative to the common Vref, 1 LSB = 8
//   _raw_fx16     : raw << 16, 1 LSB = 65536 (decimation interpolation)
//   _q0_15        : signed coefficient, 15 fractional bits (-1.0 to < 1.0)
//   _q1_15        : signed multiplier, 15 fractional bits (-2.0 to < 2.0, int32)
//   _uq1_15       : unsigned gain, 15 fractional bits (0 to < 2.0, 1.0 = 32768)
//   _mant / _exp  : mantissa (q0_15) and exponent (shift) pair
#define RAW_FX3_FRAC_BITS   MIMIC_RAW_FX3_FRAC_BITS
#define Q15_FRAC_BITS       15 // Fractional bits of q0_15 / q1_15 / uq1_15
#define FX16_FRAC_BITS      16 // Fractional bits of raw_fx16 (decimation interpolation)
#define OUT_FRAC_BITS       (Q15_FRAC_BITS + RAW_FX3_FRAC_BITS)   // raw_fx3 * q1_15 -> raw
#define OUT_ROUND_RAW_FX18  (1 << (OUT_FRAC_BITS - 1))            // 0.5 LSB (rounding)
#define RAW_MAX_FX3         (MIMIC_ADC_MAX_VALUE << RAW_FX3_FRAC_BITS)
#define RAW_MID_FX3         (MIMIC_ADC_MID_VALUE << RAW_FX3_FRAC_BITS)

// Overflow guards: values beyond these saturate in the output stage anyway.
// OUT_GUARD_RAW_FX3 also bounds the output stage: |dsp_out| <= 65535 and |delta| <= 32768
// keep dsp_out * delta + offset_frac within int32 (see MimicDSP_ProcessSample).
#define OUT_GUARD_RAW       (2 * (MIMIC_ADC_MAX_VALUE + 1))                    // +/-8192 LSB
#define OUT_GUARD_RAW_FX3   ((OUT_GUARD_RAW << RAW_FX3_FRAC_BITS) - 1)         // 65535
// Recursive states (integral, envelope) keep fractional bits below raw_fx3, so that the
// truncation of state * coefficient does not accumulate (no dead band, smooth decay).
#define INTEG_FRAC_BITS     14                                  // integral state: raw_vref_fx3 << 14
#define INTEG_STATE_LIM     (((1 << 16) - 1) << INTEG_FRAC_BITS) // |y| < 2^16 raw_fx3 (= +/-8192 LSB)
#define ENV_FRAC_BITS       15                                  // envelope state: raw_vref_fx3 << 15
#define COEF12_SHIFT_MAX    40      // mant12 / sh coefficients: c >= 2^-29 (tau up to hours)

// =========================================================
// DSP Specific Constants
// =========================================================
// LOG mode: per-mode tables in raw << LOG_OUT_FRAC_BITS (= raw_fx3 with 4 more bits)
#define LOG_SEG_BITS        6                              // 64 segments per octave
#define LOG_OUT_FRAC_BITS   7
#define LOG_TO_FX3_SHIFT    (LOG_OUT_FRAC_BITS - RAW_FX3_FRAC_BITS)   // raw << 7 -> raw_fx3
#define LOG_TO_FX3_ROUND    (1 << (LOG_TO_FX3_SHIFT - 1))
#define LOG_X_BITS          15                             // LOG input: raw_fx3, 1 - 32767
#define LOG_OCT_COUNT       LOG_X_BITS                     // e = floor(log2(x)) = 0 - 14
#define LOG_F_BITS          (LOG_X_BITS - 1)               // mantissa fraction bits
#define LOG10_2_Q16         19728                          // log10(2) * 2^16
#define ANTILOG_T_FRAC_BITS 20                             // octaves, Q20
#define ANTILOG_MAX_OCT     13                             // beyond this the output saturates
#define ANTILOG_LIM         (OUT_GUARD_RAW << LOG_OUT_FRAC_BITS)
// Biquad mode: TPT state-variable filter (bilinear, same response as a cookbook biquad).
// Signals are raw_vref_fx3; the two integrator states have SVF_STATE_FRAC_BITS fractional
// bits (raw << 15) so that low cutoffs (down to fs / 100000) keep moving.
#define SVF_STATE_FRAC_BITS 15                                       // states: raw << 15
#define SVF_STATE_TO_FX3    (SVF_STATE_FRAC_BITS - RAW_FX3_FRAC_BITS) // state -> raw_vref_fx3
#define SVF_G1_FRAC_BITS    10                                       // g1 = k + g
#define SVF_K_FRAC_BITS     11                                       // k = 1 / Q
#define SVF_SHIFT_MAX       30

// =========================================================
// Base Tables (LOG mode, used at configuration time)
// =========================================================
// The per-sample tables are scaled from these in MimicDSP_UpdateParameters() and kept
// in the mode context (SRAM), so the sampling path only reads RAM.
// log2(1 + i/64) in Q15 (i = 0 - 64), used at configuration time
static const int32_t log2_1pf_q15[65] = {
    0, 733, 1455, 2166, 2866, 3556, 4236, 4907, 5568, 6220,
    6863, 7498, 8124, 8742, 9352, 9954, 10549, 11136, 11716, 12289,
    12855, 13415, 13968, 14514, 15055, 15589, 16117, 16639, 17156, 17667,
    18173, 18673, 19168, 19658, 20143, 20623, 21098, 21568, 22034, 22495,
    22952, 23404, 23852, 24296, 24736, 25172, 25604, 26031, 26455, 26876,
    27292, 27705, 28114, 28520, 28922, 29321, 29717, 30109, 30498, 30884,
    31267, 31647, 32024, 32397, 32768
};

// 2^(i/64) in Q15 (i = 0 - 64), used at configuration time
static const int32_t exp2_f_q15[65] = {
    32768, 33125, 33486, 33850, 34219, 34591, 34968, 35349, 35734, 36123,
    36516, 36914, 37316, 37722, 38133, 38548, 38968, 39392, 39821, 40255,
    40693, 41136, 41584, 42037, 42495, 42958, 43425, 43898, 44376, 44859,
    45348, 45842, 46341, 46846, 47356, 47871, 48393, 48920, 49452, 49991,
    50535, 51085, 51642, 52204, 52773, 53347, 53928, 54515, 55109, 55709,
    56316, 56929, 57549, 58176, 58809, 59449, 60097, 60751, 61413, 62081,
    62757, 63441, 64132, 64830, 65536
};

// =========================================================
// 1. DSP Internal State Variables (Exclusive to Sampling ISR)
// =========================================================
// Note: 'volatile' is omitted on internal DSP states because the sampling engine
// is strictly stopped before MimicDSP_UpdateParameters() modifies them from Main.

typedef struct {
    // Output stage: mult = sign * 1.0 + delta, offset = offset_int (raw_fx3) + offset_frac
    // (see MimicDSP_ProcessSample)
    int32_t            out_sign;            // +1, or -1 when inverted
    int32_t            out_delta_q0_15;     // Output gain - sign * 1.0, signed (|delta| <= 1.0)
    int32_t            out_offset_raw_fx3;  // Integer part of the offset in raw_fx3
    int32_t            out_offset_frac;     // Fraction below raw_fx3 (15 bits) incl. rounding
    int32_t            vref_raw_fx3;        // Common reference (Vref), 0 V - VDD
    uint8_t            current_mode;
    MimicOutputRoute_t output_route;
    uint8_t            global_flags;
} MimicPipeline_t;

static MimicPipeline_t pipeline;

#define DELAY_BUFFER_SIZE 4096
#define DELAY_BUFFER_MASK (DELAY_BUFFER_SIZE - 1)
static uint16_t delay_buffer_raw_fx3[DELAY_BUFFER_SIZE] = {0};   // 0 - 32760 fits uint16


// =========================================================
// Complete DSP Mode-Specific Contexts (Unified Structs)
// =========================================================
// Every mode takes and returns raw_fx3. Modes with a signal center work around the
// common Vref (raw_vref_fx3); the others work on raw_fx3 directly.

// Positive coefficient c applied to a state with F fractional bits below raw_fx3:
// c * v * 2^F = ((v * mant) << ls + half) >> rs (one of ls / rs is 0, see SetCoef12)
typedef struct {
    int32_t mant;            // 0 or 2048 - 4095 (0: c = 0)
    int32_t ls;
    int32_t rs;
    int32_t half;
} Coef12_t;

typedef struct {
    uint8_t type;            // Config: MIMIC_FILTER_1ST_*
    int32_t g_mant;          // Config: G = g / (1 + g), G * 2^12 = g_mant >> g_shift
    int32_t g_shift;         // Config
    int32_t g_half;          // Config: rounding for g_shift
    int32_t s;               // State: TPT integrator, raw_vref_fx3 << SVF_STATE_TO_FX3
} Filter1st_t;

typedef struct {
    int32_t g_mant;          // Config: g = tan(pi fc / fs) = g_mant >> g_shift (raw_vref_fx3 -> state units)
    int32_t g_half;          // Config: rounding for g_shift
    int32_t e_mant;          // Config: e = 1 - 1 / (1 + g k + g^2) = e_mant >> e_shift
    int32_t e_half;          // Config: rounding for e_shift
    int32_t g1;              // Config: k + g, SVF_G1_FRAC_BITS
    int32_t k;               // Config: 1 / Q, SVF_K_FRAC_BITS
    uint32_t g_shift;        // Config
    uint32_t e_shift;        // Config
    uint8_t type;            // Config: MIMIC_FILTER_BIQUAD_*
    int32_t s1, s2;          // State: integrators, raw_vref << SVF_STATE_FRAC_BITS
} Svf_t;

typedef struct {
    int32_t gain_mant_q0_15; // Config: gain = mant * 2^exp
    int32_t shift;           // Config: 15 - exp (0 - 15), so that y = (d * mant) >> shift
    int32_t half;            // Config: rounding for shift
} Pga_t;

typedef struct {
    uint8_t type;            // Config: MIMIC_COMPARATOR_*
    int32_t upper_raw_fx3;   // Config: HIGH when x > upper
    int32_t lower_raw_fx3;   // Config: LOW when x < lower
    int32_t wave_last_raw_fx3; // State
} Comparator_t;

typedef struct {
    uint8_t type;             // Config: MIMIC_MATH_*
    int32_t scale_mant_q0_15; // Config: scale = mant * 2^exp
    int32_t shift;            // Config: derivative 15 - exp (0 - 15), integral input shift
    int32_t half;             // Config: derivative, rounding for shift
    Coef12_t leak_c;          // Config: integral, 1 - leak (0 = no leak)
    int32_t last_raw_vref_fx3;// State: derivative, previous input
    int32_t state;            // State: integral, raw_vref_fx3 << INTEG_FRAC_BITS
} Math_t;

typedef struct {
    int32_t max_step_raw_fx3; // Config: max change per sample
    int32_t last_raw_fx3;     // State
} Slew_t;

typedef struct {
    uint8_t type;            // Config: MIMIC_CLIPPER_*
    int32_t upper_raw_fx3;   // Config: upper limit (comparison and output level)
    int32_t lower_raw_fx3;   // Config: lower limit (comparison and output level)
    int32_t center_raw_fx3;  // Config: soft, (upper + lower) / 2
    int32_t lim_raw_fx3;     // Config: soft, 1.5 x (upper - center): the curve reaches the limit
    int32_t d2_shift;        // Config: soft, d^2 >> d2_shift < 2^16
    int32_t m;               // Config: soft, 4 / (27 h^2) scaled: r = ((d^2 >> shift) * m) >> 15
} Clipper_t;

typedef struct {
    Coef12_t decay_c;         // Config: 1 - decay (0 = peak hold)
    uint8_t polarity;         // Config
    int32_t state;            // State: magnitude, raw_vref_fx3 << ENV_FRAC_BITS
} Envelope_t;

typedef struct {
    uint8_t type;            // Config: MIMIC_RECT_FULL / MIMIC_RECT_HALF
    uint8_t polarity;        // Config: MIMIC_POLARITY_*
} Rectifier_t;

typedef struct {
    uint32_t period_samples;  // Config
    uint32_t track_samples;   // Config
    uint32_t counter;         // State
    int32_t  last_raw_fx3;    // State
} SampleHold_t;

typedef struct {
    uint32_t delay_samples;   // Config
    uint32_t delay_write_ptr; // State
} Delay_t;

typedef struct {
    uint8_t type;             // Config: MIMIC_LOG_TYPE_*
    int32_t seg[65];          // LOG: slope * log2(1 + i/64), ANTILOG: unit * 2^(i/64) (raw << 7)
    int32_t oct[LOG_OCT_COUNT]; // LOG: Vref + slope * (e - log2(unit)) for e = 0 - 14 (raw << 7),
                                //      x and unit in raw_fx3
    int32_t d_lim;            // ANTILOG: clamp of (x - Vref), raw_fx3
    int32_t inv_k_q20;        // ANTILOG: octaves per raw_fx3 unit (Q20)
} Log_t;

typedef union {
    Filter1st_t  filter1st;
    Svf_t        svf;
    Pga_t        pga;
    Comparator_t comp;
    Math_t       math;
    Slew_t       slew;
    Clipper_t    clipper;
    Envelope_t   envelope;
    Rectifier_t  rect;
    SampleHold_t sh;
    Delay_t      delay;
    Log_t        log;
} DSPContext_u;

static DSPContext_u dsp_ctx;

// =========================================================================
// Decimation & Interpolation State Variables (Promoted to native 32-bit)
// =========================================================================
static uint32_t decimation_mask     = 0;
static uint32_t decimation_shift    = 0;
static uint32_t adc_accumulator     = 0;
static uint32_t decimation_count    = 0;
static uint32_t decimation_fx3_lshift = RAW_FX3_FRAC_BITS;   // see MimicDSP_SetDecimation()
static uint32_t decimation_fx3_rshift = 0;
static uint32_t decimation_fx3_round  = 0;

static int32_t  y_old_raw           = MIMIC_ADC_MID_VALUE;
static int32_t  y_new_raw           = MIMIC_ADC_MID_VALUE;
static int32_t  delta_step_raw_fx16 = 0;
static int32_t  out_raw_fx16        = (MIMIC_ADC_MID_VALUE << FX16_FRAC_BITS);

// =========================================================
// Utilities & I2C Parameter Parsers (32-bit Safe)
// =========================================================

// --- raw_vref_fx3: relative to the common Vref ---
static inline __attribute__((always_inline)) int32_t ToVrefFx3(int32_t x_raw_fx3) {
    return x_raw_fx3 - pipeline.vref_raw_fx3;
}

static inline __attribute__((always_inline)) int32_t FromVrefFx3(int32_t v_raw_vref_fx3) {
    return v_raw_vref_fx3 + pipeline.vref_raw_fx3;
}

static inline __attribute__((always_inline)) int32_t ClampI32(int32_t v, int32_t lim) {
    if (v >  lim) return  lim;
    if (v < -lim) return -lim;
    return v;
}

static inline __attribute__((always_inline)) uint32_t MuxDacGpio(uint32_t result) {
    return (pipeline.output_route == MIMIC_OUT_DIGITAL)
         ? ((uint32_t)OPA2_INN_PIN << (result >= MIMIC_ADC_MID_VALUE ? 0u : 16u))
         : result;
}

// =========================================================
// 2. DSP Core Functions (raw_fx3 in, raw_fx3 out; out-of-range values saturate in the output stage)
// =========================================================

// 1st-order filter (TPT one-pole, bilinear): y = Vref + H(x - Vref)
//   v = G (x - s),  lp = v + s,  s = lp + v,   G = g / (1 + g), g = tan(pi fc / fs)
//   LPF = lp, HPF = x - lp, APF = lp - hp = 2 lp - x
// Like an RC filter returning to Vref: a step of Vref settles with the time constant.
// |x - s| <= 2^16 and g_mant < 2^12: the product stays within 2^28.
static inline __attribute__((always_inline)) int32_t ProcessFilter1st(int32_t x_raw_fx3) {
    Filter1st_t *p = &dsp_ctx.filter1st;

    int32_t x  = ToVrefFx3(x_raw_fx3);
    int32_t s  = p->s;
    int32_t d  = x - ((s + (1 << (SVF_STATE_TO_FX3 - 1))) >> SVF_STATE_TO_FX3);
    int32_t v  = (p->g_mant * d + p->g_half) >> p->g_shift;
    int32_t lp = v + s;
    p->s = lp + v;

    int32_t y;
    if (p->type == MIMIC_FILTER_1ST_LPF) {
        y = (lp + (1 << (SVF_STATE_TO_FX3 - 1))) >> SVF_STATE_TO_FX3;
    } else if (p->type == MIMIC_FILTER_1ST_HPF) {
        y = x - ((lp + (1 << (SVF_STATE_TO_FX3 - 1))) >> SVF_STATE_TO_FX3);
    } else {
        y = ((lp + (1 << (SVF_STATE_TO_FX3 - 2))) >> (SVF_STATE_TO_FX3 - 1)) - x;   // 2 lp - x
    }
    return FromVrefFx3(y);
}

// Biquad mode (TPT state-variable filter): y = Vref + H(x - Vref)
//   v  = x - g1 * s1 - s2,  hp = v / (1 + g k + g^2) = v - e * v
//   bp = g * hp + s1,  s1 = bp + g * hp
//   lp = g * bp + s2,  s2 = lp + g * bp
//   LPF = lp, HPF = hp, BPF = k * bp (0 dB peak), NOTCH = x - k * bp
// Every product is (integer signal, <= 2^19) x (coefficient, <= 2^12), within int32.
static inline __attribute__((always_inline)) int32_t ProcessBiquadFilter(int32_t x_raw_fx3) {
    Svf_t *p = &dsp_ctx.svf;
    int32_t x  = ToVrefFx3(x_raw_fx3);
    int32_t s1 = p->s1;
    int32_t s2 = p->s2;

    int32_t v   = x - ((p->g1 * (s1 >> SVF_STATE_TO_FX3)) >> SVF_G1_FRAC_BITS) - (s2 >> SVF_STATE_TO_FX3);
    int32_t hp  = v - ((p->e_mant * v + p->e_half) >> p->e_shift);
    int32_t ghp = (p->g_mant * hp + p->g_half) >> p->g_shift;
    int32_t bp  = ghp + s1;
    p->s1 = bp + ghp;

    int32_t bp_fx3 = bp >> SVF_STATE_TO_FX3;
    int32_t gbp = (p->g_mant * bp_fx3 + p->g_half) >> p->g_shift;
    int32_t lp  = gbp + s2;
    p->s2 = lp + gbp;

    int32_t y;
    switch (p->type) {
        case MIMIC_FILTER_BIQUAD_LPF: y = (lp + (1 << (SVF_STATE_TO_FX3 - 1))) >> SVF_STATE_TO_FX3; break;
        case MIMIC_FILTER_BIQUAD_HPF: y = hp; break;
        case MIMIC_FILTER_BIQUAD_BPF: y = (p->k * bp_fx3 + (1 << (SVF_K_FRAC_BITS - 1))) >> SVF_K_FRAC_BITS; break;
        case MIMIC_FILTER_BIQUAD_NOTCH: y = x - ((p->k * bp_fx3 + (1 << (SVF_K_FRAC_BITS - 1))) >> SVF_K_FRAC_BITS); break;
        case MIMIC_FILTER_BIQUAD_APF:   y = x - ((p->k * bp_fx3 + (1 << (SVF_K_FRAC_BITS - 2))) >> (SVF_K_FRAC_BITS - 1)); break;
        default: y = x; break;   // UNITY: same work, input passed through (profiling)
    }
    return FromVrefFx3(y);   // high-Q overshoot is bounded by the output-stage guard
}

// PGA: y = Vref + G * (x - Vref), G = mant * 2^exp, rounded once.
// |d| <= 32767 and |mant| <= 32768: |d * mant| <= 2^30, within int32.
static inline __attribute__((always_inline)) int32_t ProcessPga(int32_t x_raw_fx3) {
    Pga_t *p = &dsp_ctx.pga;

    int32_t in_raw_vref_fx3 = ToVrefFx3(x_raw_fx3);
    return FromVrefFx3((in_raw_vref_fx3 * p->gain_mant_q0_15 + p->half) >> p->shift);
}

// Comparator: window (HIGH outside [lower, upper]) or Schmitt trigger (hysteresis)
static inline __attribute__((always_inline)) int32_t ProcessComparator(int32_t x_raw_fx3) {
    Comparator_t *p = &dsp_ctx.comp;

    if (p->type == MIMIC_COMPARATOR_WINDOW) {
        if (x_raw_fx3 > p->upper_raw_fx3 || x_raw_fx3 < p->lower_raw_fx3) return RAW_MAX_FX3;
        return 0;
    }
    if (x_raw_fx3 > p->upper_raw_fx3) p->wave_last_raw_fx3 = RAW_MAX_FX3;
    else if (x_raw_fx3 < p->lower_raw_fx3) p->wave_last_raw_fx3 = 0;
    return p->wave_last_raw_fx3;
}

// Envelope: tracks the swing from Vref and decays toward Vref
static inline __attribute__((always_inline)) int32_t ProcessEnvelopeFollower(int32_t x_raw_fx3) {
    Envelope_t *p = &dsp_ctx.envelope;

    int32_t mag = ToVrefFx3(x_raw_fx3);
    if (p->polarity == MIMIC_POLARITY_NEGATIVE) mag = -mag;
    if (mag < 0) mag = 0;                         // 0 - 32767: mag << 15 < 2^30

    int32_t e = p->state >> ENV_FRAC_BITS;
    if (mag > e) {
        p->state = mag << ENV_FRAC_BITS;
    } else {
        // state *= decay as state -= (1 - decay) * state: e < 2^15, mant < 2^12, ls <= 3
        p->state -= (((e * p->decay_c.mant) << p->decay_c.ls) + p->decay_c.half) >> p->decay_c.rs;
    }
    // magnitude above Vref (same for both polarities)
    return FromVrefFx3((p->state + (1 << (ENV_FRAC_BITS - 1))) >> ENV_FRAC_BITS);
}

// Clipper. Hard: passes x between the limits. Soft (cubic, d = x - center, h = upper - center):
//   y = center + d - 4 d^3 / (27 h^2) for |d| < 1.5 h (gain 1 at the center, slope 0 at the
//   limits), the limit beyond. |d| < 1.5 h <= 24576: d^2 < 2^30, products < 2^31.
static inline __attribute__((always_inline)) int32_t ProcessClipper(int32_t x_raw_fx3) {
    Clipper_t *p = &dsp_ctx.clipper;

    if (p->type == MIMIC_CLIPPER_HARD) {
        if (x_raw_fx3 > p->upper_raw_fx3) return p->upper_raw_fx3;
        if (x_raw_fx3 < p->lower_raw_fx3) return p->lower_raw_fx3;
        return x_raw_fx3;
    }

    int32_t d = x_raw_fx3 - p->center_raw_fx3;
    if (d >=  p->lim_raw_fx3) return p->upper_raw_fx3;
    if (d <= -p->lim_raw_fx3) return p->lower_raw_fx3;
    int32_t r = (((d * d) >> p->d2_shift) * p->m) >> Q15_FRAC_BITS;   // 4 d^2 / (27 h^2), q15
    return p->center_raw_fx3 + d - ((d * r) >> Q15_FRAC_BITS);
}

// LOG:     y = Vref + slope * log10(x / unit)      (x: magnitude from 0 V)
// ANTILOG: y = unit * 10^((x - Vref) / slope)     (y: magnitude from 0 V)
// The tables are in raw << LOG_OUT_FRAC_BITS; the result is rounded to raw_fx3.
static inline __attribute__((always_inline)) int32_t ProcessLog(int32_t x_raw_fx3) {
    Log_t *p = &dsp_ctx.log;

    if (p->type == MIMIC_LOG_TYPE_LOG) {
        uint32_t x = (x_raw_fx3 < 1) ? 1u : (uint32_t)x_raw_fx3;

        // e = floor(log2(x)), x = 1 - 32767 (binary search: Cortex-M0+ has no CLZ)
        uint32_t e = 0;
        if (x >> 8)       e = 8;
        if (x >> (e + 4)) e += 4;
        if (x >> (e + 2)) e += 2;
        if (x >> (e + 1)) e += 1;

        uint32_t f    = (x << (LOG_F_BITS - e)) - (1u << LOG_F_BITS);   // mantissa fraction
        uint32_t i    = f >> (LOG_F_BITS - LOG_SEG_BITS);
        int32_t  frac = (int32_t)(f & ((1u << (LOG_F_BITS - LOG_SEG_BITS)) - 1));
        int32_t  y    = p->oct[e] + p->seg[i]
                      + (((p->seg[i + 1] - p->seg[i]) * frac) >> (LOG_F_BITS - LOG_SEG_BITS));
        return (y + LOG_TO_FX3_ROUND) >> LOG_TO_FX3_SHIFT;
    }

    // ANTILOG
    int32_t d = ClampI32(ToVrefFx3(x_raw_fx3), p->d_lim);
    int32_t t = d * p->inv_k_q20;                           // octaves (Q20), |t| <= 13 + margin
    int32_t n = t >> ANTILOG_T_FRAC_BITS;                   // floor
    uint32_t f = (uint32_t)(t - (n << ANTILOG_T_FRAC_BITS));
    uint32_t i = f >> (ANTILOG_T_FRAC_BITS - LOG_SEG_BITS);
    int32_t frac = (int32_t)((f >> (ANTILOG_T_FRAC_BITS - LOG_SEG_BITS - 5)) & 31);
    int32_t m = p->seg[i] + (((p->seg[i + 1] - p->seg[i]) * frac) >> 5);
    if (n >= 0) {
        int32_t lim = ANTILOG_LIM >> n;
        if (m > lim) m = lim;
        m <<= n;
    } else {
        m >>= -n;
    }
    return (m + LOG_TO_FX3_ROUND) >> LOG_TO_FX3_SHIFT;
}

// Math: derivative y = Vref + k * dx/dt, or leaky integrator leaking toward Vref
static inline __attribute__((always_inline)) int32_t ProcessMath(int32_t x_raw_fx3) {
    Math_t *p = &dsp_ctx.math;

    int32_t in_raw_vref_fx3 = ToVrefFx3(x_raw_fx3);
    int32_t s = p->shift;

    if (p->type == MIMIC_MATH_DERIVATIVE) {
        // y = Vref + scale * (x[n] - x[n-1]), rounded once.
        // |diff| <= 65534 and |mant| <= 32768: |diff * mant| + half < 2^31.
        int32_t diff = in_raw_vref_fx3 - p->last_raw_vref_fx3;
        p->last_raw_vref_fx3 = in_raw_vref_fx3;

        int32_t out_raw_vref_fx3 = (diff * p->scale_mant_q0_15 + p->half) >> p->shift;
        return FromVrefFx3(ClampI32(out_raw_vref_fx3, OUT_GUARD_RAW_FX3));
    }

    // Leaky integrator: y = leak * y + 2^exp * d, as y += 2^exp * d - (1 - leak) * y.
    // The state keeps INTEG_FRAC_BITS below raw_fx3: |state| < 2^30, |d << s| < 2^30,
    // |y| < 2^16 and mant < 2^12 with ls <= 2: the leak term < 2^30.
    int32_t in_term = (s >= 0) ? (in_raw_vref_fx3 << s) : (in_raw_vref_fx3 >> -s);
    int32_t y = (p->state + (1 << (INTEG_FRAC_BITS - 1))) >> INTEG_FRAC_BITS;
    int32_t state = p->state + in_term
                  - ((((y * p->leak_c.mant) << p->leak_c.ls) + p->leak_c.half) >> p->leak_c.rs);
    state = ClampI32(state, INTEG_STATE_LIM);
    p->state = state;
    return FromVrefFx3((state + (1 << (INTEG_FRAC_BITS - 1))) >> INTEG_FRAC_BITS);
}

// Slew-rate limiter: the output moves toward x by at most max_step per sample
static inline __attribute__((always_inline)) int32_t ProcessSlewRateLimiter(int32_t x_raw_fx3) {
    Slew_t *p = &dsp_ctx.slew;

    int32_t diff = x_raw_fx3 - p->last_raw_fx3;
    if (diff >  p->max_step_raw_fx3) diff =  p->max_step_raw_fx3;
    if (diff < -p->max_step_raw_fx3) diff = -p->max_step_raw_fx3;

    p->last_raw_fx3 += diff;
    return p->last_raw_fx3;
}

// Rectifier: folds at Vref (d = x - Vref)
//   full : positive Vref + |d|,        negative Vref - |d|
//   half : positive Vref + max(d, 0),  negative Vref + min(d, 0)
static inline __attribute__((always_inline)) int32_t ProcessRectifier(int32_t x_raw_fx3) {
    Rectifier_t *p = &dsp_ctx.rect;

    int32_t d = ToVrefFx3(x_raw_fx3);
    if (p->polarity == MIMIC_POLARITY_NEGATIVE) d = -d;   // work on the positive side
    if (d < 0) d = (p->type == MIMIC_RECT_FULL) ? -d : 0;
    if (p->polarity == MIMIC_POLARITY_NEGATIVE) d = -d;
    return FromVrefFx3(d);
}

static inline __attribute__((always_inline)) int32_t ProcessSampleAndHold(int32_t x_raw_fx3) {
    SampleHold_t *p = &dsp_ctx.sh;

    p->counter++;
    if (p->counter >= p->period_samples) p->counter = 0;
    if (p->counter < p->track_samples) p->last_raw_fx3 = x_raw_fx3;
    return p->last_raw_fx3;
}

static inline __attribute__((always_inline)) int32_t ProcessDelay(int32_t x_raw_fx3) {
    Delay_t *p = &dsp_ctx.delay;

    delay_buffer_raw_fx3[p->delay_write_ptr] = (uint16_t)x_raw_fx3;
    uint32_t read_ptr = (p->delay_write_ptr - p->delay_samples) & DELAY_BUFFER_MASK;
    int32_t out_raw_fx3 = delay_buffer_raw_fx3[read_ptr];

    p->delay_write_ptr = (p->delay_write_ptr + 1) & DELAY_BUFFER_MASK;
    return out_raw_fx3;
}

// =========================================================
// Initialization and Parameter Updates
// =========================================================

// Output stage: final = round((dsp_out * mult + offset) / 2^18), dsp_out in raw_fx3,
//   normal  : mult = +gain, offset = offset
//   inverted: mult = -gain, offset = 4095 - offset
// mult is split as sign * 1.0 + delta and offset as offset_raw_fx3 * 2^15 + offset_frac,
// so that the per-sample products stay within int32 (see MimicDSP_ProcessSample).
static void SetOutputStage(int32_t gain_uq1_15, int32_t offset_raw_fx3, bool inverted) {
    int32_t sign = inverted ? -1 : 1;
    int64_t offset_fx18 = ((int64_t)offset_raw_fx3 << Q15_FRAC_BITS) + OUT_ROUND_RAW_FX18;
    if (inverted) {
        offset_fx18 = ((int64_t)MIMIC_ADC_MAX_VALUE << OUT_FRAC_BITS)
                    - ((int64_t)offset_raw_fx3 << Q15_FRAC_BITS) + OUT_ROUND_RAW_FX18;
    }
    pipeline.out_sign           = sign;
    pipeline.out_delta_q0_15    = sign * (gain_uq1_15 - (1 << Q15_FRAC_BITS));   // -32767 - 32768
    pipeline.out_offset_raw_fx3 = (int32_t)(offset_fx18 >> Q15_FRAC_BITS);       // floor
    pipeline.out_offset_frac    = (int32_t)(offset_fx18 & ((1 << Q15_FRAC_BITS) - 1));
}

void MimicDSP_Init(void) {
    pipeline.current_mode        = MIMIC_MODE_ID_BYPASS_DSP;
    pipeline.output_route        = MIMIC_OUT_OPEN;
    pipeline.global_flags        = 0;
    pipeline.vref_raw_fx3        = MIMIC_VREF_DEFAULT_RAW_FX3;
    SetOutputStage(1 << Q15_FRAC_BITS, 0, false);   // gain 1.0, no offset

    memset((void*)&dsp_ctx, 0, sizeof(dsp_ctx));

    mimic_output_slot = MIMIC_ADC_MID_VALUE;
}

volatile uint32_t *MimicDSP_GetOutputSlot(void) {
    return &mimic_output_slot;
}

static inline __attribute__((always_inline)) uint16_t ParseU16(const uint8_t *p) {
    return _MimicDevice_ReadU16_BE(p);
}

static inline __attribute__((always_inline)) int16_t ParseS16(const uint8_t *p) {
    return _MimicDevice_ReadS16_BE(p);
}

// log2(v) in Q15 for v >= 1 (configuration time only)
static int32_t Log2Q15(uint32_t v) {
    if (v < 1) v = 1;
    int32_t e = 0;
    while ((v >> (e + 1)) != 0) e++;
    uint32_t m = (e <= 15) ? (v << (15 - e)) : (v >> (e - 15));   // [2^15, 2^16)
    uint32_t f = m - 32768u;                                       // 15 bits
    uint32_t i = f >> (15 - LOG_SEG_BITS);
    int32_t frac = (int32_t)(f & ((1u << (15 - LOG_SEG_BITS)) - 1));
    int32_t l = log2_1pf_q15[i]
              + (((log2_1pf_q15[i + 1] - log2_1pf_q15[i]) * frac) >> (15 - LOG_SEG_BITS));
    return (e << 15) + l;
}

static void ConfigureLog(const uint8_t *p) {
    Log_t *c = &dsp_ctx.log;
    int32_t slope = ParseS16(p + 1);     // raw_fx3 per decade
    int32_t unit  = ParseS16(p + 3);     // raw_fx3
    if (slope == 0) slope = 1;
    if (unit < 1) unit = 1;

    c->type = *(p + 0);
    int32_t k_q16 = slope * LOG10_2_Q16;  // raw_fx3 per octave, Q16 (signed)

    if (c->type == MIMIC_LOG_TYPE_LOG) {
        // seg[i] = K * log2(1 + i/64), oct[e] = Vref + K * (e - log2(unit)), in raw << 7
        // (x and unit both in raw_fx3, so their ratio needs no scaling)
        for (int i = 0; i <= (1 << LOG_SEG_BITS); i++) {
            c->seg[i] = (int32_t)(((int64_t)k_q16 * log2_1pf_q15[i]) >> 27);
        }
        int32_t log2_unit_q15 = Log2Q15((uint32_t)unit);
        int32_t vref_q = pipeline.vref_raw_fx3 << LOG_TO_FX3_SHIFT;
        for (int e = 0; e < LOG_OCT_COUNT; e++) {
            c->oct[e] = vref_q + (int32_t)(((int64_t)k_q16 * ((e << 15) - log2_unit_q15)) >> 27);
        }
    } else {
        // seg[i] = unit * 2^(i/64) in raw << 7; t = (x - Vref) / K in octaves
        for (int i = 0; i <= (1 << LOG_SEG_BITS); i++) {
            c->seg[i] = (int32_t)(((int64_t)unit * exp2_f_q15[i]) >> 11);
        }
        int32_t k_abs = (k_q16 < 0) ? -k_q16 : k_q16;
        c->d_lim     = (int32_t)(((int64_t)ANTILOG_MAX_OCT * k_abs) >> 16) + 1;
        c->inv_k_q20 = (int32_t)(((int64_t)1 << 36) / k_q16);
    }
}

// Coefficient c = mant / 2^sh (mant12 / sh), applied to a state with frac_bits below raw_fx3
static void SetCoef12(Coef12_t *c, uint16_t mant, uint8_t sh, int32_t frac_bits) {
    if (mant > 4095) mant = 4095;
    if (sh < 12) sh = 12;                      // c < 1
    if (sh > COEF12_SHIFT_MAX) sh = COEF12_SHIFT_MAX;
    c->mant = mant;
    c->ls   = (frac_bits > sh) ? frac_bits - sh : 0;
    c->rs   = (sh > frac_bits) ? sh - frac_bits : 0;
    c->half = (1 << c->rs) >> 1;
}

// Soft clipper: center, reach (1.5 h) and the d^2 scaling for 4 / (27 h^2)
static void ConfigureClipper(const uint8_t *p) {
    Clipper_t *c = &dsp_ctx.clipper;
    int32_t upper = ParseS16(p + 1);
    int32_t lower = ParseS16(p + 3);
    c->type          = p[0];
    c->upper_raw_fx3 = upper;
    c->lower_raw_fx3 = lower;
    if (c->type != MIMIC_CLIPPER_SOFT) return;

    // Limits within 0 V - VDD: h <= 16383, so |d| < 1.5 h keeps d^2 < 2^30
    if (upper > MIMIC_RAW_FX3_MAX) upper = MIMIC_RAW_FX3_MAX;
    if (lower < 0) lower = 0;
    if (upper < lower) upper = lower;
    c->upper_raw_fx3  = upper;
    c->lower_raw_fx3  = lower;
    c->center_raw_fx3 = (upper + lower) >> 1;
    int32_t h = upper - c->center_raw_fx3;
    c->lim_raw_fx3 = h + (h >> 1);
    if (h == 0) { c->d2_shift = 0; c->m = 0; return; }

    // hh = h^2 >> shift in [2^14, 2^15) (or h^2 when smaller): m = 4 * 2^30 / (27 hh)
    uint32_t h2 = (uint32_t)(h * h);
    int32_t shift = 0;
    while ((h2 >> shift) >= (1u << 15)) shift++;
    uint32_t hh = h2 >> shift;
    c->d2_shift = shift;
    c->m = (int32_t)((((uint64_t)4 << 30) + 27u * hh / 2) / (27u * hh));
}

// mant * 2^exp as (v * mant + half) >> shift, exp limited to 0 - 15 (gain < 2^15)
static void MantExpToShift(int32_t exp, int32_t *shift, int32_t *half) {
    if (exp < 0) exp = 0;      // the mantissa already covers |gain| < 1
    if (exp > Q15_FRAC_BITS) exp = Q15_FRAC_BITS;
    *shift = Q15_FRAC_BITS - exp;
    *half  = (1 << *shift) >> 1;
}

void MimicDSP_UpdateParameters(const MimicDSP_Config_t *config) {
    uint8_t previous_mode = pipeline.current_mode;

    pipeline.global_flags = config->global_flags;
    pipeline.current_mode = config->mode_id;
    pipeline.output_route = config->output_route;

    // 1. Common Vref (an input: changing it does not reset any state)
    int32_t vref = config->vref_raw_fx3;
    if (vref < 0) vref = 0;
    if (vref > MIMIC_RAW_FX3_MAX) vref = MIMIC_RAW_FX3_MAX;
    pipeline.vref_raw_fx3 = vref;

    // 2. Global Decimation & Offset Parameters
    MimicDSP_SetDecimation(config->decimation_n);
    SetOutputStage(config->gain_uq1_15, config->offset_raw_fx3,
                   (pipeline.global_flags & MIMIC_FLAG_INV_OUT) != 0);

    // 3. Reset mode-specific state history upon mode transition (outputs start at mid-scale)
    if (pipeline.current_mode != previous_mode) {
        memset((void*)&dsp_ctx, 0, sizeof(dsp_ctx));

        switch (pipeline.current_mode) {
            case MIMIC_MODE_ID_COMPARATOR:        dsp_ctx.comp.wave_last_raw_fx3 = RAW_MID_FX3; break;
            case MIMIC_MODE_ID_SLEW_RATE_LIMITER: dsp_ctx.slew.last_raw_fx3      = RAW_MID_FX3; break;
            case MIMIC_MODE_ID_SAMPLE_AND_HOLD:   dsp_ctx.sh.last_raw_fx3        = RAW_MID_FX3; break;
            case MIMIC_MODE_ID_DELAY:
                for (uint32_t i = 0; i < DELAY_BUFFER_SIZE; i++) {
                    delay_buffer_raw_fx3[i] = RAW_MID_FX3;
                }
                break;
            default: break;
        }
    }

    // 4. Parse Mode-Specific Payload
    const uint8_t *p = config->payload;
    switch (pipeline.current_mode) {
        case MIMIC_MODE_ID_FILTER_BIQUAD: {
            // States are kept: changing fc / Q / type while running is smooth
            Svf_t *s = &dsp_ctx.svf;
            uint32_t g_shift = p[3];
            uint32_t e_shift = p[8];
            if (g_shift > SVF_SHIFT_MAX) g_shift = SVF_SHIFT_MAX;
            if (e_shift > SVF_SHIFT_MAX) e_shift = SVF_SHIFT_MAX;
            s->type    = p[0];
            s->g_mant  = ParseU16(p + 1);
            s->g_shift = g_shift;
            s->g_half  = (1 << g_shift) >> 1;
            s->g1      = ParseU16(p + 4);
            s->e_mant  = ParseU16(p + 6);
            s->e_shift = e_shift;
            s->e_half  = (1 << e_shift) >> 1;
            s->k       = ParseU16(p + 9);
            break;
        }

        case MIMIC_MODE_ID_FILTER_1ST: {
            // State is kept: changing fc / type while running is smooth
            uint32_t g_shift = p[3];
            if (g_shift > SVF_SHIFT_MAX) g_shift = SVF_SHIFT_MAX;
            dsp_ctx.filter1st.type    = p[0];
            dsp_ctx.filter1st.g_mant  = ParseU16(p + 1) & 0x0FFF;
            dsp_ctx.filter1st.g_shift = (int32_t)g_shift;
            dsp_ctx.filter1st.g_half  = (1 << g_shift) >> 1;
            break;
        }

        case MIMIC_MODE_ID_PGA: {
            dsp_ctx.pga.gain_mant_q0_15 = ParseS16(p + 0);
            MantExpToShift((int8_t)*(p + 2), &dsp_ctx.pga.shift, &dsp_ctx.pga.half);
            break;
        }

        case MIMIC_MODE_ID_COMPARATOR:
            dsp_ctx.comp.type          = *(p + 0);
            dsp_ctx.comp.upper_raw_fx3 = ParseS16(p + 1);
            dsp_ctx.comp.lower_raw_fx3 = ParseS16(p + 3);
            break;

        case MIMIC_MODE_ID_MATH: {
            int32_t exp = (int8_t)*(p + 3);
            dsp_ctx.math.type = *(p + 0);
            dsp_ctx.math.scale_mant_q0_15 = ParseS16(p + 1);
            if (dsp_ctx.math.type == MIMIC_MATH_INTEGRAL) {
                // Input shift exp <= 1, applied to the state with INTEG_FRAC_BITS
                if (exp > 1) exp = 1;
                if (exp < -(INTEG_FRAC_BITS + 16)) exp = -(INTEG_FRAC_BITS + 16);
                dsp_ctx.math.shift        = INTEG_FRAC_BITS + exp;
                dsp_ctx.math.half         = 0;
                SetCoef12(&dsp_ctx.math.leak_c, ParseU16(p + 1), p[4], INTEG_FRAC_BITS);
            } else {
                MantExpToShift(exp, &dsp_ctx.math.shift, &dsp_ctx.math.half);
            }
            break;
        }

        case MIMIC_MODE_ID_SLEW_RATE_LIMITER: {
            int32_t step = ParseS16(p + 0);
            if (step < 0) step = 0;
            dsp_ctx.slew.max_step_raw_fx3 = step;
            break;
        }

        case MIMIC_MODE_ID_CLIPPER:
            ConfigureClipper(p);
            break;

        case MIMIC_MODE_ID_LOG:
            ConfigureLog(p);    // depends on Vref: rebuilt on every update
            break;

        case MIMIC_MODE_ID_ENVELOPE_FOLLOWER:
            SetCoef12(&dsp_ctx.envelope.decay_c, ParseU16(p + 0), p[3], ENV_FRAC_BITS);
            dsp_ctx.envelope.polarity    = (uint8_t)*(p + 2);
            break;

        case MIMIC_MODE_ID_RECTIFIER:
            dsp_ctx.rect.type     = (uint8_t)*(p + 0);
            dsp_ctx.rect.polarity = (uint8_t)*(p + 1);
            break;

        case MIMIC_MODE_ID_SAMPLE_AND_HOLD:
            dsp_ctx.sh.period_samples = ParseU16(p + 0);
            dsp_ctx.sh.track_samples  = ParseU16(p + 2);
            break;

        case MIMIC_MODE_ID_DELAY:
            dsp_ctx.delay.delay_samples = ParseU16(p + 0);
            if (dsp_ctx.delay.delay_samples > DELAY_BUFFER_MASK) {
                dsp_ctx.delay.delay_samples = DELAY_BUFFER_MASK;
            }
            break;
    }
}

// =========================================================
// 3. The Main DSP Pipeline 
// =========================================================
// x_raw_fx3: DSP input in raw_fx3 (0 - 32760). Returns the DAC code (0 - 4095).
__attribute__((always_inline)) static inline uint16_t MimicDSP_ProcessSample(int32_t x_raw_fx3) {
    MimicPipeline_t *pipe = &pipeline;
    int32_t dsp_out = 0;

    if (pipe->current_mode == MIMIC_MODE_ID_FILTER_BIQUAD) {
        dsp_out = ProcessBiquadFilter(x_raw_fx3);
    } else {
        switch (pipe->current_mode) {
            case MIMIC_MODE_ID_PGA:                 dsp_out = ProcessPga(x_raw_fx3); break;
            case MIMIC_MODE_ID_FILTER_1ST:          dsp_out = ProcessFilter1st(x_raw_fx3); break;
            case MIMIC_MODE_ID_COMPARATOR:          dsp_out = ProcessComparator(x_raw_fx3); break;
            case MIMIC_MODE_ID_ENVELOPE_FOLLOWER:   dsp_out = ProcessEnvelopeFollower(x_raw_fx3); break;
            case MIMIC_MODE_ID_CLIPPER:             dsp_out = ProcessClipper(x_raw_fx3); break;
            case MIMIC_MODE_ID_LOG:                 dsp_out = ProcessLog(x_raw_fx3); break;
            case MIMIC_MODE_ID_MATH:                dsp_out = ProcessMath(x_raw_fx3); break;
            case MIMIC_MODE_ID_RECTIFIER:           dsp_out = ProcessRectifier(x_raw_fx3); break;
            case MIMIC_MODE_ID_SLEW_RATE_LIMITER:   dsp_out = ProcessSlewRateLimiter(x_raw_fx3); break;
            case MIMIC_MODE_ID_SAMPLE_AND_HOLD:     dsp_out = ProcessSampleAndHold(x_raw_fx3); break;
            case MIMIC_MODE_ID_DELAY:               dsp_out = ProcessDelay(x_raw_fx3); break;
            default:                                dsp_out = x_raw_fx3; break;
        }
    }

    // Post-Processing: inversion, DC offset (user + calibration) and the only rounding to raw.
    //   final = (dsp_out * mult + offset_fx18) >> 18, computed exactly as
    //   (sign * dsp_out + offset_raw_fx3 + ((dsp_out * delta + offset_frac) >> 15)) >> 3.
    //   |dsp_out| <= 65535, |delta| <= 32768, offset_frac < 2^15: the product term < 2^31.
    dsp_out = ClampI32(dsp_out, OUT_GUARD_RAW_FX3);
    int32_t out_fx3 = pipe->out_sign * dsp_out + pipe->out_offset_raw_fx3
                    + ((dsp_out * pipe->out_delta_q0_15 + pipe->out_offset_frac) >> Q15_FRAC_BITS);
    int32_t final_out = out_fx3 >> RAW_FX3_FRAC_BITS;

    // Branchless fixed-point saturation logic (Clips dynamically to 0 - 4095)
    if ((uint32_t)final_out > MIMIC_ADC_MAX_VALUE) {
        final_out = (~(final_out >> 31)) & MIMIC_ADC_MAX_VALUE;
    }

#ifdef ENABLE_DAC_PULSE
    __asm volatile ("" : "+r" (final_out));

    static uint16_t dac_pulse = MIMIC_ADC_MIN_VALUE;
    dac_pulse = (dac_pulse < MIMIC_ADC_MID_VALUE) ? MIMIC_ADC_MAX_VALUE : MIMIC_ADC_MIN_VALUE;
    final_out = dac_pulse;
#endif

    return (uint16_t)final_out;
}

void MimicDSP_SetDecimation(uint8_t decimation_n) {
    if (decimation_n > MIMIC_DECIMATION_N_MAX) {
        decimation_n = MIMIC_DECIMATION_N_MAX;
    }
    uint32_t new_mask = (1U << decimation_n) - 1;

    if (new_mask == decimation_mask) {
        return;
    }

    // Value currently on the output (called while the engine is stopped)
    int32_t hold_raw = (decimation_mask == 0) ? y_new_raw
                                              : (out_raw_fx16 >> FX16_FRAC_BITS);

    decimation_mask  = new_mask;
    decimation_shift = decimation_n;

    // Average of 2^N samples in raw_fx3, rounded: ((sum << l) + round) >> r
    if (decimation_n >= RAW_FX3_FRAC_BITS) {
        decimation_fx3_lshift = 0;
        decimation_fx3_rshift = decimation_n - RAW_FX3_FRAC_BITS;
        decimation_fx3_round  = (1U << decimation_fx3_rshift) >> 1;
    } else {
        decimation_fx3_lshift = RAW_FX3_FRAC_BITS - decimation_n;
        decimation_fx3_rshift = 0;
        decimation_fx3_round  = 0;
    }

    // Restart the interpolation from the held output (no jump)
    adc_accumulator     = 0;
    decimation_count    = 0;
    y_old_raw           = hold_raw;
    y_new_raw           = hold_raw;
    delta_step_raw_fx16 = 0;
    out_raw_fx16        = hold_raw << FX16_FRAC_BITS;
}

// Configuration for Test and Debug Features
#define ENABLE_DSP_PROFILING

static inline __attribute__((always_inline)) void SampleBody(void) {
#ifdef ENABLE_HEARTBEAT
    static volatile uint32_t mimic_tc_count = 0;
    if (++mimic_tc_count >= 100000u) {      /* 0.5 sec at 200 kHz */
        mimic_tc_count = 0;
        GPIOF->ODR ^= GPIO_PIN_6;
    }
#endif
#ifdef ENABLE_DSP_PROFILING
    uint32_t start_tick = SysTick->VAL;
#endif

    // =========================================================================
    // 2. ADC Data Read (Automatically clears hardware EOC flag)
    // =========================================================================
    uint32_t raw_val = ADC1->DR;

    // =========================================================================
    // 3. Signal Saturation Detection
    // =========================================================================
    if (raw_val == MIMIC_ADC_MIN_VALUE || raw_val >= MIMIC_ADC_MAX_VALUE) {
        MimicDevice_SetErrorFlag_ISR(MIMIC_STATUS_SIGNAL_SATURATION);
    }

    // =========================================================================
    // 4. DSP Processing & Output Slot Preloading for Next Cycle
    // =========================================================================
    uint32_t out_raw;

    if (decimation_mask == 0) {
        uint32_t x_raw_fx3 = raw_val << RAW_FX3_FRAC_BITS;
        MimicDevice_SetAdcVal_ISR(x_raw_fx3);
        out_raw = MimicDSP_ProcessSample((int32_t)x_raw_fx3);
        y_new_raw = (int32_t)out_raw;   // Current output, kept for a later decimation change
    } else {
        adc_accumulator += raw_val;
        decimation_count++;

        out_raw_fx16 += delta_step_raw_fx16;
        out_raw = (uint32_t)(out_raw_fx16 >> FX16_FRAC_BITS);

        if ((decimation_count & decimation_mask) == 0) {
            // Average of the 2^N samples, rounded to raw_fx3 (sum <= 4095 * 2^16 < 2^28)
            uint32_t x_raw_fx3 = ((adc_accumulator << decimation_fx3_lshift) + decimation_fx3_round)
                               >> decimation_fx3_rshift;
            MimicDevice_SetAdcVal_ISR(x_raw_fx3);

            y_old_raw = y_new_raw;
            y_new_raw = MimicDSP_ProcessSample((int32_t)x_raw_fx3);

            int32_t diff_raw_fx16 = (y_new_raw - y_old_raw) << FX16_FRAC_BITS;
            delta_step_raw_fx16 = diff_raw_fx16 >> decimation_shift;

            out_raw_fx16 = y_old_raw << FX16_FRAC_BITS;
            adc_accumulator = 0;
        }
    }

    mimic_output_slot = MuxDacGpio(out_raw);
#ifdef MIMIC_ENGINE_CPU
    DAC1->DHR12R1 = out_raw;
#endif

    // =========================================================================
    // 5. Exception (Overrun) Checking
    // =========================================================================
    if (ADC1->SR & ADC_SR_OVER) {
        MimicDevice_SetErrorFlag_ISR(MIMIC_STATUS_ADC_OVER_ERR);
        ADC1->SR = ADC_SR_OVER; 
    }

#ifdef ENABLE_DSP_PROFILING
    uint32_t end_tick = SysTick->VAL;
    uint32_t cycles;
    if (start_tick < end_tick) {
        cycles = start_tick + (SysTick->LOAD - end_tick);
    } else {
        cycles = start_tick - end_tick;
    }
    MimicDevice_UpdateCpuCyclesMax_ISR(cycles);
#endif
}

// =========================================================
// Deterministic Interrupt Service Routines (.ramfunc)
// =========================================================

#ifdef MIMIC_ENGINE_DMA
__attribute__((section(".ramfunc")))
void DMA1_Channel1_IRQHandler(void)
{
    DMA1->IFCR = DMA_FLAG_TC1;    /* Clear DMA Transfer Complete flag */
    SampleBody();                 /* Hardware output transfer has already completed */
}
#endif

#ifdef MIMIC_ENGINE_CPU
/**
 * @brief ADC and Comparator Interrupt Handler for Analog DAC Route
 */
__attribute__((used, section(".ramfunc")))
void ADC_COMP_IRQHandler_DAC(void) {
    // 1. [CRITICAL] DAC Output Trigger (Guarantees zero-jitter time determinism)
    DAC1->SWTRIGR = 1;
    SampleBody();
}

/**
 * @brief ADC and Comparator Interrupt Handler for Digital GPIO Route
 */
__attribute__((used, section(".ramfunc")))
void ADC_COMP_IRQHandler_GPIO(void) {
    // 1. [CRITICAL] GPIO Output Trigger (Guarantees zero-jitter time determinism)
    OPA2_PORT->BSRR = mimic_output_slot;
    SampleBody();
}

/**
 * @brief ADC and Comparator Interrupt Handler for Open (High-Z) Route
 */
__attribute__((used, section(".ramfunc")))
void ADC_COMP_IRQHandler(void) {
    SampleBody();
}
#endif
