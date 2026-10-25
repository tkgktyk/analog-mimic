/**
 ******************************************************************************
 * @file    mimic_dsp.h
 * @author  TAKAGI Katsuyuki
 * @brief   DSP Core Engine Interface and Mathematical Model Pipeline.
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

#ifndef MIMIC_DSP_H
#define MIMIC_DSP_H

#include <stdint.h>
#include "mimic_registers.h"
#include "mimic0x_msp.h"

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================
// Configuration Structures & Macros
// =========================================================

#define MIMIC_DSP_PAYLOAD_SIZE (MIMIC_REG_MODE_PAYLOAD_END - MIMIC_REG_MODE_PAYLOAD_START)

/**
 * @brief Encapsulates the complete configuration state required by the DSP core.
 * @note  Members are ordered by data size (32-bit first, then 8-bit) to minimize
 *        implicit compiler memory padding and optimize alignment.
 */
typedef struct {
    int32_t            gain_uq1_15;                     /**< Output gain calibration, uq1_15 (1.0 = 32768) */
    int32_t            offset_raw_fx3;                  /**< Output DC offset, raw_fx3 (user + calibration) */
    int32_t            vref_raw_fx3;                    /**< Common reference voltage (Vref), raw_fx3 */
    uint8_t            mode_id;                         /**< Active operational mode ID */
    MimicOutputRoute_t output_route;                    /**< Physical output path (OPEN, ANALOG, DIGITAL) */
    uint8_t            global_flags;                    /**< System-wide control flags (e.g., inversion, test pulse) */
    uint8_t            decimation_n;                    /**< Decimation shift factor (N) for sample rate reduction */
    uint8_t            payload[MIMIC_DSP_PAYLOAD_SIZE]; /**< Mode-specific parameter payload data */
} MimicDSP_Config_t;

// =========================================================
// Core DSP Interface APIs
// =========================================================

/**
 * @brief  Initializes the DSP core memory space and internal state variables.
 */
void MimicDSP_Init(void);

/**
 * @brief  Returns the memory address of the 32-bit deterministic output slot.
 * @note   Used exclusively by the hardware engine to bind the DMA source pointer.
 *         Initial slot values are managed internally by MimicDSP_UpdateParameters().
 */
volatile uint32_t *MimicDSP_GetOutputSlot(void);

/**
 * @brief  Configures the internal oversampling decimation filter.
 * @param  decimation_n The decimation shift factor (2^n). 0 disables decimation.
 */
void MimicDSP_SetDecimation(uint8_t decimation_n);

/**
 * @brief  Updates the DSP mathematical parameters safely, switches operational modes,
 *         and seeds the output slot according to the target output route.
 * @param  config Pointer to the newly constructed DSP configuration context.
 */
void MimicDSP_UpdateParameters(const MimicDSP_Config_t *config);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_DSP_H
