#include "mimic_i2c.h"
#include "py32f0xx_hal.h"
#include "py32f071_ll_i2c.h"
#include "mimic_device.h"

// =========================================================
// Constants & Macros
// =========================================================
#define I2C_READ_ERROR_VALUE 0xFF

// I2C Reception state (1-byte at a time)
static uint8_t current_reg_addr = 0;
static uint8_t is_reg_addr_received = 0;
static volatile uint8_t is_dsp_update_required = 0;

// =========================================================
// Initialization
// =========================================================

/**
 * @brief Initializes the I2C request handler variables.
 * @note  Default state defaults to 0 (unity gain / through mode) via system reset.
 */
void MimicI2c_Init(void) {
  current_reg_addr = 0;
  is_reg_addr_received = 0;
}

// =========================================================
// I2C Low-Layer Interrupt Handler (Replaces HAL Callbacks)
// =========================================================

/**
 * @brief I2C Hardware Interrupt Entry Point
 */
void I2C1_IRQHandler(void) {
  
  // 1. ADDR: Address matched (Communication started)
  if (LL_I2C_IsActiveFlag_ADDR(I2C1)) {
    // [CRITICAL] Before clearing the ADDR flag, read SR2 to determine direction.
    // Reading SR2 serves as the latter half of the hardware clear sequence.
    // To ensure atomicity and avoid race conditions, read SR1 then SR2 sequentially.
    __IO uint32_t sr1_val = I2C1->SR1; // Start clear sequence
    __IO uint32_t sr2_val = I2C1->SR2; // Complete clear sequence and latch direction

    // Direction analysis (SR2_TRA: Transmitter/receiver direction flag)
    // 0: Read (Slave receives), 1: Write (Slave transmits)
    if ((sr2_val & I2C_SR2_TRA) == 0) {
      // Master requests a WRITE operation (Slave receives data)
      // The incoming initial byte must be interpreted as the target register address.
      is_reg_addr_received = 0; 
    } else {
      // Master requests a READ operation (Slave transmits data)
      // Prepare transmission buffer based on the pre-latched current_reg_addr.
      // This will be processed shortly by the subsequent TXE flag block.
    }
    
    // Suppress compiler unused-variable warning safely
    (void)sr1_val; 
  }

  // 2. RXNE: Receive buffer not empty (Data package received from Master)
  if (LL_I2C_IsActiveFlag_RXNE(I2C1)) {
    uint8_t rx_data = LL_I2C_ReceiveData8(I2C1);
    
    if (is_reg_addr_received == 0) {
      // Byte 1 represents the operation target register address pointer
      current_reg_addr = rx_data;
      is_reg_addr_received = 1;
    } else {
      // Byte 2 and subsequent bytes contain payload data; push to shadow register
      if (current_reg_addr < MIMIC_REG_SHADOW_SIZE) {
        if (MimicI2c_WriteReg(current_reg_addr, rx_data)) {
          // Raise update trigger indicating a DSP parameter reconfiguration
          is_dsp_update_required = 1; 
        }
        current_reg_addr++;
      } else {
        MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
      }
    }
  }

  // 3. TXE: Transmit buffer empty (Master requests data read out)
  // Process transmission setup only if the BTF (Byte Transfer Finished) flag is clear
  if (LL_I2C_IsActiveFlag_TXE(I2C1) && (LL_I2C_IsActiveFlag_BTF(I2C1) == 0)) {
    uint8_t tx_data = MimicI2c_ReadReg(current_reg_addr);
    LL_I2C_TransmitData8(I2C1, tx_data);
    
    // Automatic address pointer auto-increment
    if (current_reg_addr <= MIMIC_REG_TELEMETRY_END) {
      current_reg_addr++;
    }
  }

  // 4. STOPF: Stop condition detected (Transaction finished)
  if (LL_I2C_IsActiveFlag_STOP(I2C1)) {
    LL_I2C_ClearFlag_STOP(I2C1);
    
    // Notify the main thread if any runtime parameters were altered during transaction
    if (is_dsp_update_required) {
      mimic_device.i2c_dirty_flag = 1;
      is_dsp_update_required = 0;
    }
  }

  // 5. AF: Acknowledge Failure (Master signals NACK to finalize Slave Read operation)
  if (LL_I2C_IsActiveFlag_AF(I2C1)) {
    LL_I2C_ClearFlag_AF(I2C1);
  }

  // 6. Comprehensive Error Handlers (Bus Error, Arbitration Lost, Overrun)
  if (LL_I2C_IsActiveFlag_BERR(I2C1)) {
    LL_I2C_ClearFlag_BERR(I2C1);
    MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
  }
  if (LL_I2C_IsActiveFlag_ARLO(I2C1)) {
    LL_I2C_ClearFlag_ARLO(I2C1);
    MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
  }
  if (LL_I2C_IsActiveFlag_OVR(I2C1)) {
    LL_I2C_ClearFlag_OVR(I2C1);
    MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
  }
}

