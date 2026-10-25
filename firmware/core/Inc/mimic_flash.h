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

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================
// Flash Calibration Access APIs
// =========================================================

/**
 * @brief  Writes the gain calibration value to Flash memory.
 * @param  gain_uq1_15 Gain multiplier to store (uq1_15, 1.0 = 32768).
 * @return true if the Flash write operation was successful, false otherwise.
 */
bool MimicFlash_WriteGainData(uint16_t gain_uq1_15);

/**
 * @brief  Reads the gain calibration value from Flash memory.
 * @return The stored gain (uq1_15). Returns 1.0 (32768) if uninitialized.
 */
uint16_t MimicFlash_ReadGainData(void);

/**
 * @brief  Writes the offset calibration value to Flash memory.
 * @param  offset_raw_fx3 Offset to store (raw_fx3, 1 LSB = 8).
 * @return true if the Flash write operation was successful, false otherwise.
 */
bool MimicFlash_WriteOffsetData(int16_t offset_raw_fx3);

/**
 * @brief  Reads the offset calibration value from Flash memory.
 * @return The stored offset (raw_fx3). Returns 0 if uninitialized.
 */
int16_t MimicFlash_ReadOffsetData(void);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_FLASH_H
