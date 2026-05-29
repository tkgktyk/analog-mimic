/**
 ******************************************************************************
 * @file    mimic_dsp.c
 * @author  TAKAGI Katsuyuki
 * @brief   Implementation of the Core Emulation Engines, Q15 Fixed-Point DSP 
 * Routines, and Deterministic Operational Mode Execution Blocks.
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
#include "mimic_registers.h"
#include "mimic_flash.h"
#include <string.h>

// =========================================================
// Q15 Format Configurations
// =========================================================
#define Q15_SHIFT 4            // For 12-bit ADC (12 + 4 = 16 bits)
#define FRACTIONAL_BITS_Q15 15 // Q15 fractional bits for 32-bit shifting

// =========================================================
// DSP Specific Constants & Magic Number Elimination
// =========================================================
#define LUT_INDEX_SHIFT   6
#define LUT_FRAC_MASK     0x3F
#define BIQUAD_ROUND_VAL  8192
#define BIQUAD_SHIFT      14

MimicCallback_t cb_enable_output = NULL;
MimicCallback_t cb_disable_output = NULL;

// =========================================================
// Look-Up Tables (LUT)
// =========================================================
static const int32_t lut_log[65] = {
    -32768, -25000, -20000, -16000, -13000, -10500, -8500, -6800, -5300, -4000,
    -2800, -1700, -700, 200, 1000, 1800, 2500, 3200, 3800, 4400,
    4900, 5400, 5900, 6300, 6700, 7100, 7500, 7800, 8100, 8400,
    8700, 9000, 9200, 9500, 9700, 9900, 10100, 10300, 10500, 10700,
    10900, 11100, 11300, 11500, 11600, 11800, 12000, 12100, 12300, 12400,
    12600, 12700, 12900, 13000, 13100, 13300, 13400, 13500, 13600, 13800,
    13900, 14000, 14100, 14200, 14300};

static const int32_t lut_antilog[65] = {
    -32768, -31768, -30000, -27000, -23000, -18000, -12000, -5000, 2000, 8000,
    13000, 17000, 20000, 22500, 24500, 26000, 27300, 28400, 29300, 30000,
    30600, 31100, 31500, 31800, 32100, 32300, 32500, 32600, 32700, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767};

// =========================================================
// 1. DSP Internal State Variables (Promoted to 32-bit)
// =========================================================
// Pipeline context structure.
// Placing 32-bit variables at the top to eliminate alignment padding gaps.
typedef struct {
    int32_t  out_mult_q15;          // Signed Q15 multiplier
    int32_t  out_offset_calc;   // Final computed offset including inversion
    uint8_t  current_mode;
    uint8_t  global_flags;
    // 2 bytes of implicit padding will be automatically inserted here by the compiler
} MimicPipeline_t;

// Exactly one instance allocated in static memory
static MimicPipeline_t pipeline;

#define DELAY_BUFFER_SIZE 4096
#define DELAY_BUFFER_MASK (DELAY_BUFFER_SIZE - 1)
static uint16_t delay_buffer[DELAY_BUFFER_SIZE] = {0};

// For non-linear LUT: kept outside the union context to allow safe dynamic pointer 
// switching of the active LUT across different operational modes.
static uint8_t nonlinear_lut_type = MIMIC_LUT_LOG;
const int32_t *active_lut_ptr = lut_log;

// =========================================================
// Complete DSP Mode-Specific Contexts (Unified Structs)
// =========================================================

// --- 1st Order Filter (LPF/HPF) ---
typedef struct {
    int32_t alpha_q15;  // Config
    int32_t accum;      // State
} Filter1st_t;

// --- Biquad Filter (DF2T) ---
typedef struct {
    int32_t b0, b1, b2; // Config
    int32_t a1, a2;     // Config
    int32_t s1, s2;     // State
} Biquad_t;

// --- Programmable Gain Amplifier (PGA) ---
typedef struct {
    int32_t gain_fract_q15; // Config
    int32_t gain_shift;     // Config
    int32_t offset_q15;     // Config
} Pga_t;

// --- Window / Schmitt Comparators ---
typedef struct {
    int32_t upper;          // Config
    int32_t lower;          // Config
    int32_t wave_last_raw;  // State
} Comparator_t;

// --- Math (Derivative / Integral) / Slew Rate Limiter ---
typedef struct {
    int32_t scale_fract_q15; // Config
    int32_t scale_shift;     // Config (Derivative/Integral only)
    int32_t last_q15;        // State
} Math_t;

// --- Clipper ---
typedef struct {
    int32_t upper;      // Config
    int32_t lower;      // Config
} Clipper_t;

// --- Envelope Follower ---
typedef struct {
    int32_t decay_q15;  // Config
    uint8_t polarity;   // Config
    int32_t last_q15;   // State
} Envelope_t;

// --- Full Wave Rectifier ---
typedef struct {
    int32_t vref_raw;   // Config
} Rectifier_t;

// --- Sample and Hold ---
typedef struct {
    uint32_t period_samples; // Config
    uint32_t track_samples;  // Config
    uint32_t counter;        // State
    int32_t  last_q15;       // State
} SampleHold_t;

// --- Digital Delay ---
typedef struct {
    uint32_t delay_samples;   // Config
    uint32_t delay_write_ptr; // State
} Delay_t;

// =========================================================
// Core Context: Merging all independent mode states into a single union
// =========================================================
typedef union {
    Filter1st_t  filter1st;
    Biquad_t     bq;
    Pga_t        pga;
    Comparator_t comp;
    Math_t       math;
    Clipper_t    clipper;
    Envelope_t   envelope;
    Rectifier_t  rect;
    SampleHold_t sh;
    Delay_t      delay;
} DSPContext_u;

// Exactly one instance of this context is allocated in RAM.
// The total footprint is fixed to the size of the largest member (Biquad_t = 28 bytes).
static DSPContext_u dsp_ctx;

// =========================================================================
// Static State Variables (すべてネイティブ32bit型に変更して最速化)
// =========================================================================
// mask = 2^N-1, shift = N
// mask = 0, shift = 0 : Native 192kHz mode (Decimation OFF)
// mask = 1023, shift = 10 : 1024x Decimation (187.5Hz mode)
static uint32_t decimation_mask = 0;
static uint32_t decimation_shift = 0;
static uint32_t adc_accumulator = 0;
static uint32_t decimation_count = 0;

// DSPの結果を扱う変数も、最初から符号付き32bit (int32_t) にしておく
// これにより引き算やシフト演算時のキャストが一切不要になる
static int32_t  y_old = MIMIC_ADC_MID_VALUE;
static int32_t  y_new = MIMIC_ADC_MID_VALUE;
static int32_t  delta_step_q16 = 0;
static int32_t  current_dac_val_q16 = (MIMIC_ADC_MID_VALUE << 16);

// =========================================================
// Utilities & I2C Parameter Parsers (32-bit Safe)
// =========================================================

static inline __attribute__((always_inline)) int32_t RawToQ15(int32_t raw) {
    return (raw - MIMIC_ADC_MID_VALUE) << Q15_SHIFT;
}

static inline __attribute__((always_inline)) int32_t Q15ToRaw(int32_t q15) {
    return (q15 >> Q15_SHIFT) + MIMIC_ADC_MID_VALUE;
}

static inline __attribute__((always_inline)) int32_t InterpolateLUT(const int32_t *lut, int32_t adc_val) {
    int32_t index = adc_val >> LUT_INDEX_SHIFT;
    int32_t frac = adc_val & LUT_FRAC_MASK;
    int32_t y0 = lut[index];
    int32_t y1 = lut[index + 1];
    int32_t interpolated = y0 + (((y1 - y0) * frac) >> LUT_INDEX_SHIFT);
    return Q15ToRaw(interpolated);
}

// =========================================================
// 2. DSP Core Functions (Returns int32_t, Without Internal Clipping)
// =========================================================

static inline __attribute__((always_inline)) int32_t ProcessFilterLpf(int32_t adc_val) {
    Filter1st_t *p = (Filter1st_t *)&dsp_ctx.filter1st;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t state_q15 = p->accum >> FRACTIONAL_BITS_Q15;
    p->accum += (in_q15 - state_q15) * p->alpha_q15;
    return Q15ToRaw(p->accum >> FRACTIONAL_BITS_Q15);
}

static inline __attribute__((always_inline)) int32_t ProcessFilterHpf(int32_t adc_val) {
    Filter1st_t *p = (Filter1st_t *)&dsp_ctx.filter1st;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t state_q15 = p->accum >> FRACTIONAL_BITS_Q15;
    p->accum += (in_q15 - state_q15) * p->alpha_q15;
    return Q15ToRaw(in_q15 - (p->accum >> FRACTIONAL_BITS_Q15));
}

static inline __attribute__((always_inline)) int32_t ProcessBiquadFilter(int32_t adc_val) {
    Biquad_t *p = (Biquad_t *)&dsp_ctx.bq;

    int32_t x0 = adc_val - MIMIC_ADC_MID_VALUE;
    
    // 1. Calculate y0
    int32_t acc = p->b0 * x0 + p->s1;
    int32_t y0 = (acc + BIQUAD_ROUND_VAL) >> BIQUAD_SHIFT;
    
    // 2. Calculate error and reuse register
    // Since 'acc' is no longer needed for its original purpose, we reuse it 
    // directly as 'err' to save one CPU register allocation.
    acc -= (y0 << BIQUAD_SHIFT); 

    // 3. Compute the next s1 state sequentially
    // Instead of using a single complex expression, operations are serialized to guide 
    // the compiler to consume and release registers efficiently, avoiding simultaneous 
    // loads of b1, a1, and s2.
    acc += p->s2;
    acc += p->b1 * x0;
    acc += p->a1 * y0;
    p->s1 = acc; // Write back to memory immediately upon calculation completion

    // 4. Compute the next s2 state
    // Registers are fully freed up now that s1 has been written back to memory.
    p->s2 = p->b2 * x0 + p->a2 * y0;

    return y0 + MIMIC_ADC_MID_VALUE; 
}

static inline __attribute__((always_inline)) int32_t ProcessPga(int32_t adc_val) {
    Pga_t *p = (Pga_t *)&dsp_ctx.pga;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t signal = in_q15 - p->offset_q15;
    int32_t out_q15 = (signal * p->gain_fract_q15) >> FRACTIONAL_BITS_Q15;
    
    if (p->gain_shift > 0) out_q15 <<= p->gain_shift;
    return Q15ToRaw(out_q15 + p->offset_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessPrsWindow(int32_t adc_val) {
    Comparator_t *p = (Comparator_t *)&dsp_ctx.comp;

    if (adc_val > p->upper || adc_val < p->lower) return MIMIC_ADC_MAX_VALUE;
    return MIMIC_ADC_MIN_VALUE;
}

static inline __attribute__((always_inline)) int32_t ProcessComparatorSchmitt(int32_t adc_val) {
    Comparator_t *p = (Comparator_t *)&dsp_ctx.comp;
    
    if (adc_val > p->upper) p->wave_last_raw = MIMIC_ADC_MAX_VALUE;
    else if (adc_val < p->lower) p->wave_last_raw = MIMIC_ADC_MIN_VALUE;
    
    return p->wave_last_raw;
}

static inline __attribute__((always_inline)) int32_t ProcessEnvelopeFollower(int32_t adc_val) {
    Envelope_t *p = (Envelope_t *)&dsp_ctx.envelope;

    int32_t in_q15 = RawToQ15(adc_val);
    if (p->polarity == MIMIC_POLALYTY_NEGATIVE) in_q15 = -in_q15;
    if (in_q15 < 0) in_q15 = 0;

    if (in_q15 > p->last_q15) {
        p->last_q15 = in_q15;
    } else {
        p->last_q15 = (p->last_q15 * p->decay_q15) >> FRACTIONAL_BITS_Q15;
    }
    return Q15ToRaw(p->last_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessClipper(int32_t adc_val) {
    Clipper_t *p = (Clipper_t *)&dsp_ctx.clipper;

    if (adc_val > p->upper) return p->upper;
    if (adc_val < p->lower) return p->lower;
    return adc_val; 
}

static inline __attribute__((always_inline)) int32_t ProcessNonlinearLut(int32_t adc_val) {
    // 1. Direct pointer loading eliminates standard if-else branching penalties
    const int32_t *lut = active_lut_ptr;

    // 2. Fragment calculation
    int32_t index = adc_val >> LUT_INDEX_SHIFT;
    int32_t frac = adc_val & LUT_FRAC_MASK;

    // 3. Optimize memory access utilizing explicit compiler offset addressing
    const int32_t *p = &lut[index];
    int32_t y0 = p[0];
    int32_t y1 = p[1];

    // 4. Interpolate leveraging Cortex-M0+ single-cycle multiplier (MULS)
    int32_t interpolated = y0 + (((y1 - y0) * frac) >> LUT_INDEX_SHIFT);
    
    return Q15ToRaw(interpolated);
}

static inline __attribute__((always_inline)) int32_t ProcessMathDerivative(int32_t adc_val) {
    Math_t *p = (Math_t *)&dsp_ctx.math;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t diff = in_q15 - p->last_q15;
    p->last_q15 = in_q15;

    // 1. 差分に対するスケール適用 (仮数部の乗算)
    int32_t diff_mult = (diff * p->scale_fract_q15) >> FRACTIONAL_BITS_Q15;
    
    // 2. 最終ゲインの適用 (指数部のシフト)
    int32_t s = p->scale_shift;
    int32_t out_q15 = (s >= 0) ? (diff_mult << s) : (diff_mult >> -s);
    
    return Q15ToRaw(out_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessMathIntegral(int32_t adc_val) {
    Math_t *p = (Math_t *)&dsp_ctx.math;

    int32_t in_q15 = RawToQ15(adc_val);

    // 1. 履歴の減衰 (仮数部の乗算)
    int32_t history_leaked = (p->last_q15 * p->scale_fract_q15) >> FRACTIONAL_BITS_Q15;

    // 2. 入力スケーリング (指数部のシフト)
    // 三項演算子を使うことで、コンパイラが「if文」ではなく
    // パイプラインフラッシュの起きにくい「条件付き実行命令」に最適化しやすくなります
    int32_t s = p->scale_shift;
    int32_t input_scaled = (s >= 0) ? (in_q15 << s) : (in_q15 >> -s);

    // 3. 累積と出力
    p->last_q15 = history_leaked + input_scaled;
    return Q15ToRaw(p->last_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessSlewRateLimiter(int32_t adc_val) {
    Math_t *p = (Math_t *)&dsp_ctx.math;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t diff = in_q15 - p->last_q15;

    // Mathematical clamping to suppress standard runtime branch penalties.
    // Limits the rate of change (diff) strictly within the bounds of the threshold (p->scale_fract_q15).
    int32_t clamped_diff = diff;
    if (clamped_diff > p->scale_fract_q15)  clamped_diff = p->scale_fract_q15;
    if (clamped_diff < -p->scale_fract_q15) clamped_diff = -p->scale_fract_q15;
    
    // Modern GCC compilers automatically translate these distinct conditional statements 
    // into branchless assembly instructions (e.g., conditional execution or selection loops), 
    // completely avoiding heavy pipeline-flushing jumps on architectures like Cortex-M0+.

    // Safely advance the accumulation historical state using the rate-limited delta
    p->last_q15 += clamped_diff;

    return Q15ToRaw(p->last_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessRectifierFull(int32_t adc_val) {
    Rectifier_t *p = (Rectifier_t *)&dsp_ctx.rect;

    int32_t diff = adc_val - p->vref_raw;
    if (diff < 0) diff = -diff;
    return p->vref_raw + diff;
}

static inline __attribute__((always_inline)) int32_t ProcessSampleAndHold(int32_t adc_val) {
    SampleHold_t *p = (SampleHold_t *)&dsp_ctx.sh;

    p->counter++;
    if (p->counter >= p->period_samples) p->counter = 0;
    if (p->counter < p->track_samples) p->last_q15 = RawToQ15(adc_val);
    return Q15ToRaw(p->last_q15);
}

// Dedicated Mode Processing Function for Digital Delay
static inline __attribute__((always_inline)) int32_t ProcessDelay(int32_t adc_val) {
    Delay_t *p = (Delay_t *)&dsp_ctx.delay;

    delay_buffer[p->delay_write_ptr] = adc_val;
    uint32_t read_ptr = (p->delay_write_ptr - p->delay_samples) & DELAY_BUFFER_MASK;
    int32_t out_val = delay_buffer[read_ptr];
    
    p->delay_write_ptr = (p->delay_write_ptr + 1) & DELAY_BUFFER_MASK;
    return out_val;
}

// =========================================================
// Initialization and Parameter Updates
// =========================================================
void MimicDSP_Init(MimicCallback_t enable_output, MimicCallback_t disable_output) {
    cb_enable_output = enable_output;
    cb_disable_output = disable_output;

    pipeline.current_mode = MIMIC_MODE_ID_BYPASS_DSP;
    pipeline.global_flags = 0;
    pipeline.out_mult_q15 = 1 << 15;
    pipeline.out_offset_calc = 0;

    memset((void*)&dsp_ctx, 0, sizeof(dsp_ctx));
    dsp_ctx.comp.wave_last_raw = MIMIC_ADC_MID_VALUE;
}

void MimicDSP_UpdateParameters(void) {
    uint8_t previous_mode = pipeline.current_mode;

    pipeline.global_flags = MimicDevice_ReadRegU8(MIMIC_REG_GLOBAL_FLAGS);
    pipeline.current_mode = MimicDevice_ReadRegU8(MIMIC_REG_MODE_ID);

    // --- Global Parameters ---
    MimicDSP_SetDecimation(MimicDevice_ReadRegU8(MIMIC_REG_GLOBAL_DECIMATION_N));
    int32_t global_gain_q15 = (int32_t)MimicDevice_ReadRegU16(MIMIC_REG_NVM_GAIN_Q15);
    int32_t global_output_shift_raw = MimicDevice_ReadRegS16(MIMIC_REG_GLOBAL_OFFSET) + MimicDevice_ReadRegS16(MIMIC_REG_NVM_OFFSET);

    // Pre-calculate inversion and offsets
    if (pipeline.global_flags & MIMIC_FLAG_INV_OUT) {
        pipeline.out_mult_q15 = -global_gain_q15;
        pipeline.out_offset_calc = MIMIC_ADC_MAX_VALUE - global_output_shift_raw; 
    } else {
        pipeline.out_mult_q15 = global_gain_q15; 
        pipeline.out_offset_calc = global_output_shift_raw;
    }

    // Completely reset the memory region (parameters + states) at once when the mode is switched.
    if (pipeline.current_mode != previous_mode) {
        memset((void*)&dsp_ctx, 0, sizeof(dsp_ctx));
        // Exception handling: explicitly set default values for fields requiring non-zero initialization
        dsp_ctx.comp.wave_last_raw = MIMIC_ADC_MID_VALUE;
    }

    // --- Mode Specific Parameters (Successfully Unified into dsp_ctx) ---
    switch (pipeline.current_mode) {
        case MIMIC_MODE_ID_FILTER_BIQUAD:
            dsp_ctx.bq.b0 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            dsp_ctx.bq.b1 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 2);
            dsp_ctx.bq.b2 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 4);
            dsp_ctx.bq.a1 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 6);
            dsp_ctx.bq.a2 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 8);
            break;

        case MIMIC_MODE_ID_FILTER_1ST_LPF:
        case MIMIC_MODE_ID_FILTER_1ST_HPF:
            dsp_ctx.filter1st.alpha_q15 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            break;

        case MIMIC_MODE_ID_PGA:
            dsp_ctx.pga.gain_fract_q15 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            dsp_ctx.pga.gain_shift = MimicDevice_ReadRegS8(MIMIC_REG_MODE_PAYLOAD_START + 2);
            dsp_ctx.pga.offset_q15 = RawToQ15(MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 3));
            break;

        case MIMIC_MODE_ID_COMPARATOR_WIN:
        case MIMIC_MODE_ID_COMPARATOR_SCHMITT:
            dsp_ctx.comp.upper = MimicDevice_ReadRegU16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            dsp_ctx.comp.lower = MimicDevice_ReadRegU16(MIMIC_REG_MODE_PAYLOAD_START + 2);
            break;

        case MIMIC_MODE_ID_MATH_DERIVATIVE:
        case MIMIC_MODE_ID_MATH_INTEGRAL:
            dsp_ctx.math.scale_fract_q15 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            dsp_ctx.math.scale_shift = MimicDevice_ReadRegS8(MIMIC_REG_MODE_PAYLOAD_START + 2);
            break;

        case MIMIC_MODE_ID_SLEW_RATE_LIMITER:
            // Slew Rate Limiter smartly reuses the Math structure for its parameter layout
            dsp_ctx.math.scale_fract_q15 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            break;

        case MIMIC_MODE_ID_CLIPPER:
            dsp_ctx.clipper.upper = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            dsp_ctx.clipper.lower = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 2);
            break;

        case MIMIC_MODE_ID_NONLINEAR_LUT:
            nonlinear_lut_type = MimicDevice_ReadRegS8(MIMIC_REG_MODE_PAYLOAD_START + 0);
            switch (nonlinear_lut_type) {
                case MIMIC_LUT_LOG: active_lut_ptr = lut_log; break;
                case MIMIC_LUT_ANTILOG: active_lut_ptr = lut_antilog; break;
            }
            break;

        case MIMIC_MODE_ID_ENVELOPE_FOLLOWER:
            dsp_ctx.envelope.decay_q15 = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            dsp_ctx.envelope.polarity = MimicDevice_ReadRegU8(MIMIC_REG_MODE_PAYLOAD_START + 2);
            break;

        case MIMIC_MODE_ID_RECTIFIER_FULL:
            dsp_ctx.rect.vref_raw = MimicDevice_ReadRegS16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            break;

        case MIMIC_MODE_ID_SAMPLE_AND_HOLD:
            dsp_ctx.sh.period_samples = MimicDevice_ReadRegU16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            dsp_ctx.sh.track_samples = MimicDevice_ReadRegU16(MIMIC_REG_MODE_PAYLOAD_START + 2);
            break;

        case MIMIC_MODE_ID_DELAY:
            dsp_ctx.delay.delay_samples = MimicDevice_ReadRegU16(MIMIC_REG_MODE_PAYLOAD_START + 0);
            if (dsp_ctx.delay.delay_samples > DELAY_BUFFER_MASK)
                dsp_ctx.delay.delay_samples = DELAY_BUFFER_MASK;
    }
}

// =========================================================
// 3. The Main DSP Pipeline 
// =========================================================
__attribute__((always_inline)) static inline uint16_t MimicDSP_ProcessSample_RAM(uint16_t adc_val) {
    // Cast to remove volatile qualifier, allowing the compiler to perform full register optimization.
    MimicPipeline_t *pipe = (MimicPipeline_t *)&pipeline;

    int32_t dsp_out = 0;

    // 2. Mode-Specific DSP Core Engine
    if (pipe->current_mode == MIMIC_MODE_ID_FILTER_BIQUAD) {
        dsp_out = ProcessBiquadFilter(adc_val);
    } else {
        switch (pipe->current_mode) {
            case MIMIC_MODE_ID_PGA:                 dsp_out = ProcessPga(adc_val); break;
            case MIMIC_MODE_ID_FILTER_1ST_LPF:      dsp_out = ProcessFilterLpf(adc_val); break;
            case MIMIC_MODE_ID_FILTER_1ST_HPF:      dsp_out = ProcessFilterHpf(adc_val); break;
            case MIMIC_MODE_ID_FILTER_BIQUAD:       dsp_out = ProcessBiquadFilter(adc_val); break;
            case MIMIC_MODE_ID_COMPARATOR_WIN:      dsp_out = ProcessPrsWindow(adc_val); break;
            case MIMIC_MODE_ID_COMPARATOR_SCHMITT:  dsp_out = ProcessComparatorSchmitt(adc_val); break;
            case MIMIC_MODE_ID_ENVELOPE_FOLLOWER:   dsp_out = ProcessEnvelopeFollower(adc_val); break;
            case MIMIC_MODE_ID_CLIPPER:             dsp_out = ProcessClipper(adc_val); break;
            case MIMIC_MODE_ID_NONLINEAR_LUT:       dsp_out = ProcessNonlinearLut(adc_val); break;
            case MIMIC_MODE_ID_MATH_DERIVATIVE:     dsp_out = ProcessMathDerivative(adc_val); break;
            case MIMIC_MODE_ID_MATH_INTEGRAL:       dsp_out = ProcessMathIntegral(adc_val); break;
            case MIMIC_MODE_ID_RECTIFIER_FULL:      dsp_out = ProcessRectifierFull(adc_val); break;
            case MIMIC_MODE_ID_SLEW_RATE_LIMITER:   dsp_out = ProcessSlewRateLimiter(adc_val); break;
            case MIMIC_MODE_ID_SAMPLE_AND_HOLD:     dsp_out = ProcessSampleAndHold(adc_val); break;
            case MIMIC_MODE_ID_DELAY:               dsp_out = ProcessDelay(adc_val); break;
            default:                                dsp_out = adc_val; break;
        }
    }

    // 3. Post-Processing: Branchless inversion and dynamic DC Offset insertion
    int32_t final_out = ((dsp_out * pipe->out_mult_q15) >> 15) + pipe->out_offset_calc;

    // 4. Branchless fixed-point saturation logic (Clips dynamically to 0 - 4095)
    if ((uint32_t)final_out > MIMIC_ADC_MAX_VALUE) {
        final_out = (~(final_out >> 31)) & MIMIC_ADC_MAX_VALUE;
    }

    return (uint16_t)final_out;
}

void MimicDSP_SetDecimation(uint8_t N) {
    decimation_mask = (1U << N) - 1;
    decimation_shift = N;

    // 2. DSPの内部状態（バッファとスロープ）を完全に安全にリセット
    adc_accumulator = 0;
    decimation_count = 0;
    
    y_old = MIMIC_ADC_MID_VALUE;
    y_new = MIMIC_ADC_MID_VALUE;
    delta_step_q16 = 0;
    current_dac_val_q16 = (MIMIC_ADC_MID_VALUE << 16);
}

// Configuration for Test and Debug Features
#define ENABLE_DSP_PROFILING

/**
 * @brief ADC and Comparator Interrupt Handler
 */
