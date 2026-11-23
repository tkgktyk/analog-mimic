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
#define MIMIC_FILTER_1ST_APF              2 // All-pass (phase shifter), LPF - HPF

#define MIMIC_CLIPPER_HARD                0 // Passes the input between the limits, clips outside
#define MIMIC_CLIPPER_SOFT                1 // Cubic soft clip: gain 1 at the center, reaches a limit
                                            // at 1.5 x the center-to-limit distance
#define MIMIC_CLIPPER_DEAD_ZONE           2 // (experimental) Vref between the limits, Vref + (x - limit)
                                            // beyond a limit (slope 1, continuous)

#define MIMIC_COMPARATOR_WINDOW           0
#define MIMIC_COMPARATOR_SCHMITT          1

#define MIMIC_FILTER_BIQUAD_LPF           0
#define MIMIC_FILTER_BIQUAD_HPF           1
#define MIMIC_FILTER_BIQUAD_BPF           2 // 0 dB peak gain
#define MIMIC_FILTER_BIQUAD_NOTCH         3
#define MIMIC_FILTER_BIQUAD_UNITY         4 // Passes the input, same CPU load (profiling)
#define MIMIC_FILTER_BIQUAD_APF           5 // All-pass (phase shifter), x - 2 k bp

#define MIMIC_INTEGRATOR_DIFFERENTIATOR   0 // Vref + tau_d * dx/dt, band limited
#define MIMIC_INTEGRATOR_INTEGRATOR       1 // Vref + (1 / tau_i) * integral of (x - Vref), leaky

// (experimental) MULTIPLIER, LOG and OSCILLATOR
#define MIMIC_MULTIPLIER_SQUARER          0 // (x - Vref)^2 / unit                 (0 V based output)
#define MIMIC_MULTIPLIER_SQUARE_ROOTER    1 // sqrt((x - Vref) * unit), 0 below Vref (0 V based output)

#define MIMIC_LOG_TYPE_LOG                0 // Vref + slope * log10((x - Vref) / unit)
#define MIMIC_LOG_TYPE_ANTILOG            1 // unit * 10^((x - Vref) / slope)      (0 V based output)

// (experimental) Waveforms of the oscillator, the modulator and the PWM modulator (payload [0];
// 0 at phase 0, rising; other values: sine)
#define MIMIC_OSC_WAVE_SINE               0
#define MIMIC_OSC_WAVE_TRIANGLE           1
#define MIMIC_OSC_WAVE_SQUARE             2 // +amplitude in the first half of the period
#define MIMIC_OSC_WAVE_SAWTOOTH           3 // rises from -amplitude to +amplitude

// (experimental) Signals on the test pad (MIMIC_REG_GLOBAL_TEST_PAD; other values: off)
#define MIMIC_TEST_PAD_OFF                0 // not driven (analog, Hi-Z)
#define MIMIC_TEST_PAD_SAMPLE_CLOCK       1 // 100 kHz square, toggles at every sample (TIM2 -> TIM1 -> TIM15)

// (experimental) Noise colors (payload [0] of MIMIC_MODE_ID_NOISE)
#define MIMIC_NOISE_WHITE                 0 // uniform within +/-amplitude
#define MIMIC_NOISE_PINK                  1 // -3 dB/octave, rms amplitude / 3 (clipped beyond)

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
#define MIMIC_REG_SYSTEM_ADC_SEQ          0x0B // Sequence number of ADC_VAL; reading it latches SEQ + VAL
#define MIMIC_REG_SYSTEM_ADC_VAL_H        0x0C // DSP input, raw_fx3 (averaged over 2^N when decimated)
#define MIMIC_REG_SYSTEM_ADC_VAL_L        0x0D
#define MIMIC_REG_SYSTEM_ADC_VAL          MIMIC_REG_SYSTEM_ADC_VAL_H
#define MIMIC_REG_SYSTEM_CPU_CYCLES_H     0x0E
#define MIMIC_REG_SYSTEM_CPU_CYCLES_L     0x0F
#define MIMIC_REG_SYSTEM_CPU_CYCLES       MIMIC_REG_SYSTEM_CPU_CYCLES_H
#define MIMIC_REG_SYSTEM_END              0x0F // End of read-only system registers

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
#define MIMIC_REG_GLOBAL_TEST_PAD         0x16 // (experimental) Signal on the test pad (MIMIC_TEST_PAD_*)
#define MIMIC_REG_GLOBAL_END              0x16

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
#define MIMIC_STATUS_DEADLINE_MISS        (1 << 4) // A sample was not finished in time (output repeated or late)
#define MIMIC_STATUS_STATE_MASK           (MIMIC_STATUS_SYSTEM_READY)

// --- Global Control Flags ---
#define MIMIC_FLAG_INV_OUT                (1 << 7) // Inverts the final output code layout (4095 - val)
#define MIMIC_FLAG_OUT_OPEN               (1 << 6) // Enforces high-impedance OPEN state (Bypasses core pipeline)

// MIMIC_SERIES_1X Exclusive Controls
#define MIMIC_FLAG_SERVO_EN               (1 << 4) // Valid only for 1x series hardware architectures (Reserved)


