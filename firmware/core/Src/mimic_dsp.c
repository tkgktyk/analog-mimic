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

// =========================================================
// Deterministic Output Slot (Shared between ISR and DMA)
// =========================================================
static volatile uint32_t mimic_output_slot = MIMIC_ADC_MID_VALUE;

// =========================================================
// Fixed-Point Arithmetic Configurations
// =========================================================
// Number formats (see also mimic_registers.h)
//   _raw          : ADC / DAC code, 0 = 0 V, 4096 = VDD (integer, 0 - 4095)
//   _raw_fx3      : raw << 3, 1 LSB = 8 (interface format for voltages)
//   _raw_vref_fx3 : (raw << 3) - Vref, relative to the common Vref, 1 LSB = 8
//   _raw_ctr_fx4  : (raw - MID) << 4, 1 LSB = 16 (slew, S&H; no signal center)
//   _raw_fx15     : raw << 15 (output stage offset)
//   _raw_fx16     : raw << 16, 1 LSB = 65536 (decimation interpolation)
//   _q0_15        : signed coefficient, 15 fractional bits (-1.0 to < 1.0)
//   _q1_15        : signed multiplier, 15 fractional bits (-2.0 to < 2.0, int32)
//   _uq1_15       : unsigned gain, 15 fractional bits (0 to < 2.0, 1.0 = 32768)
//   _mant / _exp  : mantissa (q0_15) and exponent (shift) pair
#define CTR_FX4_FRAC_BITS   4  // raw -> raw_ctr_fx4 (12 + 4 = 16 bits)
#define RAW_FX3_FRAC_BITS   MIMIC_RAW_FX3_FRAC_BITS
#define RAW_FX3_HALF        (1 << (RAW_FX3_FRAC_BITS - 1))                    // 0.5 LSB in raw_fx3
#define Q15_FRAC_BITS       15 // Fractional bits of q0_15 / q1_15 / uq1_15
#define FX16_FRAC_BITS      16 // Fractional bits of raw_fx16 (decimation interpolation)

#define MID_ROUND_RAW_FX4   ((MIMIC_ADC_MID_VALUE << CTR_FX4_FRAC_BITS) + (1 << (CTR_FX4_FRAC_BITS - 1)))
#define OUT_ROUND_RAW_FX15  (1 << (Q15_FRAC_BITS - 1))                         // 0.5 LSB in raw_fx15

// Overflow guards: values beyond these saturate in the output stage anyway
#define OUT_GUARD_RAW       (2 * (MIMIC_ADC_MAX_VALUE + 1))                    // +/-8192 LSB
#define OUT_GUARD_RAW_FX3   (OUT_GUARD_RAW << RAW_FX3_FRAC_BITS)
#define INTEGRAL_STATE_LIM  (1 << 16)  // |state * leak_q0_15| < 2^31

// =========================================================
// DSP Specific Constants
// =========================================================
// LOG mode: per-mode tables in raw << LOG_OUT_FRAC_BITS (= raw_fx3 with 4 more bits)
#define LOG_SEG_BITS        6                              // 64 segments per octave
#define LOG_OUT_FRAC_BITS   7
#define LOG_OUT_ROUND       (1 << (LOG_OUT_FRAC_BITS - 1))
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
    int32_t            out_mult_q1_15;      // Output gain, signed (negative when inverted)
    int32_t            out_offset_raw_fx15; // Output offset incl. inversion and +0.5 LSB rounding
    int32_t            vref_raw_fx3;        // Common reference (Vref), 0 V - VDD
    int32_t            vref_round_raw_fx3;  // vref_raw_fx3 + 0.5 LSB (raw_vref_fx3 -> raw rounding)
    uint8_t            current_mode;
    MimicOutputRoute_t output_route;
    uint8_t            global_flags;
} MimicPipeline_t;

static MimicPipeline_t pipeline;

#define DELAY_BUFFER_SIZE 4096
#define DELAY_BUFFER_MASK (DELAY_BUFFER_SIZE - 1)
static uint16_t delay_buffer_raw[DELAY_BUFFER_SIZE] = {0};


