/**
 ******************************************************************************
 * @file    main.c
 * @author  TAKAGI Katsuyuki
 * @brief   Main Application Entry Point, Core System Lifecycle Management, 
 * and Background Non-Deterministic Parameter Processing Loop.
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

#include "py32f0xx_hal.h"
#include "py32f071_ll_i2c.h"

#include "mimic_registers.h"
#include "mimic_dsp.h"
#include "mimic_i2c.h"
#include "mimic_device.h"

#define VECTOR_TABLE_SIZE 48
__attribute__((aligned(256))) uint32_t ram_vector_table[VECTOR_TABLE_SIZE];

// =========================================================
// Configuration Macros
// =========================================================

// Peripheral configuration constants
#define I2C_CLOCK_SPEED_HZ 100000
#define I2C_SLAVE_ADDRESS (MIMIC_DEFAULT_I2C_ADDR << 1) // 7-bit address "0x40"

// CRITICAL TIMING CONSTRAINT:
// To prevent starvation of the I2C peripheral and the main loop,
// the system requires a strict margin of approximately 60-70 CPU cycles per sample.
// Maximum allowable DSP execution cycles = (SYSTEM_CLOCK_HZ / TARGET_SAMPLING_HZ) - 65.
// Exceeding this budget will result in I2C timeouts and system lockups.
#define TIM3_SAMPLING_PERIOD   ((MIMIC_SYSTEM_CLOCK_HZ / MIMIC_ADC_SAMPLING_HZ) - 1)
_Static_assert((MIMIC_SYSTEM_CLOCK_HZ % MIMIC_ADC_SAMPLING_HZ) == 0, 
               "Configuration Error: MIMIC_SYSTEM_CLOCK_HZ must be perfectly divisible by MIMIC_ADC_SAMPLING_HZ to prevent jitter.");

// =========================================================
// Initialization Function Prototypes
// =========================================================
static void RelocateVectorTableToRAM(void);
static void APP_ErrorHandler(void);
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_ADC1_Init(void);
static void MX_DAC_Init(void);
static void MX_TIM3_Init(void); // For sampling frequency
static void MX_OPA_Init(void);

// =========================================================
// Global Variables
// =========================================================

// HAL Peripheral Handles
ADC_HandleTypeDef hadc1;
DAC_HandleTypeDef hdac1;
TIM_HandleTypeDef htim3;
OPA_HandleTypeDef hopa1;
OPA_HandleTypeDef hopa2;
OPA_HandleTypeDef hopa3;

// Implements specific hardware control requested by the core for "OPEN state"
void MimicCallback_DisableOutput(void) {
    HAL_OPA_Stop(&hopa2);
}

void MimicCallback_EnableOutput(void) {
    HAL_OPA_Start(&hopa2); 
}

// =========================================================
// Main Routine
// =========================================================
int main(void) {
  HAL_Init();
  SystemClock_Config();

  // Initialize Peripherals
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_ADC1_Init();
  MX_DAC_Init();
  MX_OPA_Init();
  MX_TIM3_Init();

  // Initialize Analog Mimic Core Logic
  MimicI2c_Init();
  MimicDSP_Init(&MimicCallback_EnableOutput, &MimicCallback_DisableOutput);
  MimicDevice_LoadCalibration();

  // Relocate vector table to RAM
  RelocateVectorTableToRAM();

  // Start Peripherals
  HAL_ADC_Start_IT(&hadc1);
  HAL_DAC_Start(&hdac1, DAC_CHANNEL_1);
  HAL_TIM_Base_Start(&htim3);
  
  // Halt SysTick
  HAL_SuspendTick();

  // Initialization completed
  MimicDevice_SetStatusFlag(MIMIC_STATUS_SYSTEM_READY);

  // Main background loop
  while (1) {
    MimicDSP_ProcessPendingTasks();

    // 2. Execute WFI (Wait For Interrupt).
    // When a sampling interrupt (TIM3) occurs here, it will jump to the ISR 
    // via the vector table (VTOR) relocated on SRAM.
    __asm volatile (
      "dsb \n" // Data Synchronization Barrier: Ensures data access completion
      "isb \n" // Instruction Synchronization Barrier: Flushes the pipeline
      "wfi \n" 
    );
  }
}

static void RelocateVectorTableToRAM(void) {
    // 1. Copy the vector table from the current Flash location (address 0x08000000) to RAM.
    // The Flash start address of PY32F071 is 0x08000000.
    uint32_t *flash_vectors = (uint32_t *)0x08000000;
    
    for (int i = 0; i < VECTOR_TABLE_SIZE; i++) {
        ram_vector_table[i] = flash_vectors[i];
    }

    // 2. Data Synchronization Barrier (ensures write completion)
    __DSB();

    // 3. Set the address of the table on RAM to the VTOR register
    // Rewrite the VTOR of the System Control Block (SCB) in Cortex-M0+
    SCB->VTOR = (uint32_t)ram_vector_table;

    // 4. Instruction Synchronization Barrier (reflects settings immediately)
    __ISB();
}

// =========================================================
// Peripheral Initialization Implementations
// =========================================================
static void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  // 1. Configure HSI to 24MHz, enable PLL and multiply by 3 to achieve 72MHz
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI | RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_24MHz; // Select 24MHz
  
  RCC_OscInitStruct.HSEState = RCC_HSE_OFF;                         /* Disable HSE */
  RCC_OscInitStruct.LSIState = RCC_LSI_OFF;                         /* Disable LSI */
  RCC_OscInitStruct.LSEState = RCC_LSE_OFF;                         /* Disable LSE */

  // PLL Configuration (Fine-tuning may be required depending on the specific PLL register layout of the device)
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL3; // HSI(24MHz) * 3 = 72MHz

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    APP_ErrorHandler();
  }

  // 2. Clock distribution for each bus and FLASH latency configuration
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK; // Set PLL as source
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1; // APB1 should be DIV1 = 72MHz

  // FLASH_LATENCY_2 is safe for 72MHz operation
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
    APP_ErrorHandler();
  }
  
  // Update the global variable with the updated clock frequency
  SystemCoreClockUpdate();
}

