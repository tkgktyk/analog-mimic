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
#include "mimic0x_msp.h"
#include <string.h>

// =========================================================
// Deterministic Output Slot (Shared between ISR and DMA)
// =========================================================
static volatile uint32_t mimic_output_slot = MIMIC_ADC_MID_VALUE;

// =========================================================
// Fixed-Point Arithmetic Configurations
// =========================================================
#define Q15_SHIFT           4  // For 12-bit ADC (12 + 4 = 16 bits)
#define FRACTIONAL_BITS_Q15 15 // Q15 fractional bits for 32-bit shifting
#define Q16_SHIFT           16 // Q16 fractional bits for decimation math

// =========================================================
// DSP Specific Constants
// =========================================================
#define LUT_INDEX_SHIFT     6
#define LUT_FRAC_MASK       0x3F
#define BIQUAD_ROUND_VAL    8192
#define BIQUAD_SHIFT        14

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
    13900, 14000, 14100, 14200, 14300
};

static const int32_t lut_antilog[65] = {
    -32768, -31768, -30000, -27000, -23000, -18000, -12000, -5000, 2000, 8000,
    13000, 17000, 20000, 22500, 24500, 26000, 27300, 28400, 29300, 30000,
    30600, 31100, 31500, 31800, 32100, 32300, 32500, 32600, 32700, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    32767, 32767, 32767, 32767, 32767
};

// =========================================================
// 1. DSP Internal State Variables (Exclusive to Sampling ISR)
// =========================================================
// Note: 'volatile' is omitted on internal DSP states because the sampling engine
// is strictly stopped before MimicDSP_UpdateParameters() modifies them from Main.

typedef struct {
    int32_t            out_mult_q15;    // Signed Q15 multiplier
    int32_t            out_offset_calc; // Final computed offset including inversion
    uint8_t            current_mode;
    MimicOutputRoute_t output_route;
    uint8_t            global_flags;
} MimicPipeline_t;

static MimicPipeline_t pipeline;

#define DELAY_BUFFER_SIZE 4096
#define DELAY_BUFFER_MASK (DELAY_BUFFER_SIZE - 1)
static uint16_t delay_buffer[DELAY_BUFFER_SIZE] = {0};

static uint8_t nonlinear_lut_type = MIMIC_LUT_LOG;
static const int32_t *active_lut_ptr = lut_log;

// =========================================================
// Complete DSP Mode-Specific Contexts (Unified Structs)
// =========================================================

typedef struct {
    int32_t alpha_q15;       // Config
    int32_t accum;           // State
} Filter1st_t;

typedef struct {
    int32_t b0, b1, b2;      // Config
    int32_t a1, a2;          // Config
    int32_t s1, s2;          // State
} Biquad_t;

typedef struct {
    int32_t gain_fract_q15;  // Config
    int32_t gain_shift;      // Config
    int32_t offset_q15;      // Config
} Pga_t;

typedef struct {
    int32_t upper;           // Config
    int32_t lower;           // Config
    int32_t wave_last_raw;   // State
} Comparator_t;

typedef struct {
    int32_t scale_fract_q15; // Config
    int32_t scale_shift;     // Config (Derivative/Integral only)
    int32_t last_q15;        // State
} Math_t;

typedef struct {
    int32_t upper;           // Config
    int32_t lower;           // Config
} Clipper_t;

typedef struct {
    int32_t decay_q15;       // Config
    uint8_t polarity;        // Config
    int32_t last_q15;        // State
} Envelope_t;

typedef struct {
    int32_t vref_raw;        // Config
} Rectifier_t;

typedef struct {
    uint32_t period_samples; // Config
    uint32_t track_samples;  // Config
    uint32_t counter;        // State
    int32_t  last_q15;       // State
} SampleHold_t;

typedef struct {
    uint32_t delay_samples;   // Config
    uint32_t delay_write_ptr; // State
} Delay_t;

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

static DSPContext_u dsp_ctx;

// =========================================================================
// Decimation & Interpolation State Variables (Promoted to native 32-bit)
// =========================================================================
static uint32_t decimation_mask     = 0;
static uint32_t decimation_shift    = 0;
static uint32_t adc_accumulator     = 0;
static uint32_t decimation_count    = 0;

static int32_t  y_old               = MIMIC_ADC_MID_VALUE;
static int32_t  y_new               = MIMIC_ADC_MID_VALUE;
static int32_t  delta_step_q16      = 0;
static int32_t  current_dac_val_q16 = (MIMIC_ADC_MID_VALUE << Q16_SHIFT);

// =========================================================
// Utilities & I2C Parameter Parsers (32-bit Safe)
// =========================================================

