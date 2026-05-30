/**
 ******************************************************************************
 * @file    mimic_registers.h
 * @author  TAKAGI Katsuyuki
 * @brief   Memory-Mapped Register Definitions, Volatile Configuration Layouts, 
 * and Thread-Safe Virtual Telemetry Handlers.
 * @note    This register map is shared directly between the device firmware
 * (Secondary peripheral) and the software / Arduino library (Host).
 *-----------------------------------------------------------------------------
 * Copyright (C) 2026 TAKAGI Katsuyuki
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
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
#define MIMIC_ADC_SAMPLING_HZ             192000U

#define MIMIC_ADC_MAX_VALUE               4095
#define MIMIC_ADC_MIN_VALUE               0
#define MIMIC_ADC_MID_VALUE               2048 // Analog mid-scale ground (Vref/2)

#define MIMIC_LUT_LOG                     0
#define MIMIC_LUT_ANTILOG                 1

#define MIMIC_POLARITY_POSITIVE           0
#define MIMIC_POLARITY_NEGATIVE           1


// =========================================================
// Analog Mimic - I2C Protocol & Register Definitions
// (This file is shared between Firmware and Arduino Library)
// =========================================================

// --- Connection Defaults ---
#define MIMIC_DEFAULT_I2C_ADDR            0x40
#define MIMIC_REG_SHADOW_SIZE             0x40 // Size of physical RAM registers (0x00 - 0x3F)

// --- Device Memory Map ---
#define MIMIC_REG_SYSTEM_START            0x00 // Start of virtual telemetry registers
#define MIMIC_REG_SYSTEM_ID               0x00 // WHO_AM_I Register
#define MIMIC_REG_SYSTEM_HW_VERSION       0x01
#define MIMIC_REG_SYSTEM_FW_VERSION       0x02 // Firmware Revision (0 - 255)
#define MIMIC_REG_SYSTEM_STATUS           0x03
#define MIMIC_REG_SYSTEM_ADC_VAL_H        0x04
#define MIMIC_REG_SYSTEM_ADC_VAL_L        0x05
#define MIMIC_REG_SYSTEM_ADC_VAL          MIMIC_REG_SYSTEM_ADC_VAL_H
#define MIMIC_REG_SYSTEM_CPU_CYCLES_H     0x06
#define MIMIC_REG_SYSTEM_CPU_CYCLES_L     0x07
#define MIMIC_REG_SYSTEM_CPU_CYCLES       MIMIC_REG_SYSTEM_CPU_CYCLES_H
#define MIMIC_REG_SYSTEM_END              0x07 // End of virtual telemetry registers

// --- Common Parameters (Applied globally across all DSP modes) ---
#define MIMIC_REG_GLOBAL_START            0x10
#define MIMIC_REG_GLOBAL_FLAGS            0x10
#define MIMIC_REG_GLOBAL_DECIMATION_N     0x11
#define MIMIC_REG_GLOBAL_OFFSET_H         0x12
#define MIMIC_REG_GLOBAL_OFFSET_L         0x13
#define MIMIC_REG_GLOBAL_OFFSET           MIMIC_REG_GLOBAL_OFFSET_H
#define MIMIC_REG_GLOBAL_END              0x13

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
#define MIMIC_REG_NVM_GAIN_Q15_H          0x30 // Gain Calibration (High Byte)
#define MIMIC_REG_NVM_GAIN_Q15_L          0x31 // Gain Calibration (Low Byte)
#define MIMIC_REG_NVM_GAIN_Q15            MIMIC_REG_NVM_GAIN_Q15_H
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
#define MIMIC_FLAG_SERVO_EN               (1 << 5) // Valid only for 1x series hardware architectures


// =========================================================
// Operating Mode IDs (Integrated 16 Modes)
// =========================================================

// --- Basic Input / Output Engine ---
#define MIMIC_MODE_ID_BYPASS_DSP          0x00 // DSP Bypass (Applies Delay/Offset/Inversion pipelines only)
#define MIMIC_MODE_ID_PGA                 0x01 // Programmable Gain Amplifier (Supports AC inversion with reference voltage)

// --- Filtering Topologies ---
#define MIMIC_MODE_ID_FILTER_1ST_LPF      0x02 // 1st-Order Low Pass Filter
#define MIMIC_MODE_ID_FILTER_1ST_HPF      0x03 // 1st-Order High Pass Filter (Automatic DC Servo tracking)
#define MIMIC_MODE_ID_FILTER_BIQUAD       0x04 // 2nd-Order Biquad Filter (Configurable to LPF, HPF, BPF, Notch)

// --- Analog Emulation: Comparators, Dynamics & Waveform Shapers ---
#define MIMIC_MODE_ID_COMPARATOR_WIN      0x05 // Window Comparator Configuration
#define MIMIC_MODE_ID_COMPARATOR_SCHMITT  0x06 // Schmitt Trigger Configuration
#define MIMIC_MODE_ID_ENVELOPE_FOLLOWER   0x07 // Peak Hold / Envelope Follower Architecture
#define MIMIC_MODE_ID_CLIPPER             0x08 // Analog Style Signal Clipper Effect
#define MIMIC_MODE_ID_NONLINEAR_LUT       0x09 // Non-linear Look-Up Table Waveshaper (Log / Antilog)

// --- Digital Mathematical Operations & Advanced Features ---
#define MIMIC_MODE_ID_MATH_DERIVATIVE     0x0A // Signal Differentiation (Derivative)
#define MIMIC_MODE_ID_MATH_INTEGRAL       0x0B // Signal Integration (Integral)
#define MIMIC_MODE_ID_RECTIFIER_FULL      0x0C // Full-Wave Rectifier (Absolute value operation)
#define MIMIC_MODE_ID_SLEW_RATE_LIMITER   0x0D // Slew Rate Limiter Block
#define MIMIC_MODE_ID_SAMPLE_AND_HOLD     0x0E // Time-based Sample & Hold / Track & Hold
#define MIMIC_MODE_ID_DELAY               0x0F // Pure Digital Delay Buffer

#define MIMIC_MODE_MAX_ID                 MIMIC_MODE_ID_DELAY

#endif // MIMIC_REGISTERS_H
