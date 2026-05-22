#ifndef MIMIC_REGISTERS_H
#define MIMIC_REGISTERS_H

// =========================================================
// Analog Mimic - I2C Protocol & Register Definitions
// (This file is shared between Firmware and Arduino Library)
// =========================================================

// --- 0. Connection Defaults
#define MIMIC_DEFAULT_I2C_ADDR 0x40

// --- 1. Device Memory Map ---
#define MIMIC_REG_SHADOW_SIZE       0x20 /* Size of physical RAM registers (0x00 - 0x1F) */
#define MIMIC_REG_TELEMETRY_START   0x20 /* Start of virtual telemetry registers */
#define MIMIC_REG_TELEMETRY_END     0x24 /* End of virtual telemetry registers (MIMIC_REG_CPU_CYCLES_L) */
#define MIMIC_REG_GLOBAL_FLAGS      0x00

// --- Common Parameters (Applied globally across all DSP modes) ---
#define MIMIC_REG_COMMON_START      0x01
#define MIMIC_REG_OUTPUT_OFFSET_H   0x01
#define MIMIC_REG_OUTPUT_OFFSET_L   0x02

// --- Mode-Specific Parameter Blocks ---
#define MIMIC_REG_MODE_SELECT       0x10
#define MIMIC_REG_PAYLOAD_START     0x11 

// --- Read-Only Telemetry Status Registers ---
#define MIMIC_REG_STATUS            0x20
#define MIMIC_REG_ADC_VAL_H         0x21
#define MIMIC_REG_ADC_VAL_L         0x22
#define MIMIC_REG_CPU_CYCLES_H      0x23
#define MIMIC_REG_CPU_CYCLES_L      0x24

// --- Register Label Aliases ---
#define MIMIC_REG_DELAY_SAMPLES     MIMIC_REG_DELAY_SAMPLES_H
#define MIMIC_REG_OUTPUT_OFFSET     MIMIC_REG_OUTPUT_OFFSET_H
#define MIMIC_REG_ADC_VAL           MIMIC_REG_ADC_VAL_H
#define MIMIC_REG_CPU_CYCLES        MIMIC_REG_CPU_CYCLES_H

// --- 2. Global Control Flags ---
#define MIMIC_FLAG_INV_OUT  (1 << 7) // Inverts the final output code layout (4095 - val)
#define MIMIC_FLAG_OUT_OPEN (1 << 6) // Enforces high-impedance OPEN state (Bypasses core pipeline)

// MIMIC_SERIES_1X Exclusive Controls
#define MIMIC_FLAG_SERVO_EN (1 << 5) // Valid only for 1x series hardware architectures

// =========================================================
// 3. Operating Mode IDs (Integrated 16 Modes)
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

// --- 4. System Status Telemetry Flags ---
#define MIMIC_STATUS_SYSTEM_READY      (1 << 0)
#define MIMIC_STATUS_SIGNAL_SATURATION (1 << 1)
#define MIMIC_STATUS_I2C_COM_ERR       (1 << 2)
#define MIMIC_STATUS_ADC_OVER_ERR      (1 << 3)
#define MIMIC_STATUS_STATE_MASK        (MIMIC_STATUS_SYSTEM_READY)

// =========================================================
// System Constants & Operational Macros
// =========================================================
#define MIMIC_SYSTEM_CLOCK_HZ   72000000U
#define MIMIC_ADC_SAMPLING_HZ   300000U

#define MIMIC_ADC_MAX_VALUE 4095
#define MIMIC_ADC_MIN_VALUE 0
#define MIMIC_ADC_MID_VALUE 2048 // Analog mid-scale ground (Vref/2)

#define MIMIC_LUT_LOG       0
#define MIMIC_LUT_ANTILOG   1

#define MIMIC_POLALYTY_POSITIVE 0
#define MIMIC_POLALYTY_NEGATIVE 1

#endif // MIMIC_REGISTERS_H
