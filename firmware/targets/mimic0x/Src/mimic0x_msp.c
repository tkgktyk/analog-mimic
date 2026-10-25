/**
 ******************************************************************************
 * @file    mimic0x_msp.c
 * @author  TAKAGI Katsuyuki
 * @brief   MCU Support Package (MSP) Implementation for Base-Model Hardware, 
 *          Handling Clocks, GPIO, Peripherals, RAM Vector Table, and 
 *          Deterministic Sampling Engine Control.
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
#include "py32f071_ll_i2c.h"
#include "mimic_registers.h"

/* ==================================================================== */
/* Hardware Revision Pin Definitions                                    */
/* ==================================================================== */

#if defined(TARGET_MIMIC00) || defined(TARGET_MIMIC01)
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

  #if defined(TARGET_MIMIC01)
    // LED
    #define READY_LED_PORT  GPIOF
    #define READY_LED_PIN   GPIO_PIN_6
    
    // I2C address jumpers (closed = GND, internal pull-ups)
    #define ADDR_JP_PORT      GPIOB
    #define ADDR_A0_PIN       GPIO_PIN_3    // PB3 -> address bit 0
    #define ADDR_A1_PIN       GPIO_PIN_4    // PB4 -> address bit 1

  #endif
#else
  #error "Hardware revision macro is not defined! Please define TARGET_MIMIC00 or TARGET_MIMIC01."
#endif

/* ==================================================================== */
/* System & Timing Constants                                            */
/* ==================================================================== */

#define I2C_IRQ_PRIORITY    1
#define ADC_IRQ_PRIORITY    0

#define VECTOR_TABLE_SIZE   48
#define VECTOR_INDEX(irqn)  (16u + (uint32_t)(irqn))

#define I2C_CLOCK_SPEED_HZ  100000
#define I2C_SLAVE_ADDRESS(offset) ((MIMIC_DEFAULT_I2C_ADDR + (offset)) << 1)  // 7-bit 0x40 - 0x43

// CRITICAL TIMING CONSTRAINT:
// To prevent starvation of the I2C peripheral and the main loop,
// the system requires a strict margin of approximately 60-70 CPU cycles per sample.
// Maximum allowable DSP execution cycles = (SYSTEM_CLOCK_HZ / TARGET_SAMPLING_HZ) - 65.
#define TIM2_SAMPLING_PERIOD ((MIMIC_SYSTEM_CLOCK_HZ / MIMIC_ADC_SAMPLING_HZ) - 1)
_Static_assert((MIMIC_SYSTEM_CLOCK_HZ % MIMIC_ADC_SAMPLING_HZ) == 0, 
               "Configuration Error: MIMIC_SYSTEM_CLOCK_HZ must be perfectly divisible by MIMIC_ADC_SAMPLING_HZ to prevent jitter.");
_Static_assert((TIM2_SAMPLING_PERIOD % 2) == 1, 
               "Configuration Error: (MIMIC_SYSTEM_CLOCK_HZ / MIMIC_ADC_SAMPLING_HZ) must be an EVEN number to align with the 2-phase AMBA AHB bus and prevent phase-shift jitter.");

/* ==================================================================== */
/* Private Peripheral Handles & Vector Table                            */
/* ==================================================================== */

static volatile __attribute__((aligned(256))) uint32_t ram_vector_table[VECTOR_TABLE_SIZE];

static ADC_HandleTypeDef hadc1;
static DAC_HandleTypeDef hdac1;
static TIM_HandleTypeDef htim1;
static TIM_HandleTypeDef htim2;
static TIM_HandleTypeDef htim15;
static OPA_HandleTypeDef hopa1;
static OPA_HandleTypeDef hopa2;
static OPA_HandleTypeDef hopa3;

#ifdef MIMIC_ENGINE_DMA
static DMA_HandleTypeDef hdma_out;
#endif

#ifdef MIMIC_ENGINE_CPU
// Extern ISR entry points defined in mimic_dsp.c (.ramfunc)
extern void ADC_COMP_IRQHandler_DAC(void);
extern void ADC_COMP_IRQHandler_GPIO(void);
extern void ADC_COMP_IRQHandler(void);

static void SetSampleHandler(void (*handler)(void))
{
    ram_vector_table[VECTOR_INDEX(ADC_COMP_IRQn)] = (uint32_t)handler;
    __DSB();
    __ISB();
}
#endif

