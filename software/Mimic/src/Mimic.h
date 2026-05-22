/**
 ******************************************************************************
 * @file    Mimic.h
 * @author  TAKAGI Katsuyuki
 * @brief   Host-Side Helper Functions and Arduino Library API for 
 * Communicating with the Analog Mimic Hardware via I2C.
 *-----------------------------------------------------------------------------
 * Copyright (C) 2026 TAKAGI Katsuyuki
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 ******************************************************************************
 */

#ifndef MIMIC_H
#define MIMIC_H

#include <Arduino.h>
#include <Wire.h>

// Include shared register definitions (Single Source of Truth)
#include "mimic_registers.h"

// Constants for Power Supply Voltage Selection
#define MIMIC_VDD_3V3 3300
#define MIMIC_VDD_5V 5000

// =========================================================
// Filter Constants (Q-Factors)
// =========================================================
#define MIMIC_FILTER_Q_BUTTERWORTH 0.7071f
#define MIMIC_FILTER_Q_BESSEL 0.5773f
#define MIMIC_FILTER_Q_CHEBYSHEV 1.0f

// =========================================================
// MimicBase (Core interface for all variants)
// =========================================================
/**
 * @brief Core interface for the Analog Mimic module.
 * * This class abstracts the low-level I2C communication and Q15 fixed-point
 * arithmetic, allowing users to configure the hardware-deterministic DSP 
 * engine using intuitive physical units (Hz, mV, microseconds, etc.).
 */
class MimicBase
{
public:
  /**
   * @brief Initializes the Mimic module and synchronizes initial states.
   * @param wire Pointer to the TwoWire (I2C) instance. Defaults to &Wire.
   * @return true if initialization is successful.
   */
  bool begin(TwoWire *wire = &Wire);

  // --- Read Section ---

  /**
   * @brief Reads a raw 8-bit value from a specific I2C register.
   * @param addr Register address.
   * @return The 8-bit register value.
   */
  uint8_t getValue(uint8_t addr);

  /**
   * @brief Retrieves the current status flag of the module.
   * @return 8-bit status code.
   */
  uint8_t getStatus();

  /**
   * @brief Reads the current raw 12-bit ADC value (0-4095).
   * @return 12-bit raw ADC reading.
   */
  uint16_t getAdcValue();

  /**
   * @brief Retrieves the CPU cycle count consumed by the current DSP routine.
   * @return Number of clock cycles executed in the Primary ISR.
   */
  uint16_t getCpuCycles();

  // --- Global Configurations (Pipeline Pre/Post-Processing) ---

  /**
   * @brief Enables or disables the output (Hi-Z state if supported).
   * @param openEnabled True to open/disable output, False to enable.
   */
  void setOutputOpen(bool openEnabled);

  /**
   * @brief Applies an absolute hardware-level polarity inversion to the final output.
   * @param inverted True to invert the DAC output (4095 - val), False for normal.
   */
  void setOutputInverted(bool inverted);

  /**
   * @brief Applies a manual DC offset to the output signal.
   * @param offsetMv Offset voltage in millivolts (can be negative).
   */
  void setOutputOffset(int16_t offsetMv);

  // --- 0. Bypass DSP ---

  /**
   * @brief Bypasses all DSP processing, acting as a simple voltage follower.
   */
  void setBypassDSP();

  // --- 1. Filtering ---

  /**
   * @brief Configures a 1st-order Low-Pass Filter (RC filter equivalent).
   * @param fc Cutoff frequency in Hz.
   */
  void setFilter1stLPF(float fc);

  /**
   * @brief Configures a 1st-order High-Pass Filter.
   * @param fc Cutoff frequency in Hz.
   */
  void setFilter1stHPF(float fc);

  /**
   * @brief Configures a Unity-Gain Biquad. Passes signal unchanged but consumes 
   * the exact same CPU cycles as other Biquad filters for profiling purposes.
   */
  void setFilterBiquadUnity();

