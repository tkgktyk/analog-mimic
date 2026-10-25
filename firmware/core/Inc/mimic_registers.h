/**
 ******************************************************************************
 * @file    mimic_registers.h
 * @author  TAKAGI Katsuyuki
 * @brief   Memory-Mapped Register Definitions, Volatile Configuration Layouts,
 * and Thread-Safe Virtual Telemetry Handlers.
 *-----------------------------------------------------------------------------
 * Copyright (C) 2026 TAKAGI Katsuyuki
 *
 * SPDX-License-Identifier: GPL-3.0-only OR MIT
 *
 * This file is shared between the firmware (GPL-3.0) and the Arduino library
 * (MIT), and may be used under either license at your option.
 * See LICENSE in the respective repository for the full license texts.
 ******************************************************************************
 */

#ifndef MIMIC_REGISTERS_H
#define MIMIC_REGISTERS_H

// =========================================================
// System Constants & Operational Macros
// =========================================================

// --- System Identification Values ---
#define MIMIC_DEVICE_ID_VALUE             0x4D // WHO_AM_I Value ('M' = 0x4D)

#define MIMIC_SYSTEM_CLOCK_HZ             72000000U
#define MIMIC_ADC_SAMPLING_HZ             200000U

#define MIMIC_ADC_MAX_VALUE               4095
#define MIMIC_ADC_MIN_VALUE               0
#define MIMIC_ADC_MID_VALUE               2048 // Analog mid-scale ground (Vref/2)

// --- Sub-types (payload [0] of the grouped modes) ---
#define MIMIC_FILTER_1ST_LPF              0
#define MIMIC_FILTER_1ST_HPF              1

#define MIMIC_COMPARATOR_WINDOW           0
#define MIMIC_COMPARATOR_SCHMITT          1

#define MIMIC_FILTER_BIQUAD_LPF           0
#define MIMIC_FILTER_BIQUAD_HPF           1
#define MIMIC_FILTER_BIQUAD_BPF           2 // 0 dB peak gain
#define MIMIC_FILTER_BIQUAD_NOTCH         3
#define MIMIC_FILTER_BIQUAD_UNITY         4 // Passes the input, same CPU load (profiling)

#define MIMIC_MATH_DERIVATIVE             0
#define MIMIC_MATH_INTEGRAL               1

#define MIMIC_LOG_TYPE_LOG                0 // Vref + slope * log10(x / unit)
#define MIMIC_LOG_TYPE_ANTILOG            1 // unit * 10^((x - Vref) / slope)

#define MIMIC_POLARITY_POSITIVE           0
#define MIMIC_POLARITY_NEGATIVE           1

// --- Rectifier types (payload of MIMIC_MODE_ID_RECTIFIER, with MIMIC_POLARITY_*) ---
#define MIMIC_RECT_FULL                   0 // positive: Vref + |x - Vref|, negative: Vref - |x - Vref|
#define MIMIC_RECT_HALF                   1 // positive: max(x, Vref),      negative: min(x, Vref)

// --- raw_fx3: interface format for voltages (signed 16-bit, 1 LSB = 8) ---
#define MIMIC_RAW_FX3_FRAC_BITS           3
#define MIMIC_RAW_FX3_MAX                 32767  // 4095.875 LSB (just below VDD)
#define MIMIC_VREF_DEFAULT_RAW_FX3        16384  // VDD / 2

// --- Decimation: processing rate = MIMIC_ADC_SAMPLING_HZ / 2^N ---
// Limit: interpolation step (raw_fx16 >> N) keeps < 1 LSB error up to N = 16
#define MIMIC_DECIMATION_N_MAX            16     // 200 kHz / 65536 = about 3 Hz


// =========================================================
// Analog Mimic - I2C Protocol & Register Definitions
// (This file is shared between Firmware and Arduino Library)
// =========================================================

// --- Connection Defaults ---
#define MIMIC_DEFAULT_I2C_ADDR            0x40
#define MIMIC_REG_SHADOW_SIZE             0x40 // Size of physical RAM registers (0x00 - 0x3F)