/* ==================================================================== */
/* Private Initialization Prototypes                                    */
/* ==================================================================== */

static void SystemClock_Config(void);
static void RelocateVectorTableToRAM(void);
static void GPIO_Hardware_Init(void);
static void I2C1_Hardware_Init(void);
static void MX_I2C1_Init(void);
static void MX_ADC1_Init(void);
static void MX_DAC_Init(void);
static void MX_OPA_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM15_Init(void);
#ifdef MIMIC_ENGINE_DMA
static void MX_OutputDMA_Init(void);
#endif

/* ==================================================================== */
/* HAL MSP Callbacks (Invoked automatically by HAL_PPP_Init)            */
/* ==================================================================== */

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}

void HAL_OPA_MspInit(OPA_HandleTypeDef *hopa)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_OPA_CLK_ENABLE();

    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;

    switch (hopa->Init.Part) {
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

    if (hadc->Instance == ADC1) {
        __HAL_RCC_ADC_CONFIG(RCC_ADCCLKSOURCE_PCLK_DIV6);
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_ADC_CLK_ENABLE();

        GPIO_InitStruct.Pin  = ADC_IN9_PIN;
        GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(ADC_IN9_PORT, &GPIO_InitStruct);

        HAL_NVIC_SetPriority(ADC_COMP_IRQn, ADC_IRQ_PRIORITY, 0);
    }
}

void HAL_DAC_MspInit(DAC_HandleTypeDef *hdac)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (hdac->Instance == DAC1) {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_DAC1_CLK_ENABLE();

        GPIO_InitStruct.Pin  = DAC1_OUT_PIN;
        GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(DAC_PORT, &GPIO_InitStruct);
    }
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim_base)
{
    if (htim_base->Instance == TIM1) {
        __HAL_RCC_TIM1_CLK_ENABLE();
    } else if (htim_base->Instance == TIM2) {
        __HAL_RCC_TIM2_CLK_ENABLE();
    } else if (htim_base->Instance == TIM15) {
        __HAL_RCC_TIM15_CLK_ENABLE();
    }
}

/* ==================================================================== */
/* Public Hardware Lifecycle & Engine Control APIs                      */
/* ==================================================================== */

void Mimic_HW_Init(void)
{
    HAL_Init();
    SystemClock_Config();

    // 1. Initialize Peripherals
    GPIO_Hardware_Init();
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

    // 2. Relocate vector table to RAM for zero-wait-state ISR execution
    RelocateVectorTableToRAM();

    // 3. Power on ADC peripheral and start Timer 2 timebase
    (void)ADC1->DR;
    __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_EOC | ADC_FLAG_OVR);
#ifdef MIMIC_ENGINE_DMA
    HAL_ADC_Start(&hadc1);
#else
    HAL_ADC_Start_IT(&hadc1);
#endif
    // Keep the engine paused until MimicDevice_Init() configures and starts it
    Mimic_HW_StopEngine();

    HAL_TIM_Base_Start(&htim2);

    // 4. Halt SysTick interrupts to prevent jitter and use SysTick purely as a cycle counter
    HAL_SuspendTick();
}

void Mimic_HW_SetOutputRoute(MimicOutputRoute_t route)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin  = OPA2_INN_PIN;
    gpio.Pull = GPIO_NOPULL;

    switch (route) {
        case MIMIC_OUT_DIGITAL:
            /* GPIO drives the shared node; the AIF amplifier must be off first. */
            HAL_OPA_Stop(&hopa2);
            HAL_DAC_Stop(&hdac1, DAC_CHANNEL_1);
            gpio.Mode  = GPIO_MODE_OUTPUT_PP;
            gpio.Speed = GPIO_SPEED_FREQ_HIGH;
            HAL_GPIO_Init(OPA2_PORT, &gpio);
#ifdef MIMIC_ENGINE_CPU
            SetSampleHandler(&ADC_COMP_IRQHandler_GPIO);
#endif
            break;

        case MIMIC_OUT_ANALOG:
            /* Analog mode also disables the Schmitt input buffer on the pin. */
            gpio.Mode = GPIO_MODE_ANALOG;
            HAL_GPIO_Init(OPA2_PORT, &gpio);
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
            HAL_GPIO_Init(OPA2_PORT, &gpio);
#ifdef MIMIC_ENGINE_CPU
            SetSampleHandler(&ADC_COMP_IRQHandler);
#endif
            break;
    }
}