// =========================================================
// Complete DSP Mode-Specific Contexts (Unified Structs)
// =========================================================
// Every mode with a signal center works around the common Vref (raw_vref_fx3, 1 LSB = 8).
// Modes without a center keep their own formats.

typedef struct {
    uint8_t type;            // Config: MIMIC_FILTER_1ST_*
    int32_t alpha_q0_15;     // Config
    int32_t accum;           // State (raw_vref_fx3 << Q15_FRAC_BITS)
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
    int32_t gain_exp;        // Config
    int32_t mant_lim;        // Config: |mant product| limit before the shift (overflow guard)
} Pga_t;

typedef struct {
    uint8_t type;            // Config: MIMIC_COMPARATOR_*
    int32_t upper_raw;       // Config: "adc > upper" (floor of the raw_fx3 threshold)
    int32_t lower_raw;       // Config: "adc < lower" (ceil of the raw_fx3 threshold)
    int32_t wave_last_raw;   // State
} Comparator_t;

typedef struct {
    uint8_t type;             // Config: MIMIC_MATH_*
    int32_t scale_mant_q0_15; // Config: scale = mant * 2^exp
    int32_t scale_exp;        // Config
    int32_t mant_lim;         // Config: overflow guard (derivative)
    int32_t last_raw_vref_fx3;// State
} Math_t;

typedef struct {
    int32_t max_step_raw_ctr_fx4; // Config: max change per sample
    int32_t last_raw_ctr_fx4;     // State
} Slew_t;

typedef struct {
    int32_t upper_raw;       // Config: comparison (floor)
    int32_t lower_raw;       // Config: comparison (ceil)
    int32_t upper_out_raw;   // Config: output level (rounded)
    int32_t lower_out_raw;   // Config: output level (rounded)
} Clipper_t;

typedef struct {
    int32_t decay_q0_15;      // Config
    uint8_t polarity;         // Config
    int32_t last_raw_vref_fx3;// State
} Envelope_t;

typedef struct {
    uint8_t type;            // Config: MIMIC_RECT_FULL / MIMIC_RECT_HALF
    uint8_t polarity;        // Config: MIMIC_POLARITY_*
} Rectifier_t;

typedef struct {
    uint32_t period_samples;  // Config
    uint32_t track_samples;   // Config
    uint32_t counter;         // State
    int32_t  last_raw_ctr_fx4;// State
} SampleHold_t;

typedef struct {
    uint32_t delay_samples;   // Config
    uint32_t delay_write_ptr; // State
} Delay_t;