  /**
   * @brief Configures a 2nd-order Biquad Low-Pass Filter.
   * @param fc Cutoff frequency in Hz.
   * @param Q Quality factor. Defaults to Butterworth (0.7071).
   */
  void setFilterBiquadLPF(float fc, float Q = MIMIC_FILTER_Q_BUTTERWORTH);

  /**
   * @brief Configures a 2nd-order Biquad High-Pass Filter.
   * @param fc Cutoff frequency in Hz.
   * @param Q Quality factor. Defaults to Butterworth (0.7071).
   */
  void setFilterBiquadHPF(float fc, float Q = MIMIC_FILTER_Q_BUTTERWORTH);

  /**
   * @brief Configures a 2nd-order Biquad Band-Pass Filter.
   * @param fc Center frequency in Hz.
   * @param Q Quality factor (bandwidth control). Defaults to 1.0.
   */
  void setFilterBiquadBPF(float fc, float Q = 1.0f);

  /**
   * @brief Configures a 2nd-order Biquad Notch (Band-Stop) Filter.
   * @param fc Center frequency in Hz.
   * @param Q Quality factor (notch width control). Defaults to 1.0.
   */
  void setFilterBiquadNotch(float fc, float Q = 1.0f);

  // --- 2. Programmable Gain Amplifier (PGA) ---

  /**
   * @brief Configures the Programmable Gain Amplifier.
   * @param gain Linear gain multiplier (e.g., 5.5 for 5.5x amplification).
   * @param refMv Reference voltage in mV for the amplification center (DC offset).
   */
  void setPGA(float gain, uint16_t refMv = 2500);

  // --- 3. Non-linear Amplifier ---

  /**
   * @brief Configures a Logarithmic Amplifier.
   */
  void setNonlinearLog();

  /**
   * @brief Configures an Exponential (Anti-Log) Amplifier.
   */
  void setNonlinearAntilog();

  // --- 4. Comparators ---

  /**
   * @brief Configures a Window Comparator.
   * @param upperMv Upper threshold voltage in mV.
   * @param lowerMv Lower threshold voltage in mV.
   */
  void setComparatorWindow(uint16_t upperMv, uint16_t lowerMv);

  /**
   * @brief Configures a Schmitt Trigger with hysteresis.
   * @param upperMv Upper threshold voltage in mV (Trigger ON).
   * @param lowerMv Lower threshold voltage in mV (Trigger OFF).
   */
  void setComparatorSchmitt(uint16_t upperMv, uint16_t lowerMv);

  // --- 5. Waveform Analysis ---

  /**
   * @brief Configures an Envelope Follower (AM detector).
   * @param decayTimeUs Release/decay time constant in microseconds.
   * @param polarity MIMIC_POLALYTY_POSITIVE (peak) or MIMIC_POLALYTY_NEGATIVE (trough).
   */
  void setEnvelopeFollower(float decayTimeUs, uint8_t polarity = MIMIC_POLALYTY_POSITIVE);

  /**
   * @brief Configures a Peak Hold circuit (Envelope follower with infinite decay).
   * @param polarity MIMIC_POLALYTY_POSITIVE (max hold) or MIMIC_POLALYTY_NEGATIVE (min hold).
   */
  void setPeakHold(uint8_t polarity = MIMIC_POLALYTY_POSITIVE);

  /**
   * @brief Configures a Sample and Hold (S&H) circuit.
   * @param freqHz Sampling frequency in Hz.
   */
  void setSampleAndHold(float freqHz);

  /**
   * @brief Configures a Track and Hold (T&H) circuit.
   * @param freqHz Trigger frequency in Hz.
   * @param trackRatio Ratio of the period spent tracking the signal (0.0 to 1.0).
   */
  void setTrackAndHold(float freqHz, float trackRatio = 0.5f);

  // --- 6. Clipping (Limiter) ---

  /**
   * @brief Configures a Hard Clipper (Limiter).
   * @param upperMv Upper voltage limit in mV.
   * @param lowerMv Lower voltage limit in mV.
   */
  void setClipper(uint16_t upperMv, uint16_t lowerMv);