void Mimic_HW_StartEngine(MimicOutputRoute_t route, volatile uint32_t *slot_addr)
{
    static MimicOutputRoute_t last_route = MIMIC_OUT_OPEN;

    /* 1. Seed the slot and DAC register when the output format (Digital vs Analog) changes */
    bool format_changed = ((route == MIMIC_OUT_DIGITAL) != (last_route == MIMIC_OUT_DIGITAL))
                       || (last_route == MIMIC_OUT_OPEN);
    if (format_changed) {
        if (route == MIMIC_OUT_DIGITAL) {
            *slot_addr = ((uint32_t)OPA2_INN_PIN << 16); // Default GPIO Low via BSRR
        } else {
            *slot_addr = MIMIC_ADC_MID_VALUE;
#ifdef MIMIC_ENGINE_CPU
            DAC1->DHR12R1 = MIMIC_ADC_MID_VALUE;
#endif
        }
    }
    last_route = route;

#ifdef MIMIC_ENGINE_DMA
    uint32_t dst = (route == MIMIC_OUT_DIGITAL)
                 ? (uint32_t)&OPA2_PORT->BSRR
                 : (uint32_t)&DAC1->DHR12R1;

    /* 2. Re-arm the DMA channel on the selected destination. */
    if (HAL_DMA_Start_IT(&hdma_out, (uint32_t)slot_addr, dst, 1) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    /* 3. Discard any interrupt left pending from the previous route. */
    DMA1->IFCR = DMA_FLAG_GL1;
    HAL_NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

    /* 4. Enable DMA requests and clear any stale EOC/OVER flags. */
    SET_BIT(ADC1->CR2, ADC_CR2_DMA);
    (void)ADC1->DR;
    CLEAR_BIT(ADC1->SR, ADC_SR_OVER | ADC_SR_EOC);
    SET_BIT(ADC1->CR2, ADC_CR2_EXTTRIG);
#else
    (void)ADC1->DR;
    CLEAR_BIT(ADC1->SR, ADC_SR_OVER | ADC_SR_EOC);
    HAL_NVIC_ClearPendingIRQ(ADC_COMP_IRQn);
    HAL_NVIC_EnableIRQ(ADC_COMP_IRQn);
    SET_BIT(ADC1->CR2, ADC_CR2_EXTTRIG);
#endif
}

void Mimic_HW_StopEngine(void)
{
    // 1. Stop new ADC conversion triggers immediately so ADC_DR won't overrun
    CLEAR_BIT(ADC1->CR2, ADC_CR2_EXTTRIG);

#ifdef MIMIC_ENGINE_DMA
    // 2. Disable ADC DMA requests before aborting the DMA channel
    CLEAR_BIT(ADC1->CR2, ADC_CR2_DMA);
    (void)HAL_DMA_Abort(&hdma_out);
    HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
#else
    HAL_NVIC_DisableIRQ(ADC_COMP_IRQn);
#endif
}

void Mimic_SetReadyLED(bool is_on)
{
#if defined(TARGET_MIMIC01)
    HAL_GPIO_WritePin(READY_LED_PORT, READY_LED_PIN, is_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    (void)is_on;
#endif
}

void Mimic_HW_ErrorTrap(void)
{
    while (1) {
        Mimic_SetReadyLED(true);
        for (volatile uint32_t i = 0; i < 300000; i++);
        Mimic_SetReadyLED(false);
        for (volatile uint32_t i = 0; i < 300000; i++);
    }
}

/* ==================================================================== */
/* Private Hardware Initialization Implementations                      */
/* ==================================================================== */

static void RelocateVectorTableToRAM(void)
{
    uint32_t *flash_vectors = (uint32_t *)0x08000000;

    for (int i = 0; i < VECTOR_TABLE_SIZE; i++) {
        ram_vector_table[i] = flash_vectors[i];
    }

    __DSB();
    SCB->VTOR = (uint32_t)ram_vector_table;
    __ISB();
}

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI | RCC_OSCILLATORTYPE_LSE;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_24MHz;
    RCC_OscInitStruct.HSEState = RCC_HSE_OFF;
    RCC_OscInitStruct.LSIState = RCC_LSI_OFF;
    RCC_OscInitStruct.LSEState = RCC_LSE_OFF;

    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL3; // HSI(24MHz) * 3 = 72MHz

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    SystemCoreClockUpdate();
}

static void GPIO_Hardware_Init(void)
{
#if defined(TARGET_MIMIC01)
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    HAL_GPIO_WritePin(READY_LED_PORT, READY_LED_PIN, GPIO_PIN_RESET);

    /* 1. LED */
    GPIO_InitStruct.Pin   = READY_LED_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(READY_LED_PORT, &GPIO_InitStruct);

    /* 2. I2C address jumpers: inputs with internal pull-ups */
    GPIO_InitStruct.Pin   = ADDR_A0_PIN | ADDR_A1_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull  = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ADDR_JP_PORT, &GPIO_InitStruct);
#endif
}

