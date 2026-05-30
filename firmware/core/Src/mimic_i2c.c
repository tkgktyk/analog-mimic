/**
 ******************************************************************************
 * @file    mimic_i2c.c
 * @author  TAKAGI Katsuyuki
 * @brief   Implementation of I2C Peripheral Interrupt Handlers (Secondary ISR), 
 * Packet Deserialization, and Runtime Parameter Hot-Swapping.
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

#include "mimic_i2c.h"
#include "py32f0xx_hal.h"
#include "py32f071_ll_i2c.h"
#include "mimic_device.h"

// =========================================================
// Constants & Macros
// =========================================================
#define I2C_READ_ERROR_VALUE 0xFF

// I2C Reception state (1-byte at a time)
// Note: Static variables are implicitly initialized to 0/false (allocated in .bss)
static uint8_t current_reg_addr;
static bool is_reg_addr_received;
static volatile bool is_dsp_update_required;
static volatile uint8_t latched_system_command;

// =========================================================
// I2C Register Access Interfaces
// =========================================================

/**
 * @brief Writes a value to a specified I2C shadow register.
 * @param addr The register address to write to.
 * @param val The value to write.
 * @return true if successful, false if the access is out of bounds or read-only.
 */
static bool PerformWriteReg(uint8_t addr, uint8_t val) {
    // Valid writable memory map bounds: Reflect changes directly to shadow RAM
    if (addr >= MIMIC_REG_GLOBAL_START && addr < MIMIC_REG_SHADOW_SIZE) {
        MimicDevice_WriteReg(addr, val);
        return true;
    }

    // Reject out-of-bounds access or attempts to write to the Read-Only region (0x00 - 0x0F)
    MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
    return false;
}

/**
 * @brief Reads a value from a specified I2C register or internal virtual register.
 * @param addr The register address to read from.
 * @return The value of the requested register, or I2C_READ_ERROR_VALUE on error.
 */
static uint8_t PerformReadReg(uint8_t addr) {
    static uint16_t latched_adc_val = 0;
    static uint16_t latched_cpu_cycles = 0;

    // 1. Intercept and resolve specialized virtual registers (Read-Only Info & Telemetry)
    switch (addr) {
        // --- Static System Information (0x00 - 0x02) ---
        case MIMIC_REG_SYSTEM_ID:
            return MIMIC_DEVICE_ID_VALUE;
        case MIMIC_REG_SYSTEM_HW_VERSION:
            return MIMIC_HW_VERSION_VALUE; // Injected globally via Makefile
        case MIMIC_REG_SYSTEM_FW_VERSION:
            return MIMIC_FW_VERSION_VALUE; // Injected globally via Makefile

        // --- Dynamic Telemetry (0x03) ---
        case MIMIC_REG_SYSTEM_STATUS:
            return MimicDevice_GetAndClearStatusFlag();

        // --- 16-bit Bound Live ADC Sample Read Out (0x04 - 0x05) ---
        case MIMIC_REG_SYSTEM_ADC_VAL_H:
            latched_adc_val = MimicDevice_GetAdcVal(); // Latch atomic context upon high-byte access
            return (uint8_t)(latched_adc_val >> 8);
        case MIMIC_REG_SYSTEM_ADC_VAL_L:
            return (uint8_t)(latched_adc_val & 0xFF);  // Return latched low-byte context safely

        // --- 16-bit Execution Profiler Cycle Count Read Out (0x06 - 0x07) ---
        case MIMIC_REG_SYSTEM_CPU_CYCLES_H:
            latched_cpu_cycles = MimicDevice_PopCpuCyclesMax(); // Fetch peak hold and clear atomicity
            return (uint8_t)(latched_cpu_cycles >> 8);
        case MIMIC_REG_SYSTEM_CPU_CYCLES_L:
            return (uint8_t)(latched_cpu_cycles & 0xFF);

        default:
            break;
    }

    // 2. Handle all remaining spaces (Global, Mode, Payload, NVM) via standard memory map
    if (addr < MIMIC_REG_SHADOW_SIZE) {
        // Automatically returns shadow RAM values for addresses between 0x10 and 0x3F.
        // This implicitly covers the NVM calibration read-back region as well.
        return MimicDevice_ReadReg(addr);
    }

    // 3. Out of bounds access (0x40 and above)
    MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
    return I2C_READ_ERROR_VALUE;
}

// =========================================================
// Initialization
// =========================================================

/**
 * @brief Initializes the I2C request handler variables.
 * @note  Default state defaults to 0 (unity gain / through mode) via system reset.
 */
void MimicI2C_Init(void) {
    current_reg_addr = 0;
    is_reg_addr_received = false;
}

