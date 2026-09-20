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
\* but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 ******************************************************************************
 */

#include "py32f0xx_hal.h"
#include "py32f071_ll_i2c.h"

#include "mimic0x_msp.h"
#include "mimic_registers.h"
#include "mimic_device.h"
#include "mimic_dsp.h"

// =========================================================
// Configuration Macros
// =========================================================

#define VECTOR_TABLE_SIZE 48
#define VECTOR_INDEX(irqn)  (16u + (uint32_t)(irqn))

volatile __attribute__((aligned(256))) uint32_t ram_vector_table[VECTOR_TABLE_SIZE];

// Peripheral configuration constants
#define I2C_CLOCK_SPEED_HZ 100000
#define I2C_SLAVE_ADDRESS (MIMIC_DEFAULT_I2C_ADDR << 1) // 7-bit address "0x40"

// CRITICAL TIMING CONSTRAINT:
// To prevent starvation of the I2C peripheral and the main loop,
// the system requires a strict margin of approximately 60-70 CPU cycles per sample.
// Maximum allowable DSP execution cycles = (SYSTEM_CLOCK_HZ / TARGET_SAMPLING_HZ) - 65.
// Exceeding this budget will result in I2C timeouts and system lockups.
#define TIM2_SAMPLING_PERIOD ((MIMIC_SYSTEM_CLOCK_HZ / MIMIC_ADC_SAMPLING_HZ) - 1)
_Static_assert((MIMIC_SYSTEM_CLOCK_HZ % MIMIC_ADC_SAMPLING_HZ) == 0, 
               "Configuration Error: MIMIC_SYSTEM_CLOCK_HZ must be perfectly divisible by MIMIC_ADC_SAMPLING_HZ to prevent jitter.");
_Static_assert((TIM2_SAMPLING_PERIOD % 2) == 1, 
               "Configuration Error: (MIMIC_SYSTEM_CLOCK_HZ / MIMIC_ADC_SAMPLING_HZ) must be an EVEN number to align with the 2-phase AMBA AHB bus and prevent phase-shift jitter.");

#ifdef MIMIC_ENGINE_DMA
DMA_HandleTypeDef hdma_out;
#endif

// =========================================================
// Global Variables
// =========================================================

// HAL Peripheral Handles
ADC_HandleTypeDef hadc1;
DAC_HandleTypeDef hdac1;
TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim15;
OPA_HandleTypeDef hopa1;
OPA_HandleTypeDef hopa2;
OPA_HandleTypeDef hopa3;

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
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM15_Init(void);
static void MX_OPA_Init(void);
#ifdef MIMIC_ENGINE_DMA
static void MX_OutputDMA_Init(void);
#endif

#ifdef MIMIC_ENGINE_CPU
void ADC_COMP_IRQHandler_DAC(void);
void ADC_COMP_IRQHandler_GPIO(void);

/**
 * @brief  Bind the sampling interrupt to the handler of the active output route.
 * @note   Must be called with the sampling interrupt disabled, i.e. inside the
 *         host-opened reconfiguration window. The vector table has already been
 *         relocated to RAM at start-up; only the single entry is rewritten here.
 */
static void SetSampleHandler(void (*handler)(void))
{
    ram_vector_table[VECTOR_INDEX(ADC_COMP_IRQn)] = (uint32_t)handler;
    __DSB();
    __ISB();
}

#endif /* MIMIC_ENGINE_CPU */

// =========================================================
// Hardware Callback Implementations
// =========================================================
static MimicOutputRoute_t current_route = MIMIC_OUT_OPEN;

