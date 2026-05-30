/**
 ******************************************************************************
 * @file    mimic0x_hal_msp.c
 * @author  TAKAGI Katsuyuki
 * @brief   MCU Support Package (MSP) Implementation for Base-Model Hardware, 
 * Handling Low-Level Peripheral Clock, GPIO, and NVIC Initializations.
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

#include "mimic0x_msp.h"
#include "py32f0xx_hal.h"

/* ==================================================================== */
/* Hardware Revision Pin Definitions (mimic00)                          */
/* ==================================================================== */

#if defined(TARGET_MIMIC00)
  // I2C Peripheral Configuration Pins (PB6 / PB7)
  #define I2C_SCL_PORT      GPIOB
  #define I2C_SCL_PIN       GPIO_PIN_6
  #define I2C_SCL_AF        GPIO_AF1_I2C1 

  #define I2C_SDA_PORT      GPIOB
  #define I2C_SDA_PIN       GPIO_PIN_7
  #define I2C_SDA_AF        GPIO_AF1_I2C1

  // OPA1 Pins (Input Buffer)
  #define OPA1_INP_PIN      GPIO_PIN_9    // PA9
  #define OPA1_INN_PIN      GPIO_PIN_10   // PA10
  #define OPA1_OUT_PIN      GPIO_PIN_8    // PA8
  #define OPA1_PORT         GPIOA

  // OPA2 Pins (Output Driver / DAC2)
  #define OPA2_INP_PIN      GPIO_PIN_7    // PA7
  #define OPA2_INN_PIN      GPIO_PIN_6    // PA6
  #define OPA2_OUT_PIN      GPIO_PIN_5    // PA5
  #define OPA2_PORT         GPIOA

  // OPA3 Pins (Anti-Aliasing Filter)
  #define OPA3_INP_PIN      GPIO_PIN_13   // PB13
  #define OPA3_INN_PIN      GPIO_PIN_12   // PB12
  #define OPA3_OUT_PIN      GPIO_PIN_14   // PB14
  #define OPA3_PORT         GPIOB

  // ADC & DAC Pins
  #define ADC_IN9_PIN       GPIO_PIN_1    // PB1 (Reads AAF Out)
  #define ADC_IN9_PORT      GPIOB
  #define DAC1_OUT_PIN      GPIO_PIN_4    // PA4
  #define DAC_PORT          GPIOA
#elif defined(TARGET_MIMIC01)
  // I2C Peripheral Configuration Pins (PB6 / PB7)
  #define I2C_SCL_PORT      GPIOB
  #define I2C_SCL_PIN       GPIO_PIN_6
  #define I2C_SCL_AF        GPIO_AF1_I2C1 

  #define I2C_SDA_PORT      GPIOB
  #define I2C_SDA_PIN       GPIO_PIN_7
  #define I2C_SDA_AF        GPIO_AF1_I2C1

  // OPA1 Pins (Input Buffer)
  #define OPA1_INP_PIN      GPIO_PIN_9    // PA9
  #define OPA1_INN_PIN      GPIO_PIN_10   // PA10
  #define OPA1_OUT_PIN      GPIO_PIN_8    // PA8
  #define OPA1_PORT         GPIOA

  // OPA2 Pins (Output Driver / DAC2)
  #define OPA2_INP_PIN      GPIO_PIN_7    // PA7
  #define OPA2_INN_PIN      GPIO_PIN_6    // PA6
  #define OPA2_OUT_PIN      GPIO_PIN_5    // PA5
  #define OPA2_PORT         GPIOA

  // OPA3 Pins (Anti-Aliasing Filter)
  #define OPA3_INP_PIN      GPIO_PIN_13   // PB13
  #define OPA3_INN_PIN      GPIO_PIN_12   // PB12
  #define OPA3_OUT_PIN      GPIO_PIN_14   // PB14
  #define OPA3_PORT         GPIOB

  // ADC & DAC Pins
  #define ADC_IN9_PIN       GPIO_PIN_1    // PB1 (Reads AAF Out)
  #define ADC_IN9_PORT      GPIOB
  #define DAC1_OUT_PIN      GPIO_PIN_4    // PA4
  #define DAC_PORT          GPIOA