// =========================================================
// Operating Mode IDs
//   (experimental): implemented in the firmware, but not yet verified on hardware and
//   in application examples, and not yet released in the library; the payload may change
//   (docs/experimental_modes.md).
// =========================================================

#define MIMIC_MODE_ID_BYPASS_DSP          0x00 // DSP bypass (offset / inversion only)
#define MIMIC_MODE_ID_PGA                 0x01 // Programmable gain amplifier around Vref
#define MIMIC_MODE_ID_FILTER_1ST          0x02 // 1st-order filter (LPF / HPF / all-pass)
#define MIMIC_MODE_ID_FILTER_BIQUAD       0x03 // 2nd-order filter (state-variable, coefficients from the host)
#define MIMIC_MODE_ID_COMPARATOR          0x04 // Comparator (window / Schmitt), GPIO output
#define MIMIC_MODE_ID_CLIPPER             0x05 // Clipper (upper / lower limits, hard / soft)
#define MIMIC_MODE_ID_RECTIFIER           0x06 // Rectifier folding at Vref (full / half)
#define MIMIC_MODE_ID_ENVELOPE_FOLLOWER   0x07 // Envelope follower / peak hold
#define MIMIC_MODE_ID_LOG                 0x08 // (experimental) Logarithmic / antilogarithmic amplifier
#define MIMIC_MODE_ID_INTEGRATOR          0x09 // Integrator / differentiator (op-amp RC circuits)
#define MIMIC_MODE_ID_SLEW_RATE_LIMITER   0x0A // (experimental) Slew rate limiter
#define MIMIC_MODE_ID_SAMPLE_AND_HOLD     0x0B // (experimental) Sample & hold / track & hold
#define MIMIC_MODE_ID_DELAY               0x0C // (experimental) Digital delay line
#define MIMIC_MODE_ID_OSCILLATOR          0x0D // (experimental) Function generator / VCO (DDS)
#define MIMIC_MODE_ID_MULTIPLIER          0x0E // (experimental) Multiplier: squarer / square rooter
#define MIMIC_MODE_ID_TRUE_RMS            0x0F // (experimental) True RMS-to-DC converter

#define MIMIC_MODE_ID_MODULATOR           0x10 // (experimental) Balanced modulator: AM / ring (DSB)
#define MIMIC_MODE_ID_PWM_MODULATOR       0x11 // (experimental) PWM modulator, GPIO output
#define MIMIC_MODE_ID_VF_CONVERTER        0x12 // (experimental) V/F converter, GPIO output
#define MIMIC_MODE_ID_NOISE               0x13 // (experimental) Noise generator: white / pink

#define MIMIC_MODE_MAX_ID                 MIMIC_MODE_ID_NOISE