void MimicCallback_SetOutputRoute(MimicOutputRoute_t route)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin  = MIMIC_OUTPUT_PIN;
    gpio.Pull = GPIO_NOPULL;

    current_route = route;

    switch (route) {
    case MIMIC_OUT_DIGITAL:
        /* GPIO drives the shared node; the AIF amplifier must be off first. */
        HAL_OPA_Stop(&hopa2);
        HAL_DAC_Stop(&hdac1, DAC_CHANNEL_1);
        gpio.Mode  = GPIO_MODE_OUTPUT_PP;
        gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(MIMIC_OUTPUT_PORT, &gpio);
#ifdef MIMIC_ENGINE_CPU
        *(volatile uint32_t *)MimicDSP_GetOutputSlot() = ((uint32_t)MIMIC_OUTPUT_PIN << 16);
        SetSampleHandler(&ADC_COMP_IRQHandler_GPIO);
#endif
        break;

    case MIMIC_OUT_ANALOG:
        /* Analog mode also disables the Schmitt input buffer on the pin. */
        gpio.Mode = GPIO_MODE_ANALOG;
        HAL_GPIO_Init(MIMIC_OUTPUT_PORT, &gpio);
        HAL_OPA_Start(&hopa2);
        HAL_DAC_Start(&hdac1, DAC_CHANNEL_1);
#ifdef MIMIC_ENGINE_CPU
        SetSampleHandler(&ADC_COMP_IRQHandler_DAC);
#endif
        break;

    case MIMIC_OUT_OPEN:
    default:
        HAL_OPA_Stop(&hopa2);
        HAL_DAC_Stop(&hdac1, DAC_CHANNEL_1);
        gpio.Mode = GPIO_MODE_ANALOG;
        HAL_GPIO_Init(MIMIC_OUTPUT_PORT, &gpio);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Sample engine start / stop                                         */
/* ------------------------------------------------------------------ */
void MimicCallback_EngineStop(void)
{
#ifdef MIMIC_ENGINE_DMA
    /* Stop new requests before touching the channel: the CPAR/CMAR/CNDTR
       registers must not be written while EN is set. */
    CLEAR_BIT(ADC1->CR2, ADC_CR2_DMA);
    (void)HAL_DMA_Abort(&hdma_out);          /* may report NO_XFER; ignore */
    HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
    /* Conversions must not continue while nothing reads ADC_DR:
       the second unread result sets OVER (RM 16.7). */
    CLEAR_BIT(ADC1->CR2, ADC_CR2_EXTTRIG);
#else
    HAL_NVIC_DisableIRQ(ADC_COMP_IRQn);
    /* Conversions must not continue while nothing reads ADC_DR:
       the second unread result sets OVER (RM 16.7). */
    CLEAR_BIT(ADC1->CR2, ADC_CR2_EXTTRIG);
#endif
}

void MimicCallback_EngineStart(void)
{
#ifdef MIMIC_ENGINE_CPU
    (void)ADC1->DR;
    CLEAR_BIT(ADC1->SR, ADC_SR_OVER | ADC_SR_EOC);
    HAL_NVIC_ClearPendingIRQ(ADC_COMP_IRQn);
    HAL_NVIC_EnableIRQ(ADC_COMP_IRQn);
    SET_BIT(ADC1->CR2, ADC_CR2_EXTTRIG);   /* ★ 追加 */
#else
    static MimicOutputRoute_t last_route = MIMIC_OUT_OPEN;
    volatile uint32_t *slot = (volatile uint32_t *)MimicDSP_GetOutputSlot();
    bool format_changed =
        ((current_route == MIMIC_OUT_DIGITAL) != (last_route == MIMIC_OUT_DIGITAL));

    if (format_changed) {
        *slot = (current_route == MIMIC_OUT_DIGITAL)
              ? ((uint32_t)MIMIC_OUTPUT_PIN << 16)
              : MIMIC_ADC_MID_VALUE;
    }
    last_route = current_route;

    uint32_t dst;
    /* 1. Seed the slot with a value valid for the destination register,
          before the channel can transfer it. */
    if (current_route == MIMIC_OUT_DIGITAL) {
        dst   = (uint32_t)&MIMIC_OUTPUT_PORT->BSRR;
    } else {
        dst   = (uint32_t)&DAC1->DHR12R1;
    }

    /* 2. Re-arm the channel on the selected destination. */
    if (HAL_DMA_Start_IT(&hdma_out, (uint32_t)slot, dst, 1) != HAL_OK) {
        APP_ErrorHandler();
    }

    /* 3. Discard any interrupt left pending from the previous route. */
    DMA1->IFCR = DMA_FLAG_GL1;
    HAL_NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

    (void)ADC1->DR;
    CLEAR_BIT(ADC1->SR, ADC_SR_OVER | ADC_SR_EOC);
    SET_BIT(ADC1->CR2, ADC_CR2_DMA);
    SET_BIT(ADC1->CR2, ADC_CR2_EXTTRIG);
#endif
}

// =========================================================
// Main Routine
// =========================================================
int main(void) {
  HAL_Init();
  SystemClock_Config();

  // 1. Initialize Peripherals
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_ADC1_Init();
  MX_DAC_Init();
  MX_OPA_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM15_Init();
#ifdef MIMIC_ENGINE_DMA
  MX_OutputDMA_Init();
#endif

  // 2. Initialize Analog Mimic Core Logic
  MimicDevice_Init(&MimicCallback_SetOutputRoute, &MimicCallback_EngineStart, &MimicCallback_EngineStop);

  // 3. Relocate vector table to RAM for faster/safe ISR execution
  RelocateVectorTableToRAM();

  // 4. Start Peripherals
#ifdef MIMIC_ENGINE_DMA
  /* Calibration can leave EOC asserted. The ADC DMA request is edge-triggered
    on the EOC rising edge, so a stale EOC would prevent the first request
    and stall the output chain permanently. */
  (void)ADC1->DR;
  __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_EOC);
  HAL_ADC_Start(&hadc1);          /* EOCIE を立てない */
#endif
#ifdef MIMIC_ENGINE_CPU
  HAL_ADC_Start_IT(&hadc1);
#endif
  HAL_DAC_Start(&hdac1, DAC_CHANNEL_1);
  HAL_TIM_Base_Start(&htim2);

  // 5. Halt SysTick interrupts
  HAL_SuspendTick();

  // Initialization completed
  MimicDevice_SetStatusFlag(MIMIC_STATUS_SYSTEM_READY);
  Mimic_SetReadyLED(true);

  // Main background loop
  //MimicCallback_EngineStop();
  //MimicCallback_SetOutputRoute(MIMIC_OUT_DIGITAL);
  //MimicCallback_EngineStart();
  while (1) {
    MimicDevice_ProcessPendingTasks();

    // Execute WFI (Wait For Interrupt).
    // When a sampling interrupt (TIM3) occurs, execution jumps to the ISR 
    // via the VTOR relocated on SRAM.
    __asm volatile (
//      "dsb \n" // Data Synchronization Barrier: Ensures data access completion
//      "isb \n" // Instruction Synchronization Barrier: Flushes the pipeline
      "wfi \n" 
    );
  }
}