static inline __attribute__((always_inline)) int32_t RawToQ15(int32_t raw) {
    return (raw - MIMIC_ADC_MID_VALUE) << Q15_SHIFT;
}

static inline __attribute__((always_inline)) int32_t Q15ToRaw(int32_t q15) {
    return (q15 >> Q15_SHIFT) + MIMIC_ADC_MID_VALUE;
}

static inline __attribute__((always_inline)) uint32_t MuxDacGpio(uint32_t result) {
    return (pipeline.output_route == MIMIC_OUT_DIGITAL)
         ? ((uint32_t)OPA2_INN_PIN << (result >= MIMIC_ADC_MID_VALUE ? 0u : 16u))
         : result;
}

// =========================================================
// 2. DSP Core Functions (Returns int32_t, Without Internal Clipping)
// =========================================================

static inline __attribute__((always_inline)) int32_t ProcessFilterLpf(int32_t adc_val) {
    Filter1st_t *p = &dsp_ctx.filter1st;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t state_q15 = p->accum >> FRACTIONAL_BITS_Q15;
    p->accum += (in_q15 - state_q15) * p->alpha_q15;
    return Q15ToRaw(p->accum >> FRACTIONAL_BITS_Q15);
}

static inline __attribute__((always_inline)) int32_t ProcessFilterHpf(int32_t adc_val) {
    Filter1st_t *p = &dsp_ctx.filter1st;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t state_q15 = p->accum >> FRACTIONAL_BITS_Q15;
    p->accum += (in_q15 - state_q15) * p->alpha_q15;
    return Q15ToRaw(in_q15 - (p->accum >> FRACTIONAL_BITS_Q15));
}

static inline __attribute__((always_inline)) int32_t ProcessBiquadFilter(int32_t adc_val) {
    Biquad_t *p = &dsp_ctx.bq;

    int32_t x0 = adc_val - MIMIC_ADC_MID_VALUE;
    
    int32_t acc = p->b0 * x0 + p->s1;
    int32_t y0 = (acc + BIQUAD_ROUND_VAL) >> BIQUAD_SHIFT;
    
    acc -= (y0 << BIQUAD_SHIFT); 
    acc += p->s2;
    acc += p->b1 * x0;
    acc += p->a1 * y0;
    p->s1 = acc;

    p->s2 = p->b2 * x0 + p->a2 * y0;

    return y0 + MIMIC_ADC_MID_VALUE; 
}

