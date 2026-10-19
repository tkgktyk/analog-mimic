/**
 ******************************************************************************
 * @file    mimic0x_msp.h
 * @author  TAKAGI Katsuyuki
 * @brief   Header for MCU Support Package (Hardware-specific initializations
 *          and deterministic sampling engine control)
 ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2023 Puya Semiconductor Co.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by Puya under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */

#ifndef MIMIC0X_MSP_H
#define MIMIC0X_MSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(TARGET_MIMIC01) || defined(TARGET_MIMIC00)
// OPA2 Pins (Output Driver / Shared Digital Output Node on PA6)
#define OPA2_PORT               GPIOA
#define OPA2_INP_PIN            GPIO_PIN_7    // PA7
#define OPA2_INN_PIN            GPIO_PIN_6    // PA6 (Shared with Digital Output)
#define OPA2_OUT_PIN            GPIO_PIN_5    // PA5

// Test pad
#define MIMIC_TEST_PAD_PORT     GPIOA
#define MIMIC_TEST_PAD_PIN      GPIO_PIN_2
#endif

// =========================================================
// Hardware Control Types
// =========================================================

typedef enum {
    MIMIC_OUT_OPEN = 0,   /* High-Z: OPA2 stopped, PA6 set to Analog mode */
    MIMIC_OUT_ANALOG,     /* Analog path: DAC1 -> AIF (OPA2) */
    MIMIC_OUT_DIGITAL     /* Digital path: GPIO push-pull (PA6) */
} MimicOutputRoute_t;

// =========================================================
// System & Engine Hardware Lifecycle APIs
// =========================================================

/**
 * @brief Initializes HAL, System Clock (72MHz), RAM Vector Table, and all peripherals.
 *        Starts the sampling timer timebase with ADC external triggers disabled until
 *        Mimic_HW_StartEngine() is explicitly invoked.
 */
void Mimic_HW_Init(void);

/**
 * @brief Switches the physical output route (OPA2/DAC1 vs GPIO) and binds
 *        the corresponding deterministic ISR handler in the RAM vector table.
 * @note  Must be called while the engine is stopped.
 * @param route The target output route (OPEN, ANALOG, or DIGITAL).
 */
void Mimic_HW_SetOutputRoute(MimicOutputRoute_t route);

/**
 * @brief Clears stale ADC/DMA hardware flags, arms the DMA channel (if enabled),
 *        and re-enables ADC external triggers to start deterministic sampling.
 * @param route     The active output route.
 * @param slot_addr Pointer to the 32-bit DSP output slot in RAM (used as DMA source).
 */
void Mimic_HW_StartEngine(MimicOutputRoute_t route, volatile uint32_t *slot_addr);

/**
 * @brief Safely stops ADC conversion triggers, aborts active DMA transfers,
 *        and disables sampling interrupts.
 */
void Mimic_HW_StopEngine(void);

/**
 * @brief Controls the hardware System Ready LED.
 * @param is_on true to turn on the LED, false to turn it off.
 */
void Mimic_SetReadyLED(bool is_on);

/**
 * @brief Fatal hardware error trap (blinks Ready LED indefinitely).
 */
void Mimic_HW_ErrorTrap(void);

#ifdef __cplusplus
}
#endif

#endif /* MIMIC0X_MSP_H */