static void MX_I2C1_Init(void) {
  // 1. Enable I2C1 peripheral clock
  __HAL_RCC_I2C1_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();                      // Enable GPIOB clock

  // 2. Initialize GPIO (PB6=SCL, PB7=SDA)
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;     // Specify Pin 6 and Pin 7
  GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;            // Required for I2C: Open-Drain
  GPIO_InitStruct.Pull = GPIO_NOPULL;                // No pull-up/down (external resistors present)
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF1_I2C1;         // I2C1 AF number for PB6/PB7 is AF1
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);            // Execute initialization for GPIOB

  // 3. Enable NVIC (Interrupt)
  HAL_NVIC_SetPriority(I2C1_IRQn, 1, 0); 
  HAL_NVIC_EnableIRQ(I2C1_IRQn);

  // Force reset the I2C1 module before initialization
  __HAL_RCC_I2C1_FORCE_RESET();
  __HAL_RCC_I2C1_RELEASE_RESET();

  // I2C initialization configuration using LL
  LL_I2C_InitTypeDef I2C_InitStruct = {0};
  I2C_InitStruct.PeripheralMode  = LL_I2C_MODE_I2C;
  I2C_InitStruct.ClockSpeed      = I2C_CLOCK_SPEED_HZ;
  I2C_InitStruct.DutyCycle       = LL_I2C_DUTYCYCLE_2;
  I2C_InitStruct.OwnAddress1     = I2C_SLAVE_ADDRESS;
  I2C_InitStruct.TypeAcknowledge = LL_I2C_ACK;
  I2C_InitStruct.OwnAddrSize     = LL_I2C_OWNADDRESS1_7BIT;
  LL_I2C_Init(I2C1, &I2C_InitStruct);

  // Enable clock stretching
  LL_I2C_EnableClockStretching(I2C1);
  // Enable peripheral
  LL_I2C_Enable(I2C1);

  // Enable various interrupts required for slave operation
  LL_I2C_EnableIT_EVT(I2C1); // Event interrupts (ADDR, STOPF, etc.)
  LL_I2C_EnableIT_BUF(I2C1); // Buffer interrupts (TXE, RXNE)
  LL_I2C_EnableIT_ERR(I2C1); // Error interrupts (AF, BERR, etc.)
}

