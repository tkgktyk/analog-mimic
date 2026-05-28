/**
 ******************************************************************************
 * @file    mimic_dsp_core.h
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

#ifndef MIMIC_DSP_CORE_H
#define MIMIC_DSP_CORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================
// Initialization and Main Loop
// =========================================================

/**
 * @brief Initializes the DSP core memory space and internal state variables.
 * @note  Clears biquad delay lines and resets tracking waveform latches to default.
 */
void MimicDSP_Init(void);

/**
 * @brief Applies parameters acquired via I2C to the internal DSP mathematical models.
 * @note  Typically called from within MimicDSP_ProcessLoop() while hardware
 * analog outputs are safely isolated.
 */
void MimicDSP_UpdateParameters(void);

/**
 * @brief  Evaluates the current global configuration snapshot to determine the output state.
 * @return true if the output path should be opened (muted), false otherwise.
 */
bool MimicDSP_GetOutputOpenStateFromSnapshot(void);

void MimicDSP_SetDecimation(uint8_t N);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_DSP_CORE_H