#else
  #error "Hardware revision macro is not defined! Please define HW_REV_MIMIC00 in your build configuration."
#endif

/* ==================================================================== */
/* Interrupt Priority Definitions                                       */
/* ==================================================================== */
#define I2C_IRQ_PRIORITY    1
#define ADC_IRQ_PRIORITY    0

/* ==================================================================== */
/* System Initialization                                                */
/* ==================================================================== */
void HAL_MspInit(void)
{
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();
}

/* ==================================================================== */
/* Analog Initialization (OPA, ADC, DAC)                                */
/* ==================================================================== */
void HAL_OPA_MspInit(OPA_HandleTypeDef *hopa)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* Enable GPIO Clocks */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* Enable OPA peripheral clock */
  __HAL_RCC_OPA_CLK_ENABLE();

  /* All Analog pins must be set to GPIO_MODE_ANALOG */
  GPIO_InitStruct.Mode  = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;

  switch(hopa->Init.Part) {
    case OPA1:
      GPIO_InitStruct.Pin = OPA1_INP_PIN | OPA1_INN_PIN | OPA1_OUT_PIN;
      HAL_GPIO_Init(OPA1_PORT, &GPIO_InitStruct);
      break;
    case OPA2:
      GPIO_InitStruct.Pin = OPA2_INP_PIN | OPA2_INN_PIN | OPA2_OUT_PIN;
      HAL_GPIO_Init(OPA2_PORT, &GPIO_InitStruct);
      break;
    case OPA3:
      GPIO_InitStruct.Pin = OPA3_INP_PIN | OPA3_INN_PIN | OPA3_OUT_PIN;
      HAL_GPIO_Init(OPA3_PORT, &GPIO_InitStruct);
      break;
    default:
      break;
  }
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if(hadc->Instance == ADC1)
  {
    __HAL_RCC_ADC_CONFIG(RCC_ADCCLKSOURCE_PCLK_DIV4);

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_ADC_CLK_ENABLE();

    GPIO_InitStruct.Pin  = ADC_IN9_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ADC_IN9_PORT, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(ADC_COMP_IRQn, ADC_IRQ_PRIORITY, 0); 
    HAL_NVIC_EnableIRQ(ADC_COMP_IRQn);
  }
}

void HAL_DAC_MspInit(DAC_HandleTypeDef *hdac)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if(hdac->Instance == DAC1)
  {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DAC1_CLK_ENABLE();

    // Initialize DAC1_OUT (PA4) 
    GPIO_InitStruct.Pin  = DAC1_OUT_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(DAC_PORT, &GPIO_InitStruct);
  }
}

/* ==================================================================== */
/* Timer Initialization (TIM3)                                          */
/* ==================================================================== */
void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim_base)
{
  if(htim_base->Instance == TIM3)
  {
    /* Enable TIM3 peripheral clock */
    __HAL_RCC_TIM3_CLK_ENABLE();
  }
}

/**
 * @brief  Initializes the low-level hardware resources for I2C1.
 * Note: Since I2C1 uses the LL driver, this function is NOT called 
 * automatically by HAL and must be invoked manually in main.c.
 */
void I2C1_Hardware_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* 1. Enable peripheral clocks */
  __HAL_RCC_I2C1_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* 2. Initialize GPIO (PB6=SCL, PB7=SDA) */
  GPIO_InitStruct.Pin = I2C_SCL_PIN | I2C_SDA_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = I2C_SCL_AF;
  HAL_GPIO_Init(I2C_SCL_PORT, &GPIO_InitStruct);

  /* 3. Configure NVIC for I2C1 */
  HAL_NVIC_SetPriority(I2C1_IRQn, 1, 0); 
  HAL_NVIC_EnableIRQ(I2C1_IRQn);

  /* 4. Force reset the I2C1 module to clear any hanging states */
  __HAL_RCC_I2C1_FORCE_RESET();
  __HAL_RCC_I2C1_RELEASE_RESET();
}