  // --- 7. Calculus (Math) ---

  /**
   * @brief Configures an Analog Differentiator.
   * @param timeConstantUs RC time constant in microseconds.
   */
  void setMathDerivative(float timeConstantUs);

  /**
   * @brief Configures a Leaky Integrator.
   * @param timeConstantUs RC time constant in microseconds.
   * @param inputShift Bit-shift value for the input to prevent rapid saturation.
   */
  void setMathIntegral(float timeConstantUs, int8_t inputShift = -2);

  /**
   * @brief Configures a Half-Wave Rectifier.
   * @param polarity Positive or Negative half-wave to pass.
   * @param vrefMv Reference center voltage in mV.
   */
  void setRectifierHalf(bool polarity = MIMIC_POLALYTY_POSITIVE, uint16_t vrefMv = MIMIC_ADC_MID_VALUE);

  /**
   * @brief Configures a Full-Wave Rectifier (Absolute value circuit).
   * @param vrefMv Reference center voltage in mV.
   */
  void setRectifierFull(uint16_t vrefMv = MIMIC_ADC_MID_VALUE);

  /**
   * @brief Configures a Slew Rate Limiter.
   * @param maxChangePerSampleRatio Maximum allowed voltage change per sample (0.0 to 1.0 of full scale).
   */
  void setSlewRateLimiter(float maxChangePerSampleRatio);

  /**
   * @brief Configures a pure Digital Delay line (Ring Buffer).
   * @param delaySamples Number of samples to delay (Max 4095).
   * @note Actual delay time = delaySamples * (1.0 / SamplingRate).
   */
  void setDigitalDelay(uint16_t delaySamples);

protected:
  MimicBase(uint16_t vddMv, uint8_t hardwareJumper = 0);

  uint8_t _i2c_addr;
  TwoWire *_wire;
  uint16_t _vdd_mv;
  float _fs_hz;

  // State retention variables
  uint8_t _global_flags;
  uint16_t _delay_samples;
  int16_t _shift_raw;

  uint8_t readRegister8(uint8_t regAddr);
  uint16_t readRegister16(uint8_t regAddr);

  void writeModeAndPayload(uint8_t mode, const uint8_t *payload, uint8_t length);
  void updateGlobalFlags();
  void updateCommonRegisters();

  uint16_t mvToRaw(uint16_t mv);
  int16_t mvToRawSigned(int16_t mv);
  int16_t floatToQ15(float value);
  void floatToPayloadQ15(float value, uint8_t *buffer, uint8_t offset);
  void rawToPayload16(uint16_t raw, uint8_t *buffer, uint8_t offset);
  void int16ToPayload(int32_t value, uint8_t *buffer, uint8_t offset);
  void sendBiquadCoeffs_Normalized(float b0, float b1, float b2, float a0, float a1, float a2, uint8_t filterType);
};

// =========================================================
// Variant Classes
// =========================================================

/**
 * @brief Standard variant of Analog Mimic (mimic0x).
 * Uses internal OPAs for AFE without external hardware modding capabilities.
 */
class Mimic0x : public MimicBase
{
public:
  explicit Mimic0x(uint16_t vddMv, uint8_t hardwareJumper = 0) : MimicBase(vddMv, hardwareJumper) {}
};

/**
 * @brief Extended AFE variant of Analog Mimic (mimic1x).
 * Features an external OPA and DC servo capabilities for advanced sensor interfacing.
 */
class Mimic1x : public MimicBase
{
public:
  explicit Mimic1x(uint16_t vddMv, uint8_t hardwareJumper = 0) : MimicBase(vddMv, hardwareJumper) {}
  
  /**
   * @brief Enables or disables the hardware DC servo function.
   * @note Only available on mimic1x variants with the secondary internal DAC routed to the AFE.
   * @param enabled True to enable the hardware DC servo.
   */
  void setHardwareServoEnabled(bool enabled);
};

#endif // MIMIC_H
