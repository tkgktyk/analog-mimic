/**
 ******************************************************************************
 * @file    mimic_i2c.h
 * @author  TAKAGI Katsuyuki
 * @brief   I2C Peripheral Interrupt Service Routine (Secondary ISR) and 
 * Host-Communication Command Packet Deserialization Interfaces.
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

#ifndef MIMIC_I2C_H
#define MIMIC_I2C_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================
// Public API Implementations
// =========================================================

/**
 * @brief Initializes the I2C request handler and registry system states.
 */
void MimicI2C_Init(void);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_I2C_H
