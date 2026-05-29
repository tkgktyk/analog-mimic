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
  volatile uint8_t registers[MIMIC_REG_SHADOW_SIZE]; /**< I2C protocol register memory map */
  volatile uint8_t status;                             /**< Bitfield containing current system flags and transient errors */
  volatile uint16_t adc_val;                           /**< Latest raw data acquired from the ADC input channel */
  volatile uint16_t cpu_cycles_max;                    /**< Peak-hold execution cycle counter for profiling */
  volatile bool i2c_dirty_flag;                     /**< Semaphore flag indicating a parameter update is requested */
  volatile uint8_t pending_system_command;
} MimicDevice_t;

/**
 * @brief Global instantiation of the shared device state context.
 * @note  Exposed for absolute performance (inlining), but DIRECT member access 
 * from other files is strictly prohibited! Use _Inline accessors instead.
 */
extern MimicDevice_t mimic_device;

// =========================================================
// Parameter Decoding Utilities (Big-Endian)
// =========================================================

/**
 * @brief Retrieves an unsigned 16-bit parameter from the device registry.
 */
static inline __attribute__((always_inline)) uint16_t MimicDevice_GetRegU16(uint8_t start_reg) {
    return (uint16_t)((mimic_device.registers[start_reg] << 8) | mimic_device.registers[start_reg + 1]);
}

/**
 * @brief Retrieves a signed 16-bit parameter with guaranteed sign extension.
 */
static inline __attribute__((always_inline)) int32_t MimicDevice_GetRegS16(uint8_t start_reg) {
    return (int32_t)(int16_t)((mimic_device.registers[start_reg] << 8) | mimic_device.registers[start_reg + 1]);
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
uint8_t MimicDevice_GetAndClearStatus(void);

/**
 * @brief Retrieves the latest sampled ADC value from the device context.
 * @return Raw 12-bit ADC data (0 - 4095).
 * @note  This read access is inherently atomic on 32-bit Cortex-M core architectures.
 */
uint16_t MimicDevice_GetAdcVal(void);

/**
 * @brief Retrieves the peak CPU cycle execution count and resets the tracking value to 0.
 * @return The maximum recorded CPU cycles since the previous read operation.
 * @note  This operation is atomic to prevent the sampling ISR from writing intermediate data.
 */
uint16_t MimicDevice_GetAndClearCpuCyclesMax(void);

void MimicDevice_LoadCalibration(void);

/**
 * @brief  Acknowledges the completion of the I2C parameter update transaction.
 * @note   This function safely handles flag manipulation under critical section 
 * and resolves the next physical hardware state required by the DSP context.
 */
void MimicDevice_AcknowledgeUpdate(void);

// =========================================================
// Fast Inline Accessors for High-Frequency ISRs
// =========================================================

/**
 * @brief  Writes the raw ADC value to the device context with zero-overhead.
 * @param  raw_val Raw 12-bit ADC value.
 * @note   Inlined directly into the ISR to eliminate function call penalties.
 */
static inline __attribute__((always_inline)) void MimicDevice_SetAdcVal_Inline(uint16_t raw_val) {
    mimic_device.adc_val = raw_val;
}

/**
 * @brief  Ors the status register with the specified error flags.
 * @param  error_flag Transient or persistent error flag to set.
 */
static inline __attribute__((always_inline)) void MimicDevice_SetErrorFlag_Inline(uint8_t error_flag) {
    mimic_device.status |= error_flag;
}

/**
 * @brief  Updates the maximum recorded CPU cycles if the new value is higher.
 * @param  cycles Measured CPU cycles for the current execution.
 */
static inline __attribute__((always_inline)) void MimicDevice_UpdateCpuCyclesMax_Inline(uint32_t cycles) {
    if (cycles > mimic_device.cpu_cycles_max) {
        mimic_device.cpu_cycles_max = (uint16_t)cycles;
    }
}

#ifdef __cplusplus
}
#endif

#endif // MIMIC_DEVICE_H
