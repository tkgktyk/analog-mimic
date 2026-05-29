/**
 ******************************************************************************
 * @file    mimic_device.c
 * @author  TAKAGI Katsuyuki
 * @brief   Implementation of Global Device Context, Subsystem Initialization, 
 * and Thread-Safe Critical Section Accessor Routines.
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

#include "mimic_device.h"
#include "py32f0xx_hal.h"
#include "mimic_flash.h"

/**
 * @brief  Executes a code block with interrupts disabled, restoring the original 
 * interrupt state (PRIMASK) upon completion.
 * @note   This macro handles the nested state preservation safely within a do-while block.
 */
#define CRITICAL_SECTION_BLOCK(block_code) do { \
    uint32_t __primask_save = __get_PRIMASK();  \
    __disable_irq();                            \
    { block_code }                              \
    __set_PRIMASK(__primask_save);              \
} while(0)

// =========================================================
// Global Device State Instantiation
// =========================================================
// Defines and initializes the single, system-wide global device state structure.
MimicDevice_t mimic_device = {
    .registers = {0},
    .status = 0x00,
    .adc_val = 2048, // Initialized to mid-scale analog ground (Vref/2) for safety
    .cpu_cycles_max = 0,
    .i2c_dirty_flag = false,
    .pending_system_command = MIMIC_CMD_NOP};

// =========================================================
// Thread-Safe State Management APIs
// =========================================================

/**
 * @brief Thread-safe setter for status flags.
 * Prevents data races when multiple interrupts try to update the status
 * register simultaneously.
 */
void MimicDevice_SetStatusFlag(uint8_t flag) {
  CRITICAL_SECTION_BLOCK(
    mimic_device.status |= flag;
  );
}


/**
 * @brief Retrieves the status and clears transient error flags atomically.
 * Called primarily when the I2C master reads the STATUS register.
 */
uint8_t MimicDevice_GetAndClearStatusFlag(void) {
  uint8_t current_status;
  
  CRITICAL_SECTION_BLOCK(
    current_status = mimic_device.status;
    // Retain active state flags (e.g., SYS_READY) but clear transient errors
    mimic_device.status &= MIMIC_STATUS_STATE_MASK;
  );  

  return current_status;
}

void MimicDevice_LoadCalibration(void) {
  uint16_t gain_q15 = MimicFlash_ReadGainQ15Data();
  mimic_device.registers[MIMIC_REG_NVM_GAIN_Q15_H] = (uint8_t)(gain_q15 >> 8);
  mimic_device.registers[MIMIC_REG_NVM_GAIN_Q15_L] = (uint8_t)(gain_q15 & 0xFF);
  int16_t offset = MimicFlash_ReadOffsetData();
  mimic_device.registers[MIMIC_REG_NVM_OFFSET_H] = (uint8_t)(offset >> 8);
  mimic_device.registers[MIMIC_REG_NVM_OFFSET_L] = (uint8_t)(offset & 0xFF);
}