/**
 * @brief  Reads the I2C address jumpers (closed = 1).
 * @return Offset added to MIMIC_DEFAULT_I2C_ADDR (0 - 3). 0 on boards without jumpers.
 */
static uint8_t GetAddressJumpers(void)
{
#if defined(TARGET_MIMIC01)
  HAL_Delay(1);   // pull-ups settle (SysTick is still running at this point)
  uint8_t offset = 0;
  if (HAL_GPIO_ReadPin(ADDR_JP_PORT, ADDR_A0_PIN) == GPIO_PIN_RESET) offset |= 0x01;
  if (HAL_GPIO_ReadPin(ADDR_JP_PORT, ADDR_A1_PIN) == GPIO_PIN_RESET) offset |= 0x02;
  return offset;
#else
  return 0;
#endif
}

static void I2C1_Hardware_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin       = I2C_SCL_PIN | I2C_SDA_PIN;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = I2C_SCL_AF;
    HAL_GPIO_Init(I2C_SCL_PORT, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(I2C1_IRQn, I2C_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(I2C1_IRQn);

    __HAL_RCC_I2C1_FORCE_RESET();
    __HAL_RCC_I2C1_RELEASE_RESET();
}

static void MX_I2C1_Init(void)
{
    I2C1_Hardware_Init();

    LL_I2C_InitTypeDef I2C_InitStruct = {0};
    I2C_InitStruct.PeripheralMode  = LL_I2C_MODE_I2C;
    I2C_InitStruct.ClockSpeed      = I2C_CLOCK_SPEED_HZ;
    I2C_InitStruct.DutyCycle       = LL_I2C_DUTYCYCLE_2;
    I2C_InitStruct.OwnAddress1     = I2C_SLAVE_ADDRESS(GetAddressJumpers());
    I2C_InitStruct.TypeAcknowledge = LL_I2C_ACK;
    I2C_InitStruct.OwnAddrSize     = LL_I2C_OWNADDRESS1_7BIT;
    LL_I2C_Init(I2C1, &I2C_InitStruct);

    LL_I2C_EnableClockStretching(I2C1);
    LL_I2C_Enable(I2C1);

    LL_I2C_EnableIT_EVT(I2C1);
    LL_I2C_EnableIT_BUF(I2C1);
    LL_I2C_EnableIT_ERR(I2C1);
}

static void MX_ADC1_Init(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    hadc1.Instance                   = ADC1;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.NbrOfDiscConversion   = 1;
    hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIGCONV_T2_CC2;

    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    if (HAL_ADC_ConfigVrefBuf(&hadc1, ADC_VREFBUF_VCCA) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    sConfig.Channel      = ADC_CHANNEL_9;
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES_5;

    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }
}

static void MX_DAC_Init(void)
{
    DAC_ChannelConfTypeDef sConfig = {0};

    hdac1.Instance = DAC1;
    if (HAL_DAC_Init(&hdac1) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

#ifdef MIMIC_ENGINE_DMA
    sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
#endif
#ifdef MIMIC_ENGINE_CPU
    sConfig.DAC_Trigger = DAC_TRIGGER_SOFTWARE;
#endif
    sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;

    if (HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_1) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }
}

static void MX_OPA_Init(void)
{
    hopa1.Instance  = OPA;
    hopa1.Init.Part = OPA1;
    if (HAL_OPA_Init(&hopa1) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }
    HAL_OPA_Start(&hopa1);

    hopa2.Instance  = OPA;
    hopa2.Init.Part = OPA2;
    if (HAL_OPA_Init(&hopa2) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }
    HAL_OPA_Start(&hopa2);

    hopa3.Instance  = OPA;
    hopa3.Init.Part = OPA3;
    if (HAL_OPA_Init(&hopa3) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }
    HAL_OPA_Start(&hopa3);
}

static void MX_TIM1_Init(void)
{
    TIM_ClockConfigTypeDef sClock  = {0};
    TIM_SlaveConfigTypeDef sSlave  = {0};
    TIM_OC_InitTypeDef sConfigOC   = {0};

    htim1.Instance               = TIM1;
    htim1.Init.Prescaler         = 0;
    htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim1.Init.Period            = TIM2_SAMPLING_PERIOD;
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim1) != HAL_OK) Mimic_HW_ErrorTrap();

    sClock.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim1, &sClock) != HAL_OK) Mimic_HW_ErrorTrap();

    sSlave.SlaveMode    = TIM_SLAVEMODE_RESET;
    sSlave.InputTrigger = TIM_TS_ITR1;
    if (HAL_TIM_SlaveConfigSynchro(&htim1, &sSlave) != HAL_OK) Mimic_HW_ErrorTrap();

    sConfigOC.OCMode       = TIM_OCMODE_PWM1;
    sConfigOC.Pulse        = 153;
    sConfigOC.OCPolarity   = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    sConfigOC.OCFastMode   = TIM_OCFAST_DISABLE;
    sConfigOC.OCIdleState  = TIM_OCIDLESTATE_RESET;
    sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    __HAL_TIM_MOE_ENABLE(&htim1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
    __HAL_TIM_ENABLE(&htim1);
}

