/**
 ******************************************************************************
 * @file    mimic_flash.h
 * @author  TAKAGI Katsuyuki
 * @brief   NVM (Non-Volatile Memory) Flash Calibration Data Storage API.
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

#ifndef MIMIC_FLASH_H
#define MIMIC_FLASH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================
// Flash Calibration Access APIs
// =========================================================

/**
 * @brief  Writes the Q15 fractional gain calibration value to Flash memory.
 * @param  gain_q15 The 16-bit unsigned Q15 gain multiplier to store.
 * @return true if the Flash write operation was successful, false otherwise.
 */
bool MimicFlash_WriteGainQ15Data(uint16_t gain_q15);

/**
 * @brief  Reads the Q15 fractional gain calibration value from Flash memory.
 * @return The stored 16-bit Q15 gain value. Returns a default value if uninitialized.
 */
uint16_t MimicFlash_ReadGainQ15Data(void);

/**
 * @brief  Writes the raw ADC offset calibration value to pseudo-EEPROM (Flash page).
 * @param  offset The 16-bit signed offset value to store.
 * @return true if the Flash write operation was successful, false otherwise.
 */
bool MimicFlash_WriteOffsetData(int16_t offset);

/**
 * @brief  Reads the raw ADC offset calibration value from Flash memory.
 * @return The stored 16-bit signed offset value. Returns a default value if uninitialized.
 */
int16_t MimicFlash_ReadOffsetData(void);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_FLASH_H
