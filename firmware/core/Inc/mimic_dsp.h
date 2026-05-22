/**
 ******************************************************************************
 * @file    mimic_dsp_core.h
 * @author  Analog Mimic Development Team
 * @brief   DSP Core Engine Interface and Mathematical Model Pipeline.
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

#ifdef __cplusplus
}
#endif

#endif // MIMIC_DSP_CORE_H