static inline __attribute__((always_inline)) int32_t ProcessPga(int32_t adc_val) {
    Pga_t *p = &dsp_ctx.pga;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t signal = in_q15 - p->offset_q15;
    int32_t out_q15 = (signal * p->gain_fract_q15) >> FRACTIONAL_BITS_Q15;
    
    if (p->gain_shift > 0) out_q15 <<= p->gain_shift;
    return Q15ToRaw(out_q15 + p->offset_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessPrsWindow(int32_t adc_val) {
    Comparator_t *p = &dsp_ctx.comp;

    if (adc_val > p->upper || adc_val < p->lower) return MIMIC_ADC_MAX_VALUE;
    return MIMIC_ADC_MIN_VALUE;
}

static inline __attribute__((always_inline)) int32_t ProcessComparatorSchmitt(int32_t adc_val) {
    Comparator_t *p = &dsp_ctx.comp;
    
    if (adc_val > p->upper) p->wave_last_raw = MIMIC_ADC_MAX_VALUE;
    else if (adc_val < p->lower) p->wave_last_raw = MIMIC_ADC_MIN_VALUE;
    
    return p->wave_last_raw;
}

static inline __attribute__((always_inline)) int32_t ProcessEnvelopeFollower(int32_t adc_val) {
    Envelope_t *p = &dsp_ctx.envelope;

    int32_t in_q15 = RawToQ15(adc_val);
    if (p->polarity == MIMIC_POLARITY_NEGATIVE) in_q15 = -in_q15;
    if (in_q15 < 0) in_q15 = 0;

    if (in_q15 > p->last_q15) {
        p->last_q15 = in_q15;
    } else {
        p->last_q15 = (p->last_q15 * p->decay_q15) >> FRACTIONAL_BITS_Q15;
    }
    return Q15ToRaw(p->last_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessClipper(int32_t adc_val) {
    Clipper_t *p = &dsp_ctx.clipper;

    if (adc_val > p->upper) return p->upper;
    if (adc_val < p->lower) return p->lower;
    return adc_val; 
}

static inline __attribute__((always_inline)) int32_t ProcessNonlinearLut(int32_t adc_val) {
    const int32_t *lut = active_lut_ptr;

    int32_t index = adc_val >> LUT_INDEX_SHIFT;
    int32_t frac = adc_val & LUT_FRAC_MASK;

    const int32_t *p = &lut[index];
    int32_t y0 = p[0];
    int32_t y1 = p[1];

    int32_t interpolated = y0 + (((y1 - y0) * frac) >> LUT_INDEX_SHIFT);
    return Q15ToRaw(interpolated);
}

static inline __attribute__((always_inline)) int32_t ProcessMathDerivative(int32_t adc_val) {
    Math_t *p = &dsp_ctx.math;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t diff = in_q15 - p->last_q15;
    p->last_q15 = in_q15;

    int32_t diff_mult = (diff * p->scale_fract_q15) >> FRACTIONAL_BITS_Q15;
    int32_t s = p->scale_shift;
    int32_t out_q15 = (s >= 0) ? (diff_mult << s) : (diff_mult >> -s);
    
    return Q15ToRaw(out_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessMathIntegral(int32_t adc_val) {
    Math_t *p = &dsp_ctx.math;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t history_leaked = (p->last_q15 * p->scale_fract_q15) >> FRACTIONAL_BITS_Q15;

    int32_t s = p->scale_shift;
    int32_t input_scaled = (s >= 0) ? (in_q15 << s) : (in_q15 >> -s);

    p->last_q15 = history_leaked + input_scaled;
    return Q15ToRaw(p->last_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessSlewRateLimiter(int32_t adc_val) {
    Math_t *p = &dsp_ctx.math;

    int32_t in_q15 = RawToQ15(adc_val);
    int32_t diff = in_q15 - p->last_q15;

    int32_t clamped_diff = diff;
    if (clamped_diff > p->scale_fract_q15)  clamped_diff = p->scale_fract_q15;
    if (clamped_diff < -p->scale_fract_q15) clamped_diff = -p->scale_fract_q15;
    
    p->last_q15 += clamped_diff;
    return Q15ToRaw(p->last_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessRectifierFull(int32_t adc_val) {
    Rectifier_t *p = &dsp_ctx.rect;

    int32_t diff = adc_val - p->vref_raw;
    if (diff < 0) diff = -diff;
    return p->vref_raw + diff;
}

static inline __attribute__((always_inline)) int32_t ProcessSampleAndHold(int32_t adc_val) {
    SampleHold_t *p = &dsp_ctx.sh;

    p->counter++;
    if (p->counter >= p->period_samples) p->counter = 0;
    if (p->counter < p->track_samples) p->last_q15 = RawToQ15(adc_val);
    return Q15ToRaw(p->last_q15);
}

static inline __attribute__((always_inline)) int32_t ProcessDelay(int32_t adc_val) {
    Delay_t *p = &dsp_ctx.delay;

    delay_buffer[p->delay_write_ptr] = adc_val;
    uint32_t read_ptr = (p->delay_write_ptr - p->delay_samples) & DELAY_BUFFER_MASK;
    int32_t out_val = delay_buffer[read_ptr];
    
    p->delay_write_ptr = (p->delay_write_ptr + 1) & DELAY_BUFFER_MASK;
    return out_val;
}

// =========================================================
// Initialization and Parameter Updates
// =========================================================

void MimicDSP_Init(void) {
    pipeline.current_mode    = MIMIC_MODE_ID_BYPASS_DSP;
    pipeline.output_route    = MIMIC_OUT_OPEN;
    pipeline.global_flags    = 0;
    pipeline.out_mult_q15    = 1 << FRACTIONAL_BITS_Q15;
    pipeline.out_offset_calc = 0;

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

void MimicDSP_UpdateParameters(const MimicDSP_Config_t *config) {
    uint8_t previous_mode = pipeline.current_mode;

    pipeline.global_flags = config->global_flags;
    pipeline.current_mode = config->mode_id;
    pipeline.output_route = config->output_route;

    // 2. Global Decimation & Offset Parameters
    MimicDSP_SetDecimation(config->decimation_n);

    if (pipeline.global_flags & MIMIC_FLAG_INV_OUT) {
        pipeline.out_mult_q15    = -config->gain_q15;
        pipeline.out_offset_calc = MIMIC_ADC_MAX_VALUE - config->offset_raw; 
    } else {
        pipeline.out_mult_q15    = config->gain_q15; 
        pipeline.out_offset_calc = config->offset_raw;
    }

    // 3. Reset mode-specific state history upon mode transition
    if (pipeline.current_mode != previous_mode) {
        memset((void*)&dsp_ctx, 0, sizeof(dsp_ctx));
        dsp_ctx.comp.wave_last_raw = MIMIC_ADC_MID_VALUE;
    }

    // 4. Parse Mode-Specific Payload
    const uint8_t *p = config->payload;
    switch (pipeline.current_mode) {
        case MIMIC_MODE_ID_FILTER_BIQUAD:
            dsp_ctx.bq.b0 = ParseS16(p + 0);
            dsp_ctx.bq.b1 = ParseS16(p + 2);
            dsp_ctx.bq.b2 = ParseS16(p + 4);
            dsp_ctx.bq.a1 = ParseS16(p + 6);
            dsp_ctx.bq.a2 = ParseS16(p + 8);
            break;

        case MIMIC_MODE_ID_FILTER_1ST_LPF:
        case MIMIC_MODE_ID_FILTER_1ST_HPF:
            dsp_ctx.filter1st.alpha_q15 = ParseS16(p + 0);
            break;

        case MIMIC_MODE_ID_PGA:
            dsp_ctx.pga.gain_fract_q15 = ParseS16(p + 0);
            dsp_ctx.pga.gain_shift     = (int8_t)*(p + 2);
            dsp_ctx.pga.offset_q15     = RawToQ15(ParseS16(p + 3));
            break;

        case MIMIC_MODE_ID_COMPARATOR_WIN:
        case MIMIC_MODE_ID_COMPARATOR_SCHMITT:
            dsp_ctx.comp.upper = ParseU16(p + 0);
            dsp_ctx.comp.lower = ParseU16(p + 2);
            break;

        case MIMIC_MODE_ID_MATH_DERIVATIVE:
        case MIMIC_MODE_ID_MATH_INTEGRAL:
            dsp_ctx.math.scale_fract_q15 = ParseS16(p + 0);
            dsp_ctx.math.scale_shift     = (int8_t)*(p + 2);
            break;

        case MIMIC_MODE_ID_SLEW_RATE_LIMITER:
            dsp_ctx.math.scale_fract_q15 = ParseS16(p + 0);
            break;

        case MIMIC_MODE_ID_CLIPPER:
            dsp_ctx.clipper.upper = ParseS16(p + 0);
            dsp_ctx.clipper.lower = ParseS16(p + 2);
            break;

        case MIMIC_MODE_ID_NONLINEAR_LUT:
            nonlinear_lut_type = (uint8_t)*(p + 0);
            switch (nonlinear_lut_type) {
                case MIMIC_LUT_LOG:     active_lut_ptr = lut_log; break;
                case MIMIC_LUT_ANTILOG: active_lut_ptr = lut_antilog; break;
            }
            break;

        case MIMIC_MODE_ID_ENVELOPE_FOLLOWER:
            dsp_ctx.envelope.decay_q15 = ParseS16(p + 0);
            dsp_ctx.envelope.polarity  = (uint8_t)*(p + 2);
            break;

        case MIMIC_MODE_ID_RECTIFIER_FULL:
            dsp_ctx.rect.vref_raw = ParseS16(p + 0);
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
            case MIMIC_MODE_ID_FILTER_1ST_LPF:      dsp_out = ProcessFilterLpf(adc_val); break;
            case MIMIC_MODE_ID_FILTER_1ST_HPF:      dsp_out = ProcessFilterHpf(adc_val); break;
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

    // Post-Processing: Branchless inversion and dynamic DC Offset insertion
    int32_t final_out = ((dsp_out * pipe->out_mult_q15) >> FRACTIONAL_BITS_Q15) + pipe->out_offset_calc;

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
    uint32_t new_mask = (1U << decimation_n) - 1;

    if (new_mask == decimation_mask) {
        return;
    }

    decimation_mask  = new_mask;
    decimation_shift = decimation_n;

    adc_accumulator     = 0;
    decimation_count    = 0;
    y_old               = MIMIC_ADC_MID_VALUE;
    y_new               = MIMIC_ADC_MID_VALUE;
    delta_step_q16      = 0;
    current_dac_val_q16 = (MIMIC_ADC_MID_VALUE << Q16_SHIFT);
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
    } else {
        adc_accumulator += raw_val;
        decimation_count++;

        current_dac_val_q16 += delta_step_q16;
        out_raw = (uint32_t)(current_dac_val_q16 >> Q16_SHIFT);

        if ((decimation_count & decimation_mask) == 0) {
            uint32_t x_in = adc_accumulator >> decimation_shift;
            MimicDevice_SetAdcVal_ISR(x_in); 

            y_old = y_new;
            y_new = MimicDSP_ProcessSample(x_in);

            int32_t diff_q16 = (y_new - y_old) << Q16_SHIFT;
            delta_step_q16 = diff_q16 >> decimation_shift;

            current_dac_val_q16 = y_old << Q16_SHIFT;
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
