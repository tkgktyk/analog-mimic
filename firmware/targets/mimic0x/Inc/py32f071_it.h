/**
  ******************************************************************************
  * @file    py32f071_it.h
  * @author  MCU Application Team
  * @brief   This file contains the headers of the interrupt handlers.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2023 Puya Semiconductor Co.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by Puya under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  * opensource.org/licenses/BSD-3-Clause
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
  * opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef PY32F071_IT_H
#define PY32F071_IT_H

#ifdef __cplusplus
 extern "C" {
#endif 

/* ==================================================================== */
/* Cortex-M0+ Processor Interruption and Exception Handlers             */
/* ==================================================================== */

/**
 * @brief Handles Non-maskable interrupt.
 */
void NMI_Handler(void);

/**
 * @brief Handles Hard fault interrupt.
 */
void HardFault_Handler(void);

/**
 * @brief Handles System service call via SWI instruction.
 */
void SVC_Handler(void);

/**
 * @brief Handles Pendable request for system service.
 */
void PendSV_Handler(void);

/**
 * @brief Handles System tick timer.
 */
void SysTick_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* PY32F071_IT_H */
