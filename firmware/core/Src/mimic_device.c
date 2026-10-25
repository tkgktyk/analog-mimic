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
// MCU unique ID (PY32F071 RM: UID area at 0x1FFF3000)
#ifndef UID_BASE
#define UID_BASE        0x1FFF3000UL
#endif
#define UID_HASH_BYTES  16U          // Hash the first 128 bits of the UID area

// Firmware build date as hex 0xYYYYMMDD, taken from the compiler's __DATE__ ("Oct 21 2026")
#define DATE_DIGIT(i)   ((uint32_t)((__DATE__[i] == ' ') ? 0 : (__DATE__[i] - '0')))
#define DATE_IS(a, b, c) (__DATE__[0] == (a) && __DATE__[1] == (b) && __DATE__[2] == (c))
#define DATE_MONTH \
    (DATE_IS('J','a','n') ? 0x01 : DATE_IS('F','e','b') ? 0x02 : DATE_IS('M','a','r') ? 0x03 : \
     DATE_IS('A','p','r') ? 0x04 : DATE_IS('M','a','y') ? 0x05 : DATE_IS('J','u','n') ? 0x06 : \
     DATE_IS('J','u','l') ? 0x07 : DATE_IS('A','u','g') ? 0x08 : DATE_IS('S','e','p') ? 0x09 : \
     DATE_IS('O','c','t') ? 0x10 : DATE_IS('N','o','v') ? 0x11 : 0x12)
#define BUILD_DATE_HEX \
    ((DATE_DIGIT(7) << 28) | (DATE_DIGIT(8) << 24) | (DATE_DIGIT(9) << 20) | (DATE_DIGIT(10) << 16) | \
     ((uint32_t)DATE_MONTH << 8) | (DATE_DIGIT(4) << 4) | DATE_DIGIT(5))

#define FNV1A_32_OFFSET 2166136261UL
#define FNV1A_32_PRIME  16777619UL

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
    uint16_t gain_uq1_15 = MimicFlash_ReadGainData();
    mimic_device.registers[MIMIC_REG_NVM_GAIN_H] = (uint8_t)(gain_uq1_15 >> 8);
    mimic_device.registers[MIMIC_REG_NVM_GAIN_L] = (uint8_t)(gain_uq1_15 & 0xFF);

    int16_t offset_raw_fx3 = MimicFlash_ReadOffsetData();
    mimic_device.registers[MIMIC_REG_NVM_OFFSET_H] = (uint8_t)(offset_raw_fx3 >> 8);
    mimic_device.registers[MIMIC_REG_NVM_OFFSET_L] = (uint8_t)(offset_raw_fx3 & 0xFF);
}

static void StoreU32_BE(uint8_t addr, uint32_t v)
{
    mimic_device.registers[addr]     = (uint8_t)(v >> 24);
    mimic_device.registers[addr + 1] = (uint8_t)(v >> 16);
    mimic_device.registers[addr + 2] = (uint8_t)(v >> 8);
    mimic_device.registers[addr + 3] = (uint8_t)(v & 0xFF);
}

/**
 * @brief  Stores the static system information (build date and unit ID) in the
 *         read-only system registers. The unit ID is a 32-bit FNV-1a hash of the
 *         MCU UID; the same chip always gives the same value.
 */
static void LoadSystemInfo(void)
{
    StoreU32_BE(MIMIC_REG_SYSTEM_BUILD_DATE, BUILD_DATE_HEX);

    const volatile uint8_t *uid = (const volatile uint8_t *)UID_BASE;
    uint32_t hash = FNV1A_32_OFFSET;
    for (uint32_t i = 0; i < UID_HASH_BYTES; i++) {
        hash ^= uid[i];
        hash *= FNV1A_32_PRIME;
    }
    StoreU32_BE(MIMIC_REG_SYSTEM_UID, hash);
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
    if (mode_id == MIMIC_MODE_ID_COMPARATOR) {
        return MIMIC_OUT_DIGITAL;
    }
    return MIMIC_OUT_ANALOG;
}

static MimicOutputRoute_t active_route = MIMIC_OUT_OPEN;

static void UpdateDSPConfiguration(void)
{
    // 1. Stop sampling triggers/DMA. The output keeps its last value while reconfiguring;
    //    it is isolated (OPEN) only when the output route itself changes.
    Mimic_HW_StopEngine();

    // 2. Snapshot shadow registers into a cohesive DSP configuration struct
    MimicDSP_Config_t config;
    config.mode_id      = MimicDevice_ReadReg(MIMIC_REG_MODE_ID);
    config.global_flags = MimicDevice_ReadReg(MIMIC_REG_GLOBAL_FLAGS);
    config.output_route = ResolveOutputRoute(config.mode_id, config.global_flags);
    config.decimation_n = MimicDevice_ReadReg(MIMIC_REG_GLOBAL_DECIMATION_N);
    config.gain_uq1_15  = (int32_t)ReadRegU16(MIMIC_REG_NVM_GAIN);
    config.offset_raw_fx3 = (int32_t)ReadRegS16(MIMIC_REG_GLOBAL_OFFSET) + ReadRegS16(MIMIC_REG_NVM_OFFSET);
    config.vref_raw_fx3   = ReadRegS16(MIMIC_REG_GLOBAL_VREF);

    memcpy(config.payload,
           (const void *)&mimic_device.registers[MIMIC_REG_MODE_PAYLOAD_START],
           MIMIC_DSP_PAYLOAD_SIZE);

    bool route_changed = (config.output_route != active_route);
    if (route_changed) {
        Mimic_HW_SetOutputRoute(MIMIC_OUT_OPEN);
    }

    // 3. Apply new parameters to DSP core (also initializes the output slot safely)
    MimicDSP_UpdateParameters(&config);

    // 4. Reset profiling peak counter for the newly configured mode
    MimicDevice_PopCpuCyclesMax();

    // 5. Configure hardware output path (only if changed) and restart the sampling engine
    if (route_changed) {
        Mimic_HW_SetOutputRoute(config.output_route);
        active_route = config.output_route;
    }
    Mimic_HW_StartEngine(config.output_route, MimicDSP_GetOutputSlot());
}

static void ExecuteSystemCommand(uint8_t cmd, bool *out_needs_dsp_reconfig)
{
    switch (cmd) {
        case MIMIC_CMD_NVM_COMMIT:
            Mimic_HW_StopEngine();
            MimicFlash_WriteGainData(ReadRegU16(MIMIC_REG_NVM_GAIN));
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
    LoadSystemInfo();

    // Default common reference: VDD / 2
    mimic_device.registers[MIMIC_REG_GLOBAL_VREF_H] = (uint8_t)((uint16_t)MIMIC_VREF_DEFAULT_RAW_FX3 >> 8);
    mimic_device.registers[MIMIC_REG_GLOBAL_VREF_L] = (uint8_t)((uint16_t)MIMIC_VREF_DEFAULT_RAW_FX3 & 0xFF);

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