typedef struct {
    uint8_t type;             // Config: MIMIC_LOG_TYPE_*
    int32_t seg[65];          // LOG: slope * log2(1 + i/64), ANTILOG: unit * 2^(i/64) (raw << 7)
    int32_t oct[12];          // LOG: Vref + slope * (e - log2(unit)) for e = 0 - 11 (raw << 7)
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

static int32_t  y_old_raw           = MIMIC_ADC_MID_VALUE;
static int32_t  y_new_raw           = MIMIC_ADC_MID_VALUE;
static int32_t  delta_step_raw_fx16 = 0;
static int32_t  out_raw_fx16        = (MIMIC_ADC_MID_VALUE << FX16_FRAC_BITS);

// =========================================================
// Utilities & I2C Parameter Parsers (32-bit Safe)
// =========================================================

// --- raw_ctr_fx4: relative to MID (modes that do not depend on Vref) ---
static inline __attribute__((always_inline)) int32_t RawToCtrFx4(int32_t raw) {
    return (raw - MIMIC_ADC_MID_VALUE) << CTR_FX4_FRAC_BITS;
}

static inline __attribute__((always_inline)) int32_t CtrFx4ToRaw(int32_t v_raw_ctr_fx4) {
    return (v_raw_ctr_fx4 + MID_ROUND_RAW_FX4) >> CTR_FX4_FRAC_BITS;   // rounded
}

// --- raw_vref_fx3: relative to the common Vref ---
static inline __attribute__((always_inline)) int32_t RawToVrefFx3(int32_t raw) {
    return (raw << RAW_FX3_FRAC_BITS) - pipeline.vref_raw_fx3;
}

static inline __attribute__((always_inline)) int32_t VrefFx3ToRaw(int32_t v_raw_vref_fx3) {
    return (v_raw_vref_fx3 + pipeline.vref_round_raw_fx3) >> RAW_FX3_FRAC_BITS;   // rounded
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
// 2. DSP Core Functions (Return raw codes; out-of-range values saturate in the output stage)
// =========================================================

// 1st-order filter: LPF y = Vref + LPF(d), HPF y = Vref + (d - LPF(d)), d = x - Vref.
// Like an RC filter returning to Vref: a step of Vref settles with the time constant.
static inline __attribute__((always_inline)) int32_t ProcessFilter1st(int32_t adc_val) {
    Filter1st_t *p = &dsp_ctx.filter1st;

    int32_t in_raw_vref_fx3 = RawToVrefFx3(adc_val);
    int32_t state_raw_vref_fx3 = p->accum >> Q15_FRAC_BITS;
    p->accum += (in_raw_vref_fx3 - state_raw_vref_fx3) * p->alpha_q0_15;

    int32_t out_raw_vref_fx3 = p->accum >> Q15_FRAC_BITS;
    if (p->type == MIMIC_FILTER_1ST_HPF) out_raw_vref_fx3 = in_raw_vref_fx3 - out_raw_vref_fx3;
    return VrefFx3ToRaw(out_raw_vref_fx3);
}

// Biquad mode (TPT state-variable filter): y = Vref + H(x - Vref)
//   v  = x - g1 * s1 - s2,  hp = v / (1 + g k + g^2) = v - e * v
//   bp = g * hp + s1,  s1 = bp + g * hp
//   lp = g * bp + s2,  s2 = lp + g * bp
//   LPF = lp, HPF = hp, BPF = k * bp (0 dB peak), NOTCH = x - k * bp
// Every product is (integer signal, <= 2^19) x (coefficient, <= 2^12), within int32.
static inline __attribute__((always_inline)) int32_t ProcessBiquadFilter(int32_t adc_val) {
    Svf_t *p = &dsp_ctx.svf;
    int32_t x  = RawToVrefFx3(adc_val);
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
        default: y = x; break;   // UNITY: same work, input passed through (profiling)
    }

    // Keep the output-stage multiply within int32 for high-Q filters
    return VrefFx3ToRaw(ClampI32(y, OUT_GUARD_RAW_FX3));
}

// PGA: y = Vref + G * (x - Vref)
static inline __attribute__((always_inline)) int32_t ProcessPga(int32_t adc_val) {
    Pga_t *p = &dsp_ctx.pga;

    int32_t in_raw_vref_fx3 = RawToVrefFx3(adc_val);
    int32_t m = (in_raw_vref_fx3 * p->gain_mant_q0_15) >> Q15_FRAC_BITS;
    m = ClampI32(m, p->mant_lim);
    return VrefFx3ToRaw(m << p->gain_exp);
}

// Comparator: window (HIGH outside [lower, upper]) or Schmitt trigger (hysteresis)
static inline __attribute__((always_inline)) int32_t ProcessComparator(int32_t adc_val) {
    Comparator_t *p = &dsp_ctx.comp;

    if (p->type == MIMIC_COMPARATOR_WINDOW) {
        if (adc_val > p->upper_raw || adc_val < p->lower_raw) return MIMIC_ADC_MAX_VALUE;
        return MIMIC_ADC_MIN_VALUE;
    }
    if (adc_val > p->upper_raw) p->wave_last_raw = MIMIC_ADC_MAX_VALUE;
    else if (adc_val < p->lower_raw) p->wave_last_raw = MIMIC_ADC_MIN_VALUE;
    return p->wave_last_raw;
}

// Envelope: tracks the swing from Vref and decays toward Vref
static inline __attribute__((always_inline)) int32_t ProcessEnvelopeFollower(int32_t adc_val) {
    Envelope_t *p = &dsp_ctx.envelope;

    int32_t in_raw_vref_fx3 = RawToVrefFx3(adc_val);
    if (p->polarity == MIMIC_POLARITY_NEGATIVE) in_raw_vref_fx3 = -in_raw_vref_fx3;
    if (in_raw_vref_fx3 < 0) in_raw_vref_fx3 = 0;

    if (in_raw_vref_fx3 > p->last_raw_vref_fx3) {
        p->last_raw_vref_fx3 = in_raw_vref_fx3;
    } else {
        p->last_raw_vref_fx3 = (p->last_raw_vref_fx3 * p->decay_q0_15) >> Q15_FRAC_BITS;
    }
    return VrefFx3ToRaw(p->last_raw_vref_fx3);   // magnitude above Vref (same as before for both polarities)
}

static inline __attribute__((always_inline)) int32_t ProcessClipper(int32_t adc_val) {
    Clipper_t *p = &dsp_ctx.clipper;

    if (adc_val > p->upper_raw) return p->upper_out_raw;
    if (adc_val < p->lower_raw) return p->lower_out_raw;
    return adc_val; 
}

// LOG:     y = Vref + slope * log10(x / unit)      (x: magnitude from 0 V)
// ANTILOG: y = unit * 10^((x - Vref) / slope)     (y: magnitude from 0 V)
static inline __attribute__((always_inline)) int32_t ProcessLog(int32_t adc_val) {
    Log_t *p = &dsp_ctx.log;

    if (p->type == MIMIC_LOG_TYPE_LOG) {
        uint32_t x = (adc_val < 1) ? 1u : (uint32_t)adc_val;

        // e = floor(log2(x)), x = 1 - 4095 (binary search: Cortex-M0+ has no CLZ)
        uint32_t e = 0;
        if (x >> 8)       e = 8;
        if (x >> (e + 4)) e += 4;
        if (x >> (e + 2)) e += 2;
        if (x >> (e + 1)) e += 1;

        uint32_t f    = (x << (11 - e)) - 2048u;           // mantissa fraction, 11 bits
        uint32_t i    = f >> (11 - LOG_SEG_BITS);
        int32_t  frac = (int32_t)(f & ((1u << (11 - LOG_SEG_BITS)) - 1));
        int32_t  y    = p->oct[e] + p->seg[i]
                      + (((p->seg[i + 1] - p->seg[i]) * frac) >> (11 - LOG_SEG_BITS));
        return (y + LOG_OUT_ROUND) >> LOG_OUT_FRAC_BITS;
    }

    // ANTILOG
    int32_t d = ClampI32(RawToVrefFx3(adc_val), p->d_lim);
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
    return (m + LOG_OUT_ROUND) >> LOG_OUT_FRAC_BITS;
}

// Math: derivative y = Vref + k * dx/dt, or leaky integrator leaking toward Vref
static inline __attribute__((always_inline)) int32_t ProcessMath(int32_t adc_val) {
    Math_t *p = &dsp_ctx.math;

    int32_t in_raw_vref_fx3 = RawToVrefFx3(adc_val);
    int32_t s = p->scale_exp;

    if (p->type == MIMIC_MATH_DERIVATIVE) {
        int32_t diff = in_raw_vref_fx3 - p->last_raw_vref_fx3;
        p->last_raw_vref_fx3 = in_raw_vref_fx3;

        int32_t diff_mult = (diff * p->scale_mant_q0_15) >> Q15_FRAC_BITS;
        int32_t out_raw_vref_fx3 = (s >= 0) ? (ClampI32(diff_mult, p->mant_lim) << s)
                                            : (diff_mult >> -s);
        return VrefFx3ToRaw(out_raw_vref_fx3);
    }

    // Integral (s <= 1, limited by UpdateParameters)
    int32_t history_leaked = (p->last_raw_vref_fx3 * p->scale_mant_q0_15) >> Q15_FRAC_BITS;
    int32_t input_scaled = (s >= 0) ? (in_raw_vref_fx3 << s) : (in_raw_vref_fx3 >> -s);
    p->last_raw_vref_fx3 = ClampI32(history_leaked + input_scaled, INTEGRAL_STATE_LIM);
    return VrefFx3ToRaw(p->last_raw_vref_fx3);
}

static inline __attribute__((always_inline)) int32_t ProcessSlewRateLimiter(int32_t adc_val) {
    Slew_t *p = &dsp_ctx.slew;

    int32_t in_raw_ctr_fx4 = RawToCtrFx4(adc_val);
    int32_t diff = in_raw_ctr_fx4 - p->last_raw_ctr_fx4;

    int32_t clamped_diff = diff;
    if (clamped_diff > p->max_step_raw_ctr_fx4)  clamped_diff = p->max_step_raw_ctr_fx4;
    if (clamped_diff < -p->max_step_raw_ctr_fx4) clamped_diff = -p->max_step_raw_ctr_fx4;
    
    p->last_raw_ctr_fx4 += clamped_diff;
    return CtrFx4ToRaw(p->last_raw_ctr_fx4);
}

// Rectifier: folds at Vref (d = x - Vref)
//   full : positive Vref + |d|,        negative Vref - |d|
//   half : positive Vref + max(d, 0),  negative Vref + min(d, 0)
static inline __attribute__((always_inline)) int32_t ProcessRectifier(int32_t adc_val) {
    Rectifier_t *p = &dsp_ctx.rect;

    int32_t d = RawToVrefFx3(adc_val);
    if (p->polarity == MIMIC_POLARITY_NEGATIVE) d = -d;   // work on the positive side
    if (d < 0) d = (p->type == MIMIC_RECT_FULL) ? -d : 0;
    if (p->polarity == MIMIC_POLARITY_NEGATIVE) d = -d;
    return VrefFx3ToRaw(d);
}

static inline __attribute__((always_inline)) int32_t ProcessSampleAndHold(int32_t adc_val) {
    SampleHold_t *p = &dsp_ctx.sh;

    p->counter++;
    if (p->counter >= p->period_samples) p->counter = 0;
    if (p->counter < p->track_samples) p->last_raw_ctr_fx4 = RawToCtrFx4(adc_val);
    return CtrFx4ToRaw(p->last_raw_ctr_fx4);
}

static inline __attribute__((always_inline)) int32_t ProcessDelay(int32_t adc_val) {
    Delay_t *p = &dsp_ctx.delay;

    delay_buffer_raw[p->delay_write_ptr] = adc_val;
    uint32_t read_ptr = (p->delay_write_ptr - p->delay_samples) & DELAY_BUFFER_MASK;
    int32_t out_val = delay_buffer_raw[read_ptr];
    
    p->delay_write_ptr = (p->delay_write_ptr + 1) & DELAY_BUFFER_MASK;
    return out_val;
}

// =========================================================
// Initialization and Parameter Updates
// =========================================================

void MimicDSP_Init(void) {
    pipeline.current_mode        = MIMIC_MODE_ID_BYPASS_DSP;
    pipeline.output_route        = MIMIC_OUT_OPEN;
    pipeline.global_flags        = 0;
    pipeline.out_mult_q1_15      = 1 << Q15_FRAC_BITS;
    pipeline.out_offset_raw_fx15 = OUT_ROUND_RAW_FX15;
    pipeline.vref_raw_fx3        = MIMIC_VREF_DEFAULT_RAW_FX3;
    pipeline.vref_round_raw_fx3  = MIMIC_VREF_DEFAULT_RAW_FX3 + RAW_FX3_HALF;

    memset((void*)&dsp_ctx, 0, sizeof(dsp_ctx));
    dsp_ctx.comp.wave_last_raw = MIMIC_ADC_MID_VALUE;

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
        // seg[i] = K * log2(1 + i/64), oct[e] = Vref + K * (e - log2(unit / 8)), in raw << 7
        for (int i = 0; i <= (1 << LOG_SEG_BITS); i++) {
            c->seg[i] = (int32_t)(((int64_t)k_q16 * log2_1pf_q15[i]) >> 27);
        }
        int32_t log2_unit_q15 = Log2Q15((uint32_t)unit) - (RAW_FX3_FRAC_BITS << 15);
        int32_t vref_q = pipeline.vref_raw_fx3 << (LOG_OUT_FRAC_BITS - RAW_FX3_FRAC_BITS);
        for (int e = 0; e < 12; e++) {
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

// raw_fx3 -> integer raw code
static inline int32_t Fx3FloorToRaw(int32_t v) { return v >> RAW_FX3_FRAC_BITS; }
static inline int32_t Fx3CeilToRaw(int32_t v)  { return (v + (1 << RAW_FX3_FRAC_BITS) - 1) >> RAW_FX3_FRAC_BITS; }
static inline int32_t Fx3RoundToRaw(int32_t v) { return (v + RAW_FX3_HALF) >> RAW_FX3_FRAC_BITS; }

// Limit for a value that is shifted left by exp afterwards, so that the result stays
// within +/-OUT_GUARD_RAW_FX3 (saturates in the output stage, never overflows).
static int32_t MantLimitForExp(int32_t exp) {
    if (exp <= 0) return INT32_MAX;
    if (exp >= 31) return 1;
    int32_t lim = OUT_GUARD_RAW_FX3 >> exp;
    return (lim > 0) ? lim : 1;
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
    pipeline.vref_raw_fx3       = vref;
    pipeline.vref_round_raw_fx3 = vref + RAW_FX3_HALF;

    // 2. Global Decimation & Offset Parameters
    MimicDSP_SetDecimation(config->decimation_n);

    //   final = (dsp_out * mult + offset_fx15) >> 15
    //     normal  : gain * x + offset
    //     inverted: 4095 - (gain * x + offset)
    //   OUT_ROUND_RAW_FX15 (+0.5 LSB) turns the floor of the arithmetic shift into rounding.
    int32_t offset_raw_fx15 = config->offset_raw_fx3 << (Q15_FRAC_BITS - RAW_FX3_FRAC_BITS);
    if (pipeline.global_flags & MIMIC_FLAG_INV_OUT) {
        pipeline.out_mult_q1_15      = -config->gain_uq1_15;
        pipeline.out_offset_raw_fx15 = ((int32_t)MIMIC_ADC_MAX_VALUE << Q15_FRAC_BITS)
                                     - offset_raw_fx15 + OUT_ROUND_RAW_FX15;
    } else {
        pipeline.out_mult_q1_15      = config->gain_uq1_15;
        pipeline.out_offset_raw_fx15 = offset_raw_fx15 + OUT_ROUND_RAW_FX15;
    }

    // 3. Reset mode-specific state history upon mode transition
    if (pipeline.current_mode != previous_mode) {
        memset((void*)&dsp_ctx, 0, sizeof(dsp_ctx));
        dsp_ctx.comp.wave_last_raw = MIMIC_ADC_MID_VALUE;

        if (pipeline.current_mode == MIMIC_MODE_ID_DELAY) {
            for (uint32_t i = 0; i < DELAY_BUFFER_SIZE; i++) {
                delay_buffer_raw[i] = MIMIC_ADC_MID_VALUE;
            }
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

        case MIMIC_MODE_ID_FILTER_1ST:
            dsp_ctx.filter1st.type        = *(p + 0);
            dsp_ctx.filter1st.alpha_q0_15 = ParseS16(p + 1);
            break;

        case MIMIC_MODE_ID_PGA: {
            int32_t exp = (int8_t)*(p + 2);
            if (exp < 0) exp = 0;                       // gain mantissa already covers < 1
            dsp_ctx.pga.gain_mant_q0_15 = ParseS16(p + 0);
            dsp_ctx.pga.gain_exp        = exp;
            dsp_ctx.pga.mant_lim        = MantLimitForExp(exp);
            break;
        }

        case MIMIC_MODE_ID_COMPARATOR:
            dsp_ctx.comp.type      = *(p + 0);
            dsp_ctx.comp.upper_raw = Fx3FloorToRaw(ParseS16(p + 1));
            dsp_ctx.comp.lower_raw = Fx3CeilToRaw(ParseS16(p + 3));
            break;

        case MIMIC_MODE_ID_MATH: {
            int32_t exp = (int8_t)*(p + 3);
            dsp_ctx.math.type = *(p + 0);
            if (dsp_ctx.math.type == MIMIC_MATH_INTEGRAL && exp > 1) exp = 1;
            dsp_ctx.math.scale_mant_q0_15 = ParseS16(p + 1);
            dsp_ctx.math.scale_exp        = exp;
            dsp_ctx.math.mant_lim         = MantLimitForExp(exp);
            break;
        }

        case MIMIC_MODE_ID_SLEW_RATE_LIMITER: {
            int32_t step = ParseS16(p + 0);
            if (step < 0) step = 0;
            dsp_ctx.slew.max_step_raw_ctr_fx4 = step << (CTR_FX4_FRAC_BITS - RAW_FX3_FRAC_BITS);
            break;
        }

        case MIMIC_MODE_ID_CLIPPER: {
            int32_t upper = ParseS16(p + 0);
            int32_t lower = ParseS16(p + 2);
            dsp_ctx.clipper.upper_raw     = Fx3FloorToRaw(upper);
            dsp_ctx.clipper.lower_raw     = Fx3CeilToRaw(lower);
            dsp_ctx.clipper.upper_out_raw = Fx3RoundToRaw(upper);
            dsp_ctx.clipper.lower_out_raw = Fx3RoundToRaw(lower);
            break;
        }

        case MIMIC_MODE_ID_LOG:
            ConfigureLog(p);    // depends on Vref: rebuilt on every update
            break;

        case MIMIC_MODE_ID_ENVELOPE_FOLLOWER:
            dsp_ctx.envelope.decay_q0_15 = ParseS16(p + 0);
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
__attribute__((always_inline)) static inline uint16_t MimicDSP_ProcessSample(uint16_t adc_val) {
    MimicPipeline_t *pipe = &pipeline;
    int32_t dsp_out = 0;

    if (pipe->current_mode == MIMIC_MODE_ID_FILTER_BIQUAD) {
        dsp_out = ProcessBiquadFilter(adc_val);
    } else {
        switch (pipe->current_mode) {
            case MIMIC_MODE_ID_PGA:                 dsp_out = ProcessPga(adc_val); break;
            case MIMIC_MODE_ID_FILTER_1ST:          dsp_out = ProcessFilter1st(adc_val); break;
            case MIMIC_MODE_ID_COMPARATOR:          dsp_out = ProcessComparator(adc_val); break;
            case MIMIC_MODE_ID_ENVELOPE_FOLLOWER:   dsp_out = ProcessEnvelopeFollower(adc_val); break;
            case MIMIC_MODE_ID_CLIPPER:             dsp_out = ProcessClipper(adc_val); break;
            case MIMIC_MODE_ID_LOG:                 dsp_out = ProcessLog(adc_val); break;
            case MIMIC_MODE_ID_MATH:                dsp_out = ProcessMath(adc_val); break;
            case MIMIC_MODE_ID_RECTIFIER:           dsp_out = ProcessRectifier(adc_val); break;
            case MIMIC_MODE_ID_SLEW_RATE_LIMITER:   dsp_out = ProcessSlewRateLimiter(adc_val); break;
            case MIMIC_MODE_ID_SAMPLE_AND_HOLD:     dsp_out = ProcessSampleAndHold(adc_val); break;
            case MIMIC_MODE_ID_DELAY:               dsp_out = ProcessDelay(adc_val); break;
            default:                                dsp_out = adc_val; break;
        }
    }

    // Post-Processing: inversion, DC offset (user + calibration) and rounding in one step
    int32_t final_out = (dsp_out * pipe->out_mult_q1_15 + pipe->out_offset_raw_fx15) >> Q15_FRAC_BITS;

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
        MimicDevice_SetAdcVal_ISR(raw_val);
        out_raw = MimicDSP_ProcessSample(raw_val);
        y_new_raw = (int32_t)out_raw;   // Current output, kept for a later decimation change
    } else {
        adc_accumulator += raw_val;
        decimation_count++;

        out_raw_fx16 += delta_step_raw_fx16;
        out_raw = (uint32_t)(out_raw_fx16 >> FX16_FRAC_BITS);

        if ((decimation_count & decimation_mask) == 0) {
            uint32_t x_in = adc_accumulator >> decimation_shift;
            MimicDevice_SetAdcVal_ISR(x_in); 

            y_old_raw = y_new_raw;
            y_new_raw = MimicDSP_ProcessSample(x_in);

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
