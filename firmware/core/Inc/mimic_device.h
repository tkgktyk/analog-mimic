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

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================
// Global Device Context
// =========================================================

/**
 * @brief Encapsulates the global internal state of the Analog Mimic Device.
 * @note  All members are volatile because they are shared between the 
 * main execution background loop and high-frequency ISRs.
 */
typedef struct {
  volatile uint8_t  registers[MIMIC_REG_SHADOW_SIZE]; /**< I2C protocol register memory map */
  volatile uint8_t  status;                           /**< Bitfield containing current system flags and transient errors */
  volatile uint16_t adc_val;                          /**< Latest raw data acquired from the ADC input channel */
  volatile uint16_t cpu_cycles_max;                   /**< Peak-hold execution cycle counter for profiling */
  volatile bool     i2c_dirty_flag;                   /**< Semaphore flag indicating a parameter update is requested */
  volatile uint8_t  pending_system_command;           /**< Command byte for deferred system operations (e.g., NVM flush) */
} MimicDevice_t;

typedef void (*MimicCallback_t)(void);

void MimicDevice_Init(MimicCallback_t enable_output, MimicCallback_t disable_output);

// =========================================================
// Parameter Decoding Utilities (Big-Endian)
// =========================================================

/**
 * @brief Restores a 16-bit unsigned integer from a Big-Endian byte array (type-safe function version).
 * @note  Defined with always_inline attribute as a zero-overhead alternative to macros.
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

/* * In C, 'static inline' functions guarantee direct memory access rather than 
 * function call overhead at compile time, resulting in zero extra clock cycles.
 * * Note: Block-scope 'extern' is intentionally used here to strictly encapsulate 
 * 'mimic_device' from being accessed directly by other source files.
 */
static inline __attribute__((always_inline)) uint8_t MimicDevice_ReadReg(uint8_t reg) {
    extern MimicDevice_t mimic_device;
    return mimic_device.registers[reg];
}

static inline __attribute__((always_inline)) void MimicDevice_WriteReg(uint8_t addr, uint8_t val) {
    extern MimicDevice_t mimic_device;
    mimic_device.registers[addr] = val;
}

// =========================================================
// Thread-Safe Accessor APIs
// =========================================================

/**
 * @brief Thread-safe setter for the device status flags.
 * @param flag Bitmask flag to be set in the status register.
 * @note  This operation is completely atomic and guarantees thread safety against race conditions.
 */
void MimicDevice_SetStatusFlag(uint8_t flag);

/**
 * @brief Retrieves the current status bitmask and flushes transient error flags automatically.
 * @return The current state of the status register prior to flushing.
 * @note  This operation is atomic. Retains foundational system states (e.g., READY).
 */
uint8_t MimicDevice_GetAndClearStatusFlag(void);

/**
 * @brief Retrieves the latest sampled ADC value from the device context.
 * @return Raw 12-bit ADC data (0 - 4095).
 * @note  This read access is inherently atomic on 32-bit Cortex-M core architectures.
 */
static inline __attribute__((always_inline)) uint16_t MimicDevice_GetAdcVal(void) {
    extern MimicDevice_t mimic_device;
    return mimic_device.adc_val;
}

/**
 * @brief Retrieves the peak CPU cycle execution count and resets the tracking value to 0.
 * @return The maximum recorded CPU cycles since the previous read operation.
 * @note  This operation is atomic to prevent the sampling ISR from writing intermediate data.
 */
static inline __attribute__((always_inline)) uint16_t MimicDevice_PopCpuCyclesMax(void) {
    extern MimicDevice_t mimic_device;
    uint16_t max_cycles = mimic_device.cpu_cycles_max;
    mimic_device.cpu_cycles_max = 0; // Reset upon retrieval
    return max_cycles;
}

static inline __attribute__((always_inline)) void MimicDevice_RequestSystemCommand(uint8_t cmd) {
    extern MimicDevice_t mimic_device;
    mimic_device.pending_system_command = cmd;
}

static inline __attribute__((always_inline)) void MimicDevice_RequestUpdate(void) {
    extern MimicDevice_t mimic_device;
    mimic_device.i2c_dirty_flag = true;
}

/**
 * @brief Applies parameters acquired via I2C to the internal DSP mathematical models.
 * @note  Typically called from within MimicDSP_ProcessLoop() while hardware
 * analog outputs are safely isolated.
 */
void MimicDevice_ProcessPendingTasks(void);

// =========================================================
// Fast Inline Accessors for High-Frequency ISRs
// =========================================================

/**
 * @brief  Writes the raw ADC value to the device context with zero-overhead.
 * @param  raw_val Raw 12-bit ADC value.
 * @note   Inlined directly into the ISR to eliminate function call penalties.
 */
static inline __attribute__((always_inline)) void MimicDevice_SetAdcVal_ISR(uint16_t raw_val) {
    extern MimicDevice_t mimic_device;
    mimic_device.adc_val = raw_val;
}

/**
 * @brief  ORs the status register with the specified error flags.
 * @param  error_flag Transient or persistent error flag to set.
 */
static inline __attribute__((always_inline)) void MimicDevice_SetErrorFlag_ISR(uint8_t error_flag) {
    extern MimicDevice_t mimic_device;
    mimic_device.status |= error_flag;
}

/**
 * @brief  Updates the maximum recorded CPU cycles if the new value is higher.
 * @param  cycles Measured CPU cycles for the current execution.
 */
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