// --- Device Memory Map ---
#define MIMIC_REG_SYSTEM_START            0x00 // Start of read-only system registers (0x00 - 0x0F)
// --- Static information ---
#define MIMIC_REG_SYSTEM_ID               0x00 // WHO_AM_I Register
#define MIMIC_REG_SYSTEM_HW_VERSION       0x01
#define MIMIC_REG_SYSTEM_BUILD_DATE_3     0x02 // Firmware build date, BCD-like hex 0xYYYYMMDD (bits 31-24)
#define MIMIC_REG_SYSTEM_BUILD_DATE_2     0x03 // (bits 23-16)
#define MIMIC_REG_SYSTEM_BUILD_DATE_1     0x04 // (bits 15-8)
#define MIMIC_REG_SYSTEM_BUILD_DATE_0     0x05 // (bits 7-0)
#define MIMIC_REG_SYSTEM_BUILD_DATE       MIMIC_REG_SYSTEM_BUILD_DATE_3
#define MIMIC_REG_SYSTEM_UID_3            0x06 // Unit ID: 32-bit FNV-1a hash of the MCU UID (bits 31-24)
#define MIMIC_REG_SYSTEM_UID_2            0x07 // (bits 23-16)
#define MIMIC_REG_SYSTEM_UID_1            0x08 // (bits 15-8)
#define MIMIC_REG_SYSTEM_UID_0            0x09 // (bits 7-0)
#define MIMIC_REG_SYSTEM_UID              MIMIC_REG_SYSTEM_UID_3
// --- Dynamic telemetry ---
#define MIMIC_REG_SYSTEM_STATUS           0x0A
#define MIMIC_REG_SYSTEM_ADC_VAL_H        0x0B
#define MIMIC_REG_SYSTEM_ADC_VAL_L        0x0C
#define MIMIC_REG_SYSTEM_ADC_VAL          MIMIC_REG_SYSTEM_ADC_VAL_H
#define MIMIC_REG_SYSTEM_CPU_CYCLES_H     0x0D
#define MIMIC_REG_SYSTEM_CPU_CYCLES_L     0x0E
#define MIMIC_REG_SYSTEM_CPU_CYCLES       MIMIC_REG_SYSTEM_CPU_CYCLES_H
#define MIMIC_REG_SYSTEM_END              0x0E // End of read-only system registers

// --- Common Parameters (Applied globally across all DSP modes) ---
#define MIMIC_REG_GLOBAL_START            0x10
#define MIMIC_REG_GLOBAL_FLAGS            0x10
#define MIMIC_REG_GLOBAL_DECIMATION_N     0x11
#define MIMIC_REG_GLOBAL_OFFSET_H         0x12
#define MIMIC_REG_GLOBAL_OFFSET_L         0x13
#define MIMIC_REG_GLOBAL_OFFSET           MIMIC_REG_GLOBAL_OFFSET_H
#define MIMIC_REG_GLOBAL_VREF_H           0x14 // Common reference voltage (Vref)
#define MIMIC_REG_GLOBAL_VREF_L           0x15
#define MIMIC_REG_GLOBAL_VREF             MIMIC_REG_GLOBAL_VREF_H
#define MIMIC_REG_GLOBAL_END              0x15

// --- Mode-Specific Parameter Blocks ---
#define MIMIC_REG_MODE_START              0x20
#define MIMIC_REG_MODE_ID                 0x20
#define MIMIC_REG_MODE_PAYLOAD_START      0x21 
#define MIMIC_REG_MODE_PAYLOAD_END        0x2F 
#define MIMIC_REG_MODE_END                0x2F


// =========================================================
// Non-Volatile Calibration Registers (Backed by FLASH)
// =========================================================

