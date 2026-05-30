/**
 ******************************************************************************
 * @file    mimic_flash.c
 * @author  TAKAGI Katsuyuki
 * @brief   Implementation of Non-Volatile Memory (NVM) calibration data 
 * storage operations using Flash page erase and write routines.
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

#include "mimic_flash.h"
#include <string.h>
#include "py32f0xx_hal.h"
#include "mimic_registers.h"

// =========================================================
// Configuration Macros
// =========================================================

#define MIMIC_FLASH_DEFAULT_VALUE_TWO_BYTES 0xFFFF

#define MIMIC_FLASH_CALIB_PAGE_ADDR  ((FLASH_END) - (FLASH_PAGE_SIZE) + 1)

// Note: Flash offset addresses must be strictly aligned to even boundaries
#define MIMIC_FLASH_GAIN_Q15_ADDR ((MIMIC_REG_NVM_GAIN_Q15) - (MIMIC_REG_NVM_START))
#define MIMIC_FLASH_OFFSET_ADDR   ((MIMIC_REG_NVM_OFFSET) - (MIMIC_REG_NVM_START))

// Default fallback values when flash is empty (0xFFFF)
#define MIMIC_DEFAULT_GAIN_Q15    (1 << 15)
#define MIMIC_DEFAULT_OFFSET      0

// =========================================================
// Private Flash Operations
// =========================================================

/**
 * @brief  Reads exactly one flash page into a volatile RAM buffer.
 * @param  addr Target Flash address.
 * @param  ram_page_buffer Pointer to the RAM buffer (must be at least FLASH_PAGE_SIZE).
 */
static void ReadPage(uint32_t addr, uint8_t *ram_page_buffer) {
    // Copy the entire 128-byte page directly from Flash to the RAM buffer
    memcpy(ram_page_buffer, (const uint8_t *)addr, FLASH_PAGE_SIZE);
}

/**
 * @brief  Erases a specified flash page and writes the updated RAM buffer.
 * Executes within a strict critical section to prevent ISR interference.
 * @param  addr Target Flash address to erase and write.
 * @param  ram_page_buffer Pointer to the modified RAM buffer.
 * @return true if successful, false otherwise.
 */
static bool WritePage(uint32_t addr, const uint8_t *ram_page_buffer) {
    HAL_StatusTypeDef status = HAL_OK;
    uint32_t page_error = 0;
    FLASH_EraseInitTypeDef EraseInitStruct = {0};

    // Build the erase configuration
    EraseInitStruct.TypeErase   = FLASH_TYPEERASE_PAGEERASE; // Select page erase mode
    EraseInitStruct.PageAddress = addr;                      // Target address
    EraseInitStruct.NbPages     = 1;                         // Erase only a single page

    // ================== Begin Critical Section ================== 
    uint32_t __primask_save = __get_PRIMASK();
    __disable_irq(); 

    // 1. Unlock the Flash memory for writing
    HAL_FLASH_Unlock();

    // 2. Execute page erase
    if (HAL_FLASHEx_Erase(&EraseInitStruct, &page_error) == HAL_OK) {
        // 3. If erase is successful, program the updated 128-byte buffer at once.
        // FLASH_TYPEPROGRAM_PAGE strictly targets 128-byte blocks on PY32F071.
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_PAGE, addr, (uint32_t *)ram_page_buffer);
    } else {
        status = HAL_ERROR;
    }

    // 4. Re-lock the Flash memory to prevent accidental writes
    HAL_FLASH_Lock();

    __set_PRIMASK(__primask_save);
    // ================== End Critical Section ================== 

    return (status == HAL_OK);
}

/**
 * @brief  Common internal implementation to safely write partial calibration data.
 * @param  page_offset Byte offset from the start of the Flash page.
 * @param  data        Pointer to the specific payload to write.
 * @param  size        Byte size of the payload.
 * @return true if successful, false otherwise.
 */
static bool WriteCalibrationData(uint32_t page_offset, const void *data, size_t size) {
    // Declare a 32-bit array to guarantee word alignment. Array size is 1/4 of total bytes.
    uint32_t ram_page_buffer[FLASH_PAGE_SIZE / 4];
    uint32_t addr = MIMIC_FLASH_CALIB_PAGE_ADDR;

    // 1. Snapshot the current flash content directly into the RAM buffer
    ReadPage(addr, (uint8_t *)ram_page_buffer);

    // 2. Overwrite only the specific region inside the RAM buffer (with overrun guard)
    if (page_offset + size <= FLASH_PAGE_SIZE) {
        uint8_t *byte_ptr = (uint8_t *)ram_page_buffer;
        memcpy(&byte_ptr[page_offset], data, size);
    } else {
        return false; // Reject operations that exceed page boundaries
    }

    // 3. Erase and write the updated buffer atomically
    return WritePage(addr, (const uint8_t *)ram_page_buffer);
}

// =========================================================
// Public API Implementations
// =========================================================

bool MimicFlash_WriteGainQ15Data(uint16_t gain) {
    return WriteCalibrationData(MIMIC_FLASH_GAIN_Q15_ADDR, &gain, sizeof(gain));
}

uint16_t MimicFlash_ReadGainQ15Data(void) {
    // Read directly from Flash space using pointer arithmetic
    uint16_t gain_q15 = *((uint16_t *)(MIMIC_FLASH_CALIB_PAGE_ADDR + MIMIC_FLASH_GAIN_Q15_ADDR));
    
    if (gain_q15 == MIMIC_FLASH_DEFAULT_VALUE_TWO_BYTES) {
        gain_q15 = MIMIC_DEFAULT_GAIN_Q15;
    }
    
    return gain_q15;
}

bool MimicFlash_WriteOffsetData(int16_t offset) {
    return WriteCalibrationData(MIMIC_FLASH_OFFSET_ADDR, &offset, sizeof(offset));
}

int16_t MimicFlash_ReadOffsetData(void) {
    // Read directly from Flash space using pointer arithmetic
    uint16_t raw = *((uint16_t *)(MIMIC_FLASH_CALIB_PAGE_ADDR + MIMIC_FLASH_OFFSET_ADDR));
    
    if (raw == MIMIC_FLASH_DEFAULT_VALUE_TWO_BYTES) {
        raw = MIMIC_DEFAULT_OFFSET;
    }
    
    return (int16_t)raw;
}
