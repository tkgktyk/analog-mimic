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
#include "mimic_registers.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIMIC_DSP_PAYLOAD_SIZE (MIMIC_REG_MODE_PAYLOAD_END - MIMIC_REG_MODE_PAYLOAD_START)

typedef struct {
    uint8_t mode_id;
    uint8_t global_flags;
    uint8_t decimation_n;
    int32_t gain_q15;
    int32_t offset_raw;
    uint8_t payload[MIMIC_DSP_PAYLOAD_SIZE];
} MimicDSP_Config_t;


/**
 * @brief Initializes the DSP core memory space and internal state variables.
 * @note  Clears biquad delay lines and resets tracking waveform latches to default.
 */
void MimicDSP_Init(void);

void MimicDSP_SetDecimation(uint8_t N);

void MimicDSP_UpdateParameters(const MimicDSP_Config_t *config);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_DSP_CORE_H