static void RelocateVectorTableToRAM(void) {
    // 1. Copy the current vector table from Flash (address 0x08000000) to RAM
    // Flash start address of PY32F071 is 0x08000000
    uint32_t *flash_vectors = (uint32_t *)0x08000000;
    
    for (int i = 0; i < VECTOR_TABLE_SIZE; i++) {
        ram_vector_table[i] = flash_vectors[i];
    }

    // 2. Data memory barrier instruction (ensures write completion)
    __DSB();

    // 3. Set the VTOR register to the address of the table in RAM
    // Rewrite VTOR in the System Control Block (SCB) of Cortex-M0+
    SCB->VTOR = (uint32_t)ram_vector_table;

    // 4. Instruction synchronization barrier instruction (reflects settings immediately)
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
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_24MHz; 
  
  RCC_OscInitStruct.HSEState = RCC_HSE_OFF;
  RCC_OscInitStruct.LSIState = RCC_LSI_OFF;
  RCC_OscInitStruct.LSEState = RCC_LSE_OFF;

  // PLL Configuration
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL3; // HSI(24MHz) * 3 = 72MHz

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    APP_ErrorHandler();
  }

  // 2. Clock distribution for each bus and FLASH latency configuration
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  // FLASH_LATENCY_2 is required for 72MHz operation
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
    APP_ErrorHandler();
  }
  
  // Update the global variable with the newly configured core clock frequency
  SystemCoreClockUpdate();
}