#define MIMIC_REG_NVM_START               0x30
#define MIMIC_REG_NVM_GAIN_H              0x30 // Gain Calibration (High Byte)
#define MIMIC_REG_NVM_GAIN_L              0x31 // Gain Calibration (Low Byte)
#define MIMIC_REG_NVM_GAIN                MIMIC_REG_NVM_GAIN_H
#define MIMIC_REG_NVM_OFFSET_H            0x32 // Offset Calibration (High Byte)
#define MIMIC_REG_NVM_OFFSET_L            0x33 // Offset Calibration (Low Byte)
#define MIMIC_REG_NVM_OFFSET              MIMIC_REG_NVM_OFFSET_H
#define MIMIC_REG_NVM_END                 0x33
#define MIMIC_REG_NVM_RAM_SIZE            (MIMIC_REG_NVM_END - MIMIC_REG_NVM_START + 1)


// =========================================================
// System Commands (Action Triggers, outside Shadow RAM)
// =========================================================

#define MIMIC_REG_CMD_PORT                0x40 // Command Interface (Write-Only)

// Commands for MIMIC_REG_CMD_PORT
#define MIMIC_CMD_NOP                     0x00
#define MIMIC_CMD_NVM_COMMIT              0xA5 // Save current NVM values to FLASH
#define MIMIC_CMD_NVM_RELOAD              0x5A // Discard NVM values and reload from FLASH


// =========================================================
// Status & Control Flags
// =========================================================

// --- System Status Telemetry Flags ---
#define MIMIC_STATUS_SYSTEM_READY         (1 << 0)
#define MIMIC_STATUS_SIGNAL_SATURATION    (1 << 1)
#define MIMIC_STATUS_I2C_COM_ERR          (1 << 2)
#define MIMIC_STATUS_ADC_OVER_ERR         (1 << 3)
#define MIMIC_STATUS_STATE_MASK           (MIMIC_STATUS_SYSTEM_READY)

// --- Global Control Flags ---
#define MIMIC_FLAG_INV_OUT                (1 << 7) // Inverts the final output code layout (4095 - val)
#define MIMIC_FLAG_OUT_OPEN               (1 << 6) // Enforces high-impedance OPEN state (Bypasses core pipeline)

// MIMIC_SERIES_1X Exclusive Controls
#define MIMIC_FLAG_SERVO_EN               (1 << 4) // Valid only for 1x series hardware architectures (Reserved)


// =========================================================
// Operating Mode IDs
// =========================================================

#define MIMIC_MODE_ID_BYPASS_DSP          0x00 // DSP bypass (offset / inversion only)
#define MIMIC_MODE_ID_PGA                 0x01 // Programmable gain amplifier around Vref
#define MIMIC_MODE_ID_FILTER_1ST          0x02 // 1st-order filter (LPF / HPF)
#define MIMIC_MODE_ID_FILTER_BIQUAD       0x03 // 2nd-order filter (state-variable, coefficients from the host)
#define MIMIC_MODE_ID_COMPARATOR          0x04 // Comparator (window / Schmitt), GPIO output
#define MIMIC_MODE_ID_CLIPPER             0x05 // Clipper (upper / lower limits)
#define MIMIC_MODE_ID_RECTIFIER           0x06 // Rectifier folding at Vref (full / half)
#define MIMIC_MODE_ID_ENVELOPE_FOLLOWER   0x07 // Envelope follower / peak hold
#define MIMIC_MODE_ID_LOG                 0x08 // Logarithmic / antilogarithmic amplifier
#define MIMIC_MODE_ID_MATH                0x09 // Derivative / leaky integrator
#define MIMIC_MODE_ID_SLEW_RATE_LIMITER   0x0A // Slew rate limiter
#define MIMIC_MODE_ID_SAMPLE_AND_HOLD     0x0B // Sample & hold / track & hold
#define MIMIC_MODE_ID_DELAY               0x0C // Digital delay line

#define MIMIC_MODE_MAX_ID                 MIMIC_MODE_ID_DELAY


