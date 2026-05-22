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

#ifndef MIMIC_I2C_REQ_H
#define MIMIC_I2C_REQ_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================
// Shared Function Prototypes
// =========================================================

/**
 * @brief Initializes the I2C request handler and registry system states.
 */
void MimicI2c_Init(void);

/**
 * @brief  Fast Low-Layer I2C1 Interrupt Service Routine block.
 * @note   Bypasses standard HAL callback overhead to process I2C events 
 * and errors deterministically.
 */
void MimicI2c_IRQHandler_LL(void);

/**
 * @brief Writes a value to a specified I2C shadow register.
 * @param reg The register address to write to.
 * @param val The value to write.
 * @return true if the write was successful, false if out of bounds.
 */
bool MimicI2c_WriteReg(uint8_t reg, uint8_t val);

/**
 * @brief Reads a value from a specified I2C register or internal virtual register.
 * @param reg The register address to read from.
 * @return The value of the requested register, or 0xFF on error.
 */
uint8_t MimicI2c_ReadReg(uint8_t reg);

/**
 * @brief Decodes 4 bytes transmitted from the I2C master in Big-Endian (MSB First) format.
 * @param base_addr The starting address in the register array.
 * @return The decoded 32-bit unsigned integer.
 */
uint32_t MimicI2c_GetParam32(uint8_t base_addr);

/**
 * @brief Decodes 2 bytes transmitted from the I2C master in Big-Endian (MSB First) format.
 * @param base_addr The starting address in the register array.
 * @return The decoded 16-bit unsigned integer.
 */
uint16_t MimicI2c_GetParam16(uint8_t base_addr);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_I2C_REQ_H
