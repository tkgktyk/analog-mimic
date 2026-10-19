/**
 ******************************************************************************
 * @file    mimic_i2c.c
 * @author  TAKAGI Katsuyuki
 * @brief   Implementation of I2C Peripheral Interrupt Handlers (Secondary ISR), 
 *          Packet Deserialization, and Runtime Parameter Hot-Swapping.
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

// =========================================================
// I2C Transaction State (Exclusive to I2C1_IRQHandler)
// =========================================================
// Note: 'volatile' is NOT required here because these variables are accessed
// strictly within I2C1_IRQHandler (never shared across threads or other ISRs).
// Hand-off to the main loop occurs explicitly upon STOP condition detection.
static uint8_t tx_reg_addr;
static bool    tx_addr_received;
static bool    tx_reg_modified;
static uint8_t tx_latched_cmd;

// =========================================================
// I2C Register Access Interfaces
// =========================================================

/**
 * @brief Writes a value to a specified I2C shadow register.
 * @param addr The register address to write to.
 * @param val  The value to write.
 * @return true if successful, false if the access is out of bounds or read-only.
 */
static bool PerformWriteReg(uint8_t addr, uint8_t val)
{
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
static uint8_t PerformReadReg(uint8_t addr)
{
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
        return MimicDevice_ReadReg(addr);
    }

    // 3. Out of bounds access (0x40 and above)
    MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
    return I2C_READ_ERROR_VALUE;
}

// =========================================================
// Initialization
// =========================================================

void MimicI2C_Init(void)
{
    tx_reg_addr      = 0;
    tx_addr_received = false;
    tx_reg_modified  = false;
    tx_latched_cmd   = MIMIC_CMD_NOP;
}

// =========================================================
// I2C Low-Layer Interrupt Handler
// =========================================================

void I2C1_IRQHandler(void)
{
    // 1. ADDR: Address matched (Communication started)
    if (LL_I2C_IsActiveFlag_ADDR(I2C1)) {
        // Read SR1 then SR2 sequentially to clear ADDR flag and latch direction
        __IO uint32_t sr1_val = I2C1->SR1;
        __IO uint32_t sr2_val = I2C1->SR2;
        (void)sr1_val;

        // Direction analysis (SR2_TRA: 0 = Slave receives/Write, 1 = Slave transmits/Read)
        if ((sr2_val & I2C_SR2_TRA) == 0) {
            tx_addr_received = false;
        }
    }

    // 2. RXNE: Receive buffer not empty (Data byte received from Master)
    if (LL_I2C_IsActiveFlag_RXNE(I2C1)) {
        uint8_t rx_data = LL_I2C_ReceiveData8(I2C1);

        if (!tx_addr_received) {
            // Byte 1 represents the target register address pointer
            tx_reg_addr      = rx_data;
            tx_addr_received = true;
        } else {
            // Byte 2+ contain payload data; push to shadow register or command port
            if (tx_reg_addr < MIMIC_REG_SHADOW_SIZE) {
                if (PerformWriteReg(tx_reg_addr, rx_data)) {
                    tx_reg_modified = true;
                }
                tx_reg_addr++;
            } else if (tx_reg_addr == MIMIC_REG_CMD_PORT) {
                tx_latched_cmd = rx_data;
                tx_reg_addr++;
            } else {
                MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
            }
        }
    }

    // 3. AF / TXE: Master Read handling
    if (LL_I2C_IsActiveFlag_AF(I2C1)) {
        LL_I2C_ClearFlag_AF(I2C1);
    } else if (LL_I2C_IsActiveFlag_TXE(I2C1)) {
        uint8_t tx_data = PerformReadReg(tx_reg_addr);
        LL_I2C_TransmitData8(I2C1, tx_data);

        if (tx_reg_addr < MIMIC_REG_SHADOW_SIZE) {
            tx_reg_addr++;
        }
    }

    // 4. STOPF: Stop condition detected (Transaction finished -> Notify Main Loop)
    if (LL_I2C_IsActiveFlag_STOP(I2C1)) {
        LL_I2C_ClearFlag_STOP(I2C1);

        if (tx_reg_modified) {
            MimicDevice_RequestUpdate();
            tx_reg_modified = false;
        }

        if (tx_latched_cmd != MIMIC_CMD_NOP) {
            MimicDevice_RequestSystemCommand(tx_latched_cmd);
            tx_latched_cmd = MIMIC_CMD_NOP;
        }
    }

    // 5. Error Handlers (Bus Error, Arbitration Lost, Overrun)
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