// =========================================================
// Number Formats
// =========================================================
// All multi-byte values are big-endian.
//
//   raw          ADC / DAC code. 0 = 0 V, 4096 = VDD (integer, 0 - 4095).
//                1 LSB = VDD / 4096.
//   raw_fx3      raw with 3 fractional bits, signed 16-bit (1 LSB = 8).
//                -4096 to +4095.875 LSB. Used for every voltage at the interface.
//   q0_15        Signed coefficient, 15 fractional bits (-1.0 to < 1.0).
//   uq1_15       Unsigned gain, 15 fractional bits (0 to < 2.0, 1.0 = 32768).
//   mant / exp   value = mant (q0_15) * 2^exp (exp: int8 shift).
//
// Common reference (Vref)
//   An input of the module (default VDD / 2, limited to 0 V - VDD). Modes with a
//   signal center work around Vref: y = Vref + H(x - Vref). Changing Vref does not
//   reset any state (e.g. a slow HPF settles to the new Vref with its time constant).
//   Every mode with a signal center uses Vref: 1st filter, Biquad, PGA, envelope,
//   math (derivative / integral), rectifier, and the bipolar side of LOG.
//   Thresholds (comparator, clipper) and magnitudes (LOG input, ANTILOG output) are
//   absolute (0 V based). Modes without a center (bypass, slew rate limiter, S&H,
//   delay) do not use Vref.
//
// Register formats
//   0x12-0x13  GLOBAL_OFFSET    int16   raw_fx3  User output offset
//   0x14-0x15  GLOBAL_VREF      int16   raw_fx3  Common reference voltage
//   0x30-0x31  NVM_GAIN         uint16  uq1_15   Output gain calibration
//   0x32-0x33  NVM_OFFSET       int16   raw_fx3  Output offset calibration
//
//   Mode payload (from MIMIC_REG_MODE_PAYLOAD_START). Grouped modes have the type at [0].
//   PGA                [0-1] gain mant q0_15, [2] gain exp int8 (>= 0)
//   FILTER_1ST         [0] type (MIMIC_FILTER_1ST_*), [1-2] alpha q0_15
//   FILTER_BIQUAD      TPT state-variable filter (bilinear; g = tan(pi fc / fs), k = 1 / Q)
//                      [0] type (MIMIC_FILTER_BIQUAD_*),
//                      [1-2] g mant uint16, [3] g shift: g * 2^12 = mant >> shift
//                            (mant 2048 - 4095, i.e. raw_vref_fx3 -> raw << 15 states),
//                      [4-5] k + g uint16 (10 fractional bits),
//                      [6-7] e mant uint16, [8] e shift: e = mant >> shift,
//                            e = 1 - 1 / (1 + g k + g^2),
//                      [9-10] k uint16 (11 fractional bits)
//                      Limits (library): fs / 100000 <= fc < fs / 4, 0.1 <= Q <= 10
//   COMPARATOR         [0] type (MIMIC_COMPARATOR_*), [1-2] upper raw_fx3, [3-4] lower raw_fx3
//   CLIPPER            [0-1] upper raw_fx3, [2-3] lower raw_fx3
//   RECTIFIER          [0] type (MIMIC_RECT_*), [1] polarity (MIMIC_POLARITY_*)
//   ENVELOPE_FOLLOWER  [0-1] decay q0_15, [2] polarity
//   LOG                [0] type (MIMIC_LOG_TYPE_*), [1-2] slope raw_fx3 per decade
//                      (signed, negative = inverting), [3-4] unit raw_fx3 (> 0)
//   MATH               [0] type (MIMIC_MATH_*), [1-2] mant q0_15, [3] exp int8
//                      (derivative: scale = mant * 2^exp, integral: leak = mant,
//                       input shift = exp <= 1)
//   SLEW_RATE_LIMITER  [0-1] max change per sample raw_fx3 (>= 0)
//   SAMPLE_AND_HOLD    [0-1] period, [2-3] track (samples, uint16)
//   DELAY              [0-1] delay (samples, uint16)

#endif // MIMIC_REGISTERS_H