// =========================================================
// I2C Low-Layer Interrupt Handler (Replaces HAL Callbacks)
// =========================================================

/**
 * @brief I2C Hardware Interrupt Entry Point
 */
void I2C1_IRQHandler(void) {
  
    // 1. ADDR: Address matched (Communication started)
    if (LL_I2C_IsActiveFlag_ADDR(I2C1)) {
        // [CRITICAL] Before clearing the ADDR flag, read SR2 to determine direction.
        // Reading SR2 serves as the latter half of the hardware clear sequence.
        // To ensure atomicity and avoid race conditions, read SR1 then SR2 sequentially.
        __IO uint32_t sr1_val = I2C1->SR1; // Start clear sequence
        __IO uint32_t sr2_val = I2C1->SR2; // Complete clear sequence and latch direction

        // Direction analysis (SR2_TRA: Transmitter/receiver direction flag)
        // 0: Read (Slave receives), 1: Write (Slave transmits)
        if ((sr2_val & I2C_SR2_TRA) == 0) {
            // Master requests a WRITE operation (Slave receives data)
            // The incoming initial byte must be interpreted as the target register address.
            is_reg_addr_received = false;
        } else {
            // Master requests a READ operation (Slave transmits data)
            // Prepare transmission buffer based on the pre-latched current_reg_addr.
            // This will be processed shortly by the subsequent TXE flag block.
        }
        
        // Suppress compiler unused-variable warning safely
        (void)sr1_val; 
    }

    // 2. RXNE: Receive buffer not empty (Data package received from Master)
    if (LL_I2C_IsActiveFlag_RXNE(I2C1)) {
        uint8_t rx_data = LL_I2C_ReceiveData8(I2C1);
        
        if (is_reg_addr_received == false) {
            // Byte 1 represents the operation target register address pointer
            current_reg_addr = rx_data;
            is_reg_addr_received = true;
        } else {
            // Byte 2 and subsequent bytes contain payload data; push to shadow register
            if (current_reg_addr < MIMIC_REG_SHADOW_SIZE) {
                if (PerformWriteReg(current_reg_addr, rx_data)) {
                    // Raise update trigger indicating a DSP parameter reconfiguration
                    is_dsp_update_required = true; 
                }
                current_reg_addr++;
            } else if (current_reg_addr == MIMIC_REG_CMD_PORT) {
                // Command processing interface (outside shadow RAM limits)
                latched_system_command = rx_data;
                current_reg_addr++;
            } else {
                MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
            }
        }
    }

    // 3. TXE: Transmit buffer empty (Master requests data read out)
    // Process transmission setup only if the BTF (Byte Transfer Finished) flag is clear
    if (LL_I2C_IsActiveFlag_TXE(I2C1) && (LL_I2C_IsActiveFlag_BTF(I2C1) == 0)) {
        uint8_t tx_data = PerformReadReg(current_reg_addr);
        LL_I2C_TransmitData8(I2C1, tx_data);
        
        // Automatic address pointer auto-increment
        if (current_reg_addr < MIMIC_REG_SHADOW_SIZE) {
            current_reg_addr++;
        }
    }

    // 4. STOPF: Stop condition detected (Transaction finished)
    if (LL_I2C_IsActiveFlag_STOP(I2C1)) {
        LL_I2C_ClearFlag_STOP(I2C1);
        
        // Notify the main thread if any runtime parameters were altered during transaction
        if (is_dsp_update_required) {
            MimicDevice_RequestUpdate();
            is_dsp_update_required = false;
        }

        // Defer system commands execution to the main background loop
        if (latched_system_command != MIMIC_CMD_NOP) {
            MimicDevice_RequestSystemCommand(latched_system_command);
            latched_system_command = MIMIC_CMD_NOP;
        }
    }

    // 5. AF: Acknowledge Failure (Master signals NACK to finalize Slave Read operation)
    if (LL_I2C_IsActiveFlag_AF(I2C1)) {
        LL_I2C_ClearFlag_AF(I2C1);
    }

    // 6. Comprehensive Error Handlers (Bus Error, Arbitration Lost, Overrun)
    if (LL_I2C_IsActiveFlag_BERR(I2C1)) {
        LL_I2C_ClearFlag_BERR(I2C1);
        MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
    }
    if (LL_I2C_IsActiveFlag_ARLO(I2C1)) {
        LL_I2C_ClearFlag_ARLO(I2C1);
        MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
    }
    if (LL_I2C_IsActiveFlag_OVR(I2C1)) {
        LL_I2C_ClearFlag_OVR(I2C1);
        MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
    }
}