// =========================================================
// Number Formats
// =========================================================
// All multi-byte values are big-endian.
//
//   raw          ADC / DAC code. 0 = 0 V, 4096 = VDD (integer, 0 - 4095).
//                1 LSB = VDD / 4096.
//   raw_fx3      raw with 3 fractional bits, signed 16-bit (1 LSB = 8).
//                -4096 to +4095.875 LSB. Used for every voltage at the interface
//                and for every signal inside the DSP; only the ADC read and the
//                DAC write use integer raw codes.
//   q0_15        Signed coefficient, 15 fractional bits (-1.0 to < 1.0).
//   uq1_15       Unsigned gain, 15 fractional bits (0 to < 2.0, 1.0 = 32768).
//   mant / exp   value = mant (q0_15) * 2^exp (exp: int8 shift).
//   mant12 / sh  Positive coefficient c < 1 as a 12-bit mantissa (2048 - 4095) and a shift:
//                c = mant / 2^sh (sh >= 12). mant = 0 means c = 0.
//
// Common reference (Vref)
//   An input of the module (default VDD / 2, limited to 0 V - VDD). Modes with a
//   signal center work around Vref: y = Vref + H(x - Vref). Changing Vref does not
//   reset any state (e.g. a slow HPF settles to the new Vref with its time constant).
//   Every input is taken relative to Vref (filters, PGA, envelope, rectifier, math,
//   LOG / ANTILOG, oscillator FM). Outputs with a sign are around Vref; magnitudes
//   (ANTILOG, SQUARE, SQRT, RMS) are 0 V based: lift them with GLOBAL_OFFSET when the
//   output must stay clear of 0 V. When modules are chained, set the Vref of the next
//   module to the offset of the previous one.
//   Thresholds (comparator, clipper) are absolute (0 V based). Modes without a center
//   (bypass, slew rate limiter, S&H, delay) do not use Vref.
//
// Register formats
//   0x0B       SYSTEM_ADC_SEQ   uint8   -        Counts ADC_VAL updates (wraps at 256): once per
//                                                sample, or once per 2^N block when decimated.
//                                                Reading 0x0B latches SEQ and VAL together; read
//                                                0x0B - 0x0D in one transaction.
//   0x0C-0x0D  SYSTEM_ADC_VAL   uint16  raw_fx3  DSP input (latched by the 0x0B read): the latest
//                                                ADC sample, or the rounded average of 2^N samples
//                                                when decimated.
//              Averaging without I2C traffic: read 0x0B - 0x0D (SEQ c0), wait in silence,
//              read 0x0B - 0x0D again (SEQ c, VAL v). If c - c0 >= 2, v is the average of a
//              block measured entirely in silence.
//   0x12-0x13  GLOBAL_OFFSET    int16   raw_fx3  User output offset
//   0x14-0x15  GLOBAL_VREF      int16   raw_fx3  Common reference voltage
//   0x30-0x31  NVM_GAIN         uint16  uq1_15   Output gain calibration
//   0x32-0x33  NVM_OFFSET       int16   raw_fx3  Output offset calibration
//
//   Mode payload (from MIMIC_REG_MODE_PAYLOAD_START). Grouped modes have the type at [0].
//   PGA                [0-1] gain mant q0_15, [2] gain exp int8 (>= 0)
//   FILTER_1ST         TPT one-pole filter (bilinear; g = tan(pi fc / fs), G = g / (1 + g))
//                      [0] type (MIMIC_FILTER_1ST_*),
//                      [1-2] G mant uint16, [3] G shift: G * 2^12 = mant >> shift
//                      Limits (library): fs / 100000 <= fc <= 0.24 fs
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
//   CLIPPER            [0] type (MIMIC_CLIPPER_*), [1-2] upper raw_fx3, [3-4] lower raw_fx3
//                      (dead zone, experimental: upper < lower is taken as upper = lower)
//   RECTIFIER          [0] type (MIMIC_RECT_*), [1] polarity (MIMIC_POLARITY_*)
//   ENVELOPE_FOLLOWER  [0] polarity (MIMIC_POLARITY_*), [1-2] 1 - decay mant12, [3] 1 - decay sh
//                      (decay per sample; 1 - decay = 0: peak hold)
//   LOG                (experimental) [0] type (MIMIC_LOG_TYPE_*), [1-2] slope raw_fx3 per decade
//                      (signed, negative = inverting), [3-4] unit raw_fx3 (> 0).
//                      LOG input and ANTILOG output are magnitudes from Vref / 0 V:
//                      LOG gives Vref when x - Vref = unit.
//   INTEGRATOR         [0] type (MIMIC_INTEGRATOR_*), [1-2] K mant q0_15, [3] K exp int8:
//                      K = mant * 2^(exp - 15), [4-5] c mant12, [6] c sh (11 - 32; 0: none):
//                      c = mant / 2^sh. Per sample m += c * (K * u - m), y = Vref + m.
//                      integrator:     u = x - Vref; with leak K = DC gain, c = 1 - exp(-Ts / tau_leak);
//                                      without leak (c sh 0) m += K * u, K = Ts / tau_i (<= 1)
//                      differentiator: u = x[n] - x[n-1], K = tau_d * fs,
//                                      c = 1 - exp(-Ts / tau_limit) (c sh 0: c = 1, no limit)
//   MULTIPLIER         (experimental) [0] type (MIMIC_MULTIPLIER_*), [1-2] unit raw_fx3 (> 0). The same unit
//                      in a squarer and a square-rooter module gives |x - Vref| back.
//   TRUE_RMS           (experimental) [0-1] 1 - a mant12, [2] 1 - a sh (13 - 32): averaging
//                      m += (1 - a) * ((x - Vref)^2 - m) per sample, y = sqrt(m), gain 1
//   OSCILLATOR         (experimental) [0] waveform (MIMIC_OSC_WAVE_*), [1-4] phase increment
//                      uint32 (< 2^31): f0 = inc * fs / 2^32, [5-6] amplitude raw_fx3 (peak, >= 0),
//                      [7-8] k mant int16, [9] k exp int8 (-16 - 4): the phase increment changes
//                      by k * (x - Vref), k = mant * 2^exp (0 - fs / 2); mant 0: fixed frequency.
//                      The phase continues when the payload changes (0 on mode entry).
//   MODULATOR          (experimental) as OSCILLATOR, with k as a gain (raw_fx3 per raw_fx3):
//                      y = Vref + (amplitude + k * (x - Vref)) * wave; amplitude 0: DSB (ring)
//   PWM_MODULATOR      (experimental) [0] carrier waveform (MIMIC_OSC_WAVE_*), [1-4] phase
//                      increment, [5-6] amplitude (full scale): HIGH while x - Vref > carrier
//   VF_CONVERTER       (experimental) [0-3] phase increment (f0 at Vref), [4-5] k mant int16,
//                      [6] k exp int8: square at f = f0 + k * (x - Vref), as the VCO
//   NOISE              (experimental) [0] color (MIMIC_NOISE_*), [1-2] amplitude raw_fx3
//   SLEW_RATE_LIMITER  (experimental) [0-1] max change per sample raw_fx3 (>= 0)
//   SAMPLE_AND_HOLD    (experimental) [0-1] period, [2-3] track (samples, uint16)
//   DELAY              (experimental) [0-1] delay (samples, uint16)

#endif // MIMIC_REGISTERS_H