static void MX_TIM2_Init(void)
{
    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig     = {0};
    TIM_OC_InitTypeDef sConfigOC              = {0};

    htim2.Instance               = TIM2;
    htim2.Init.Prescaler         = 0;
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = TIM2_SAMPLING_PERIOD;
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.RepetitionCounter = 0;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_Base_Init(&htim2) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
    sMasterConfig.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    sConfigOC.OCMode     = TIM_OCMODE_PWM1;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

    // CH2: ADC trigger source (EXTSEL=011), no pin output
    sConfigOC.Pulse = 100;
    if (HAL_TIM_OC_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_2) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }
    HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_2);

    // CH3: Reference edge for PA2
    sConfigOC.Pulse = 100;
    if (HAL_TIM_OC_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_3) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }
    HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_3);
}

static void MX_TIM15_Init(void)
{
    TIM_SlaveConfigTypeDef sSlave = {0};
    TIM_OC_InitTypeDef sConfigOC  = {0};

    htim15.Instance               = TIM15;
    htim15.Init.Prescaler         = 0;
    htim15.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim15.Init.Period            = 1;
    htim15.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim15.Init.RepetitionCounter = 0;
    htim15.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim15) != HAL_OK) Mimic_HW_ErrorTrap();

    sSlave.SlaveMode    = TIM_SLAVEMODE_EXTERNAL1;
    sSlave.InputTrigger = TIM_TS_ITR0;
    if (HAL_TIM_SlaveConfigSynchro(&htim15, &sSlave) != HAL_OK) Mimic_HW_ErrorTrap();

    sConfigOC.OCMode       = TIM_OCMODE_PWM1;
    sConfigOC.Pulse        = 1;
    sConfigOC.OCPolarity   = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    sConfigOC.OCFastMode   = TIM_OCFAST_DISABLE;
    sConfigOC.OCIdleState  = TIM_OCIDLESTATE_RESET;
    sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    if (HAL_TIM_PWM_ConfigChannel(&htim15, &sConfigOC, TIM_CHANNEL_1) != HAL_OK) {
        Mimic_HW_ErrorTrap();
    }

    HAL_TIM_PWM_Start(&htim15, TIM_CHANNEL_1);
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
    if (HAL_DMA_Init(&hdma_out) != HAL_OK) Mimic_HW_ErrorTrap();

    HAL_DMA_ChannelMap(&hdma_out, DMA_CHANNEL_MAP_ADC1);
    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, ADC_IRQ_PRIORITY, 0);
}
#endif
