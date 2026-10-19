/**
 ******************************************************************************
 * @file    mimic_device.h
 * @author  TAKAGI Katsuyuki
 * @brief   Global Device Context Structure and Thread-Safe Accessor APIs.
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

#ifndef MIMIC_DEVICE_H
#define MIMIC_DEVICE_H

#include <stdint.h>
#include <stdbool.h>

#include "mimic_registers.h"
#include "mimic0x_msp.h"

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================
// Global Device Context
// =========================================================

/**
 * @brief Encapsulates the global internal state of the Analog Mimic Device.
 * @note  All members are qualified as 'volatile' because they bridge asynchronous
 *        execution contexts (Sampling ISR, I2C ISR, and the Main Background Loop).
 */
typedef struct {
    // --- 1. I2C Shadow Registers (Written by I2C ISR / Main, Read by I2C ISR / Main) ---
    volatile uint8_t  registers[MIMIC_REG_SHADOW_SIZE];

    // --- 2. Live Telemetry (Written by Sampling ISR / I2C ISR, Read by I2C ISR / Main) ---
    volatile uint8_t  status;                 /**< System status flags & transient error bits */
    volatile uint16_t adc_val;                /**< Latest raw data acquired from ADC */
    volatile uint16_t cpu_cycles_max;         /**< Peak-hold execution cycle counter for profiling */

    // --- 3. Main Loop Task Triggers (Latched by I2C ISR on STOP, Consumed by Main Loop) ---
    volatile bool     pending_dsp_update;     /**< Requests DSP parameter & route reconfiguration */
    volatile uint8_t  pending_system_command; /**< Requests deferred system command (e.g., NVM commit) */
} MimicDevice_t;

// =========================================================
// Lifecycle & Background Task Processing APIs
// =========================================================

/**
 * @brief Initializes I2C, DSP, loads NVM calibration, and starts the sampling engine.
 */
void MimicDevice_Init(void);

/**
 * @brief Evaluates and executes pending commands and DSP updates requested by I2C.
 * @note  Called from the main background loop while interrupts remain active.
 */
void MimicDevice_ProcessPendingTasks(void);

// =========================================================
// Parameter Decoding Utilities (Big-Endian)
// =========================================================

/**
 * @brief Restores a 16-bit unsigned integer from a Big-Endian byte array.
 */
static inline __attribute__((always_inline)) uint16_t _MimicDevice_ReadU16_BE(const volatile uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

/**
 * @brief Restores a 16-bit signed integer from a Big-Endian byte array.
 */
static inline __attribute__((always_inline)) int16_t _MimicDevice_ReadS16_BE(const volatile uint8_t *p) {
    return (int16_t)((p[0] << 8) | p[1]);
}

// =========================================================
// Shadow Register & I2C Notification Accessors
// =========================================================

static inline __attribute__((always_inline)) uint8_t MimicDevice_ReadReg(uint8_t reg) {
    extern MimicDevice_t mimic_device;
    return mimic_device.registers[reg];
}

static inline __attribute__((always_inline)) void MimicDevice_WriteReg(uint8_t addr, uint8_t val) {
    extern MimicDevice_t mimic_device;
    mimic_device.registers[addr] = val;
}

static inline __attribute__((always_inline)) void MimicDevice_RequestSystemCommand(uint8_t cmd) {
    extern MimicDevice_t mimic_device;
    mimic_device.pending_system_command = cmd;
}

static inline __attribute__((always_inline)) void MimicDevice_RequestUpdate(void) {
    extern MimicDevice_t mimic_device;
    mimic_device.pending_dsp_update = true;
}

// =========================================================
// Thread-Safe Status & Telemetry Accessors
// =========================================================

/**
 * @brief Thread-safe setter for the device status flags.
 */
void MimicDevice_SetStatusFlag(uint8_t flag);

/**
 * @brief Retrieves the current status bitmask and flushes transient error flags atomically.
 */
uint8_t MimicDevice_GetAndClearStatusFlag(void);

/**
 * @brief Retrieves the latest sampled ADC value from the device context.
 */
static inline __attribute__((always_inline)) uint16_t MimicDevice_GetAdcVal(void) {
    extern MimicDevice_t mimic_device;
    return mimic_device.adc_val;
}

/**
 * @brief Retrieves the peak CPU cycle execution count and resets the tracking value to 0.
 */
static inline __attribute__((always_inline)) uint16_t MimicDevice_PopCpuCyclesMax(void) {
    extern MimicDevice_t mimic_device;
    uint16_t max_cycles = mimic_device.cpu_cycles_max;
    mimic_device.cpu_cycles_max = 0;
    return max_cycles;
}

// =========================================================
// Fast Inline Accessors for High-Frequency Sampling ISR
// =========================================================

static inline __attribute__((always_inline)) void MimicDevice_SetAdcVal_ISR(uint16_t raw_val) {
    extern MimicDevice_t mimic_device;
    mimic_device.adc_val = raw_val;
}

static inline __attribute__((always_inline)) void MimicDevice_SetErrorFlag_ISR(uint8_t error_flag) {
    extern MimicDevice_t mimic_device;
    mimic_device.status |= error_flag;
}

static inline __attribute__((always_inline)) void MimicDevice_UpdateCpuCyclesMax_ISR(uint32_t cycles) {
    extern MimicDevice_t mimic_device;
    if (cycles > mimic_device.cpu_cycles_max) {
        mimic_device.cpu_cycles_max = (uint16_t)cycles;
    }
}

#ifdef __cplusplus
}
#endif

#endif // MIMIC_DEVICE_H