static void MX_I2C1_Init(void) {
  // 1. Initialize hardware resources (Pins, Clocks, NVIC)
  Mimic_I2C1_Hardware_Init();

  // 2. I2C LL initialization (Logical parameters)
  LL_I2C_InitTypeDef I2C_InitStruct = {0};
  I2C_InitStruct.PeripheralMode  = LL_I2C_MODE_I2C;
  I2C_InitStruct.ClockSpeed      = I2C_CLOCK_SPEED_HZ;
  I2C_InitStruct.DutyCycle       = LL_I2C_DUTYCYCLE_2;
  I2C_InitStruct.OwnAddress1     = I2C_SLAVE_ADDRESS;
  I2C_InitStruct.TypeAcknowledge = LL_I2C_ACK;
  I2C_InitStruct.OwnAddrSize     = LL_I2C_OWNADDRESS1_7BIT;
  LL_I2C_Init(I2C1, &I2C_InitStruct);

  // Enable clock stretching and the peripheral itself
  LL_I2C_EnableClockStretching(I2C1);
  LL_I2C_Enable(I2C1);

  // Enable interrupts for slave operation
  LL_I2C_EnableIT_EVT(I2C1);
  LL_I2C_EnableIT_BUF(I2C1);
  LL_I2C_EnableIT_ERR(I2C1);
}

static void MX_ADC1_Init(void) {  
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.NbrOfDiscConversion = 1;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_CC2;

  if (HAL_ADC_Init(&hadc1) != HAL_OK) {
    APP_ErrorHandler();
  }

  // Utilize the 5V power supply via VCCA
  if (HAL_ADC_ConfigVrefBuf(&hadc1, ADC_VREFBUF_VCCA) != HAL_OK) {
    APP_ErrorHandler();
  }
  
  // ADC Channel configuration (OPA3 Out -> PB1)
  sConfig.Channel = ADC_CHANNEL_9;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES_5;

  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
    APP_ErrorHandler();
  }

  // ADC calibration
  if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK) {
    APP_ErrorHandler();
  }
}

static void MX_DAC_Init(void) {
  DAC_ChannelConfTypeDef sConfig = {0};

  hdac1.Instance = DAC1;
  if (HAL_DAC_Init(&hdac1) != HAL_OK) {
    APP_ErrorHandler();
  }

#ifdef MIMIC_ENGINE_DMA
  sConfig.DAC_Trigger = DAC_TRIGGER_NONE;      /* TEN=0 */
#endif
#ifdef MIMIC_ENGINE_CPU
  sConfig.DAC_Trigger = DAC_TRIGGER_SOFTWARE;
#endif
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

static void MX_TIM1_Init(void) {
  TIM_ClockConfigTypeDef sClock = {0};
  TIM_SlaveConfigTypeDef sSlave = {0};
  TIM_OC_InitTypeDef sConfigOC  = {0};

  htim1.Instance               = TIM1;
  htim1.Init.Prescaler         = 0;
  htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim1.Init.Period            = TIM2_SAMPLING_PERIOD;   /* 359, same as TIM2 */
  htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK) APP_ErrorHandler();

  /* Count the internal clock, not the trigger */
  sClock.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClock) != HAL_OK) APP_ErrorHandler();

  /* Reset the counter on every TIM2 update (RM Table 21-2: TIM1 ITR1 = TIM2_TRGO) */
  sSlave.SlaveMode    = TIM_SLAVEMODE_RESET;
  sSlave.InputTrigger = TIM_TS_ITR1;
  if (HAL_TIM_SlaveConfigSynchro(&htim1, &sSlave) != HAL_OK) APP_ErrorHandler();

  /* Falling edge at CCR2 counts after the reset: 144 / 72 MHz = 2.0 us */
  sConfigOC.OCMode       = TIM_OCMODE_PWM1;
  sConfigOC.Pulse        = 153;//274;//144;
  sConfigOC.OCPolarity   = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode   = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState  = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK) {
    APP_ErrorHandler();
  }

  __HAL_TIM_MOE_ENABLE(&htim1);      /* TIM1 needs MOE, unlike TIM15 */
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
  __HAL_TIM_ENABLE(&htim1);          /* in case the slave-mode guard skipped CEN */
}