static void MX_ADC1_Init(void) {
  __HAL_RCC_ADC_CONFIG(RCC_ADCCLKSOURCE_PCLK_DIV4);
  
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.NbrOfDiscConversion = 1;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T3_TRGO;

  if (HAL_ADC_Init(&hadc1) != HAL_OK) {
    APP_ErrorHandler();
  }

  // Specify ADC_VREFBUF_VCCA to fully utilize the 5V power supply
  if (HAL_ADC_ConfigVrefBuf(&hadc1, ADC_VREFBUF_VCCA) != HAL_OK) {
    APP_ErrorHandler();
  }
  
  // ADC Channel (OPA3 Out -> PB1)
  sConfig.Channel = ADC_CHANNEL_9;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_13CYCLES_5;

  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
    APP_ErrorHandler();
  }

  /* ADC calibration */
  if(HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK)
  {
    APP_ErrorHandler();
  }
}

static void MX_DAC_Init(void) {
  DAC_ChannelConfTypeDef sConfig = {0};

  hdac1.Instance = DAC1;
  if (HAL_DAC_Init(&hdac1) != HAL_OK) {
    APP_ErrorHandler();
  }

  sConfig.DAC_Trigger = DAC_TRIGGER_SOFTWARE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;

  if (HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_1) != HAL_OK) {
    APP_ErrorHandler();
  }
}

static void MX_OPA_Init(void) {
  // OPA1 (Voltage Follower)
  hopa1.Instance = OPA;
  hopa1.Init.Part = OPA1;
  if (HAL_OPA_Init(&hopa1) != HAL_OK) {
    APP_ErrorHandler();
  }
  HAL_OPA_Start(&hopa1);

  // OPA2
  hopa2.Instance = OPA;
  hopa2.Init.Part = OPA2;
  if (HAL_OPA_Init(&hopa2) != HAL_OK) {
    APP_ErrorHandler();
  }
  HAL_OPA_Start(&hopa2);

  // OPA3
  hopa3.Instance = OPA;
  hopa3.Init.Part = OPA3;
  if (HAL_OPA_Init(&hopa3) != HAL_OK) {
    APP_ErrorHandler();
  }
  HAL_OPA_Start(&hopa3);
}

static void MX_TIM3_Init(void) {
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 0;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = TIM3_SAMPLING_PERIOD;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.RepetitionCounter = 0;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

  if (HAL_TIM_Base_Init(&htim3) != HAL_OK) {
    APP_ErrorHandler();
  }

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK) {
    APP_ErrorHandler();
  }

  // Trigger ADC on Update event
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK) {
    APP_ErrorHandler();
  }
}

static void MX_GPIO_Init(void) {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
}


/**
 * @brief  Error executing function.
 */
void APP_ErrorHandler(void) {
  while (1) {
  }
}

#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 * where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 */
void assert_failed(uint8_t *file, uint32_t line) {
  // printf("Wrong parameters value: file %s on line %d\r\n", file, line);
  while (1) {
  }
}
#endif /* USE_FULL_ASSERT */

/**
 * @brief Override standard library initialization hooks to prevent 
 * the inclusion of malloc, reent, and file system overloads.
 * This saves substantial FLASH and reclaims Heap RAM.
 */
void __libc_init_array(void) {
}