// =========================================================
// I2C Register Access Interfaces
// =========================================================

/**
 * @brief Writes a value to a specified I2C shadow register.
 * @param reg The register address to write to.
 * @param val The value to write.
 */
bool MimicI2c_WriteReg(uint8_t reg, uint8_t val) {
  // Prevent out-of-bounds register access (memory corruption prevention)
  if (reg >= MIMIC_REG_SHADOW_SIZE) {
    MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
    return false;
  }

  // Map directly into the global shared memory map
  mimic_device.registers[reg] = val;
  return true;
}

/**
 * @brief Reads a value from a specified I2C register or internal virtual register.
 * @param reg The register address to read from.
 * @return The value of the requested register, or I2C_READ_ERROR_VALUE on error.
 */
uint8_t MimicI2c_ReadReg(uint8_t reg) {
  static uint16_t latched_adc_val = 0;
  static uint16_t latched_cpu_cycles = 0;

  // Intercept and resolve specialized virtual registers dynamically
  switch (reg) {
  case MIMIC_REG_STATUS:
    return MimicDevice_GetAndClearStatus();

  // --- 16-bit Bound Live ADC Sample Read Out ---
  case MIMIC_REG_ADC_VAL_H:
    latched_adc_val = MimicDevice_GetAdcVal(); // Latch atomic context upon high-byte access
    return (uint8_t)(latched_adc_val >> 8);
  case MIMIC_REG_ADC_VAL_L:
    return (uint8_t)(latched_adc_val & 0xFF);  // Return latched low-byte context safely

  // --- 16-bit Execution Profiler Cycle Count Read Out ---
  case MIMIC_REG_CPU_CYCLES_H:
    latched_cpu_cycles = MimicDevice_GetAndClearCpuCyclesMax(); // Fetch peak hold and clear atomicity
    return (uint8_t)(latched_cpu_cycles >> 8);
  case MIMIC_REG_CPU_CYCLES_L:
    return (uint8_t)(latched_cpu_cycles & 0xFF);

  default:
    break;
  }

  // Handle remaining spaces via standard memory map
  if (reg < MIMIC_REG_SHADOW_SIZE) {
    return mimic_device.registers[reg];
  }

  MimicDevice_SetStatusFlag(MIMIC_STATUS_I2C_COM_ERR);
  return I2C_READ_ERROR_VALUE;
}

// =========================================================
// Parameter Decoding Utilities
// =========================================================

/**
 * @brief Decodes 4 bytes transmitted from the I2C master in Big-Endian (MSB First) format.
 * @param base_addr The starting address in the register array.
 * @return The decoded 32-bit unsigned integer.
 */
uint32_t MimicI2c_GetParam32(uint8_t base_addr) {
  return ((uint32_t)mimic_device.registers[base_addr] << 24) | 
         ((uint32_t)mimic_device.registers[base_addr + 1] << 16) |
         ((uint32_t)mimic_device.registers[base_addr + 2] << 8) |
         ((uint32_t)mimic_device.registers[base_addr + 3]); 
}

/**
 * @brief Decodes 2 bytes transmitted from the I2C master in Big-Endian (MSB First) format.
 * @param base_addr The starting address in the register array.
 * @return The decoded 16-bit unsigned integer.
 */
uint16_t MimicI2c_GetParam16(uint8_t base_addr) {
  return ((uint16_t)mimic_device.registers[base_addr] << 8) | 
         ((uint16_t)mimic_device.registers[base_addr + 1]);   
}
