/**
 ******************************************************************************
 * @file    mimic_device.c
 * @author  TAKAGI Katsuyuki
 * @brief   Implementation of Global Device Context, Subsystem Initialization, 
 *          and Thread-Safe Critical Section Accessor Routines.
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
#include <string.h>
#include "py32f0xx_hal.h"
#include "mimic0x_msp.h"
#include "mimic_flash.h"
#include "mimic_dsp.h"
#include "mimic_i2c.h"

// =========================================================
// Configuration Macros
// =========================================================

/**
 * @brief  Executes a code block with interrupts disabled, restoring the original 
 *         interrupt state (PRIMASK) upon completion.
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

MimicDevice_t mimic_device = {
    .registers              = {0},
    .status                 = 0x00,
    .adc_val                = MIMIC_ADC_MIN_VALUE,
    .cpu_cycles_max         = 0,
    .pending_dsp_update     = false,
    .pending_system_command = MIMIC_CMD_NOP
};

// =========================================================
// Private Helper Functions
// =========================================================

static void LoadCalibration(void)
{
    uint16_t gain_q15 = MimicFlash_ReadGainQ15Data();
    mimic_device.registers[MIMIC_REG_NVM_GAIN_Q15_H] = (uint8_t)(gain_q15 >> 8);
    mimic_device.registers[MIMIC_REG_NVM_GAIN_Q15_L] = (uint8_t)(gain_q15 & 0xFF);

    int16_t offset = MimicFlash_ReadOffsetData();
    mimic_device.registers[MIMIC_REG_NVM_OFFSET_H] = (uint8_t)(offset >> 8);
    mimic_device.registers[MIMIC_REG_NVM_OFFSET_L] = (uint8_t)(offset & 0xFF);
}

static inline __attribute__((always_inline)) uint16_t ReadRegU16(uint8_t addr)
{
    return _MimicDevice_ReadU16_BE(&mimic_device.registers[addr]);
}

static inline __attribute__((always_inline)) int16_t ReadRegS16(uint8_t addr)
{
    return _MimicDevice_ReadS16_BE(&mimic_device.registers[addr]);
}

static MimicOutputRoute_t ResolveOutputRoute(uint8_t mode_id, uint8_t global_flags)
{
    if (global_flags & MIMIC_FLAG_OUT_OPEN) {
        return MIMIC_OUT_OPEN;
    }
    if (mode_id == MIMIC_MODE_ID_COMPARATOR_SCHMITT ||
        mode_id == MIMIC_MODE_ID_COMPARATOR_WIN) {
        return MIMIC_OUT_DIGITAL;
    }
    return MIMIC_OUT_ANALOG;
}

static void UpdateDSPConfiguration(void)
{
    // 1. Stop sampling triggers/DMA and isolate the analog/digital output node
    Mimic_HW_StopEngine();
    Mimic_HW_SetOutputRoute(MIMIC_OUT_OPEN);

    // 2. Snapshot shadow registers into a cohesive DSP configuration struct
    MimicDSP_Config_t config;
    config.mode_id      = MimicDevice_ReadReg(MIMIC_REG_MODE_ID);
    config.global_flags = MimicDevice_ReadReg(MIMIC_REG_GLOBAL_FLAGS);
    config.output_route = ResolveOutputRoute(config.mode_id, config.global_flags);
    config.decimation_n = MimicDevice_ReadReg(MIMIC_REG_GLOBAL_DECIMATION_N);
    config.gain_q15     = (int32_t)ReadRegU16(MIMIC_REG_NVM_GAIN_Q15);
    config.offset_raw   = ReadRegS16(MIMIC_REG_GLOBAL_OFFSET) + ReadRegS16(MIMIC_REG_NVM_OFFSET);

    memcpy(config.payload,
           (const void *)&mimic_device.registers[MIMIC_REG_MODE_PAYLOAD_START],
           MIMIC_DSP_PAYLOAD_SIZE);

    // 3. Apply new parameters to DSP core (also initializes the output slot safely)
    MimicDSP_UpdateParameters(&config);

    // 4. Reset profiling peak counter for the newly configured mode
    MimicDevice_PopCpuCyclesMax();

    // 5. Configure hardware output path and restart deterministic sampling engine
    Mimic_HW_SetOutputRoute(config.output_route);
    Mimic_HW_StartEngine(config.output_route, MimicDSP_GetOutputSlot());
}

static void ExecuteSystemCommand(uint8_t cmd, bool *out_needs_dsp_reconfig)
{
    switch (cmd) {
        case MIMIC_CMD_NVM_COMMIT:
            Mimic_HW_StopEngine();
            MimicFlash_WriteGainQ15Data(ReadRegU16(MIMIC_REG_NVM_GAIN_Q15));
            MimicFlash_WriteOffsetData(ReadRegS16(MIMIC_REG_NVM_OFFSET));
            Mimic_HW_StartEngine(
                ResolveOutputRoute(MimicDevice_ReadReg(MIMIC_REG_MODE_ID),
                                   MimicDevice_ReadReg(MIMIC_REG_GLOBAL_FLAGS)),
                MimicDSP_GetOutputSlot()
            );
            break;

        case MIMIC_CMD_NVM_RELOAD:
            LoadCalibration();
            *out_needs_dsp_reconfig = true;
            break;

        default:
            break;
    }
}

// =========================================================
// Public Initialization and Management APIs
// =========================================================

void MimicDevice_Init(void)
{
    MimicI2C_Init();
    MimicDSP_Init();
    LoadCalibration();

    // Apply default configuration and start the sampling engine
    UpdateDSPConfiguration();
}

void MimicDevice_SetStatusFlag(uint8_t flag)
{
    CRITICAL_SECTION_BLOCK(
        mimic_device.status |= flag;
    );
}

uint8_t MimicDevice_GetAndClearStatusFlag(void)
{
    uint8_t current_status;

    CRITICAL_SECTION_BLOCK(
        current_status = mimic_device.status;
        // Retain active state flags (e.g., SYS_READY) but clear transient errors
        mimic_device.status &= MIMIC_STATUS_STATE_MASK;
    );

    return current_status;
}

void MimicDevice_ProcessPendingTasks(void)
{
    uint8_t cmd = MIMIC_CMD_NOP;
    bool needs_dsp_update = false;

    CRITICAL_SECTION_BLOCK(
        cmd = mimic_device.pending_system_command;
        mimic_device.pending_system_command = MIMIC_CMD_NOP;

        needs_dsp_update = mimic_device.pending_dsp_update;
        mimic_device.pending_dsp_update = false;
    );

    if (cmd != MIMIC_CMD_NOP) {
        ExecuteSystemCommand(cmd, &needs_dsp_update);
    }

    if (needs_dsp_update) {
        UpdateDSPConfiguration();
    }
}
