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
#include <stdbool.h>
#include "mimic_registers.h"

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
 * implicit compiler memory padding and optimize alignment.
 */
typedef struct {
    int32_t gain_q15;                           /**< Global Q15 fractional gain multiplier */
    int32_t offset_raw;                         /**< Global output raw DC offset */
    uint8_t mode_id;                            /**< Active operational mode ID */
    uint8_t global_flags;                       /**< System-wide control flags (e.g., inversion, mute) */
    uint8_t decimation_n;                       /**< Decimation shift factor (N) for sample rate reduction */
    uint8_t payload[MIMIC_DSP_PAYLOAD_SIZE];    /**< Mode-specific parameter payload data */
} MimicDSP_Config_t;

// =========================================================
// Core DSP Interface APIs
// =========================================================

/**
 * @brief  Initializes the DSP core memory space and internal state variables.
 * @note   Clears biquad delay lines and resets tracking waveform latches to their defaults.
 */
void MimicDSP_Init(void);

/**
 * @brief  Configures the internal oversampling decimation filter.
 * @param  decimation_n The decimation shift factor (2^n). 0 disables decimation.
 * @note   This function safely resets accumulation buffers to prevent phase jumps.
 */
void MimicDSP_SetDecimation(uint8_t decimation_n);

/**
 * @brief  Updates the DSP mathematical parameters safely and switches operational modes.
 * @param  config Pointer to the newly constructed DSP configuration context.
 * @note   Memory blocks specific to modes are zero-initialized dynamically upon a mode change.
 */
void MimicDSP_UpdateParameters(const MimicDSP_Config_t *config);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_DSP_H