__attribute__((section(".ramfunc")))
void ADC_COMP_IRQHandler(void) {
    // =========================================================================
    // 1. [CRITICAL] DAC Output Trigger (Guarantees absolute time determinism)
    // Fire immediately upon ISR entry without any branching or variable loads
    // to minimize jitter. Bypasses 'hdac1.Instance' pointer redirection by
    // accessing the DAC macro definition directly.
    // =========================================================================
    DAC1->SWTRIGR = 1; 

#ifdef ENABLE_DSP_PROFILING
    uint32_t start_tick = SysTick->VAL;
#endif

    // =========================================================================
    // 2. ADC Data Read
    // Reading the DR register automatically clears the EOC flag in hardware.
    // Condition checks on SR are omitted since EOC is the sole active source.
    // =========================================================================
    uint32_t raw_val = ADC1->DR;

    // =========================================================================
    // 3. Signal Saturation Detection
    // =========================================================================
    if (raw_val == MIMIC_ADC_MIN_VALUE || raw_val >= MIMIC_ADC_MAX_VALUE) {
        MimicDevice_SetErrorFlag_ISR(MIMIC_STATUS_SIGNAL_SATURATION);
    }

    if (decimation_mask == 0) {
        MimicDevice_SetAdcVal_ISR(raw_val);

        // =========================================================================
        // 4. DSP Processing & Preloading DAC Holding Register for the Next Cycle
        // =========================================================================
        DAC1->DHR12R1 = MimicDSP_ProcessSample_RAM(raw_val);
    } else {
        // [DECIMATION & INTERPOLATION PATH]
        // 32bitネイティブ演算 (キャスト不要、最速)
        adc_accumulator += raw_val;
        decimation_count++;

        current_dac_val_q16 += delta_step_q16;
        DAC1->DHR12R1 = current_dac_val_q16 >> 16; 

        if ((decimation_count & decimation_mask) == 0) {
            
            // シフト結果も32bitで保持
            uint32_t x_in = adc_accumulator >> decimation_shift;
            MimicDevice_SetAdcVal_ISR(x_in); 

            y_old = y_new;
            y_new = MimicDSP_ProcessSample_RAM(x_in);

            // 【超絶最適化ポイント】
            // 互いにint32_tなのでキャストは一切不要！
            // C言語のコンパイラは純粋な「SUB」命令と「LSL」命令だけを吐き出す
            int32_t diff_q16 = (y_new - y_old) << 16;
            delta_step_q16 = diff_q16 >> decimation_shift;

            current_dac_val_q16 = y_old << 16;
            
            adc_accumulator = 0;
        }
    }

    // =========================================================================
    // 5. Exception (Overrun) Checking 
    // Defer to the end of the ISR as this condition occurs with extreme rarity.
    // =========================================================================
    if (ADC1->SR & ADC_SR_OVER) {
        MimicDevice_SetErrorFlag_ISR(MIMIC_STATUS_ADC_OVER_ERR);
        // Clear flag directly via peripheral register instead of HAL macro
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

void MimicDSP_ProcessPendingTasks(void) {
    uint8_t cmd = MimicDevice_PopSystemCommand();
    if (cmd != MIMIC_CMD_NOP) {
        switch (cmd) {
            case MIMIC_CMD_NVM_COMMIT:
                MimicFlash_WriteGainQ15Data(MimicDevice_ReadRegU16(MIMIC_REG_NVM_GAIN_Q15));
                MimicFlash_WriteOffsetData(MimicDevice_ReadRegS16(MIMIC_REG_NVM_OFFSET));
                break;
            case MIMIC_CMD_NVM_RELOAD:
                MimicDevice_LoadCalibration();
                break;
        }
    }

    // 1. Minimum necessary background processing
    if (MimicDevice_PopUpdateFlag()) {
        HAL_NVIC_DisableIRQ(ADC_COMP_IRQn);
        cb_disable_output();

        MimicDSP_UpdateParameters();
        // Reset cpu cycles count
        MimicDevice_PopCpuCyclesMax();
        if ((pipeline.global_flags & MIMIC_FLAG_OUT_OPEN) == 0) {
            cb_enable_output();
        }
        HAL_NVIC_EnableIRQ(ADC_COMP_IRQn);
    }
}