static void MX_TIM2_Init(void) {
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = TIM2_SAMPLING_PERIOD;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.RepetitionCounter = 0;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

  if (HAL_TIM_Base_Init(&htim2) != HAL_OK) {
    APP_ErrorHandler();
  }

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK) {
    APP_ErrorHandler();
  }

  // Trigger ADC on Update event
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) {
    APP_ErrorHandler();
  }

  sConfigOC.OCMode     = TIM_OCMODE_PWM1;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

  // CH2: ADC トリガ源(EXTSEL=011)。ピン出力なし
  sConfigOC.Pulse = 100;
  if (HAL_TIM_OC_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_2) != HAL_OK) {
    APP_ErrorHandler();
  }
  HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_2);

    // CH3: PA2 への基準エッジ
  sConfigOC.Pulse = 100;
  if (HAL_TIM_OC_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_3) != HAL_OK) {
    APP_ErrorHandler();
  }
  HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_3);
}

static void MX_TIM15_Init(void) {
  TIM_SlaveConfigTypeDef sSlave = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  htim15.Instance               = TIM15;
  htim15.Init.Prescaler         = 0;
  htim15.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim15.Init.Period            = 1;          /* divide TIM2 TRGO by two */
  htim15.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim15.Init.RepetitionCounter = 0;
  htim15.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim15) != HAL_OK) APP_ErrorHandler();

  /* Count TIM2 update events (RM Table 25-41: TIM15 ITR0 = TIM2_TRGO) */
  sSlave.SlaveMode    = TIM_SLAVEMODE_EXTERNAL1;
  sSlave.InputTrigger = TIM_TS_ITR0;
  if (HAL_TIM_SlaveConfigSynchro(&htim15, &sSlave) != HAL_OK) APP_ErrorHandler();

  sConfigOC.OCMode       = TIM_OCMODE_PWM1;
  sConfigOC.Pulse        = 1;                 /* 50% at the divided rate */
  sConfigOC.OCPolarity   = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode   = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState  = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim15, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) {
    APP_ErrorHandler();
  }

  HAL_TIM_PWM_Start(&htim15, TIM_CHANNEL_1);
}

static void MX_GPIO_Init(void) {
  Mimic_GPIO_Hardware_Init();
}

#ifdef MIMIC_ENGINE_DMA
static void MX_OutputDMA_Init(void)
{
  __HAL_RCC_DMA_CLK_ENABLE();

  hdma_out.Instance                 = DMA1_Channel1;
  hdma_out.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_out.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_out.Init.MemInc              = DMA_MINC_DISABLE;
  hdma_out.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  hdma_out.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
  hdma_out.Init.Mode                = DMA_CIRCULAR;
  hdma_out.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
  if (HAL_DMA_Init(&hdma_out) != HAL_OK) APP_ErrorHandler();

  /* ADC1 の EOC をこのチャネルに割り当てる */
  HAL_DMA_ChannelMap(&hdma_out, DMA_CHANNEL_MAP_ADC1);

  HAL_DMA_Start_IT(&hdma_out,
                   (uint32_t)MimicDSP_GetOutputSlot(),
                   (uint32_t)&DAC1->DHR12R1,
                   1);

  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

  SET_BIT(ADC1->CR2, ADC_CR2_DMA);                /* ADC の DMA 要求を許可 */
}
#endif

/**
 * @brief  System error handler. Traps execution.
 */
void APP_ErrorHandler(void) {
  while (1) {
    Mimic_SetReadyLED(true);
    for (volatile uint32_t i = 0; i < 300000; i++);
    Mimic_SetReadyLED(false);
    for (volatile uint32_t i = 0; i < 300000; i++);
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
