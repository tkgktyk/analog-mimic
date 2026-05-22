/**
 ******************************************************************************
 * @file    Mimic.c
 * @author  TAKAGI Katsuyuki
 * @brief   Implementation of Host-Side Helper Functions, I2C Command Packet 
 * Serialization, and Register-Mapping Wrapper APIs.
 *-----------------------------------------------------------------------------
 * Copyright (C) 2026 TAKAGI Katsuyuki
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 ******************************************************************************
 */

#include "Mimic.h"
#include <math.h>

namespace {
    // Internal enumeration for filter types, hidden from the public API
    enum class InternalFilterType : uint8_t {
        LPF = 0,
        HPF,
        BPF,
        NOTCH
    };

    // Constants for I2C communication delays (in microseconds)
    constexpr unsigned long I2C_WRITE_DELAY_US = 100;
}

// =========================================================
// Internal Helper Functions
// =========================================================

static void calculateScaleAndShift(float inputVal, float* outFract, int8_t* outShift) {
    *outShift = 0;
    *outFract = inputVal;
    while (fabs(*outFract) >= 1.0f && *outShift < 31) {
        *outFract /= 2.0f;
        (*outShift)++;
    }
}

static inline int16_t clamp16(int32_t val) {
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return (int16_t)val;
}

// =========================================================
// MimicBase Implementation
// =========================================================

MimicBase::MimicBase(uint16_t vddMv, uint8_t hardwareJumper) : 
    _i2c_addr(MIMIC_DEFAULT_I2C_ADDR + (hardwareJumper & 0x0F)),
    _wire(nullptr),
    _vdd_mv(vddMv),
    _fs_hz(MIMIC_ADC_SAMPLING_HZ),
    _global_flags(0),
    _shift_raw(0) {}

bool MimicBase::begin(TwoWire *wire) {
    _wire = wire;
    _global_flags = 0;
    _shift_raw = 0;
    updateGlobalFlags();
    updateCommonRegisters();
    return true;
}

// --- Internal I2C Communication Helpers (DRY Implementation) ---

uint8_t MimicBase::readRegister8(uint8_t regAddr) {
    _wire->beginTransmission(_i2c_addr);
    _wire->write(regAddr);
    if (_wire->endTransmission() != 0) return 0xFF; 

    uint8_t received = _wire->requestFrom(_i2c_addr, (uint8_t)1);
    if (_wire->available()) {
         return _wire->read();
    } else {
        Serial.print("[I2C Rx Error! Requested 1, Got ");
        Serial.print(received);
        Serial.println("]");
        return 0xFF; 
    }
}

uint16_t MimicBase::readRegister16(uint8_t regAddr) {
    _wire->beginTransmission(_i2c_addr);
    _wire->write(regAddr); 
    if (_wire->endTransmission() != 0) return 0xFFFF; 
  
    uint8_t received = _wire->requestFrom(_i2c_addr, (uint8_t)2); 
    if (_wire->available() == 2) {
        uint8_t h = _wire->read();
        uint8_t l = _wire->read();
        return (h << 8) | l;
    } else {
        return 0xFFFF; 
    }
}

// --- Public Register Accessors ---

uint8_t MimicBase::getValue(uint8_t addr) {
    return readRegister8(addr);
}

uint8_t MimicBase::getStatus() {
    return readRegister8(MIMIC_REG_STATUS);
}

uint16_t MimicBase::getAdcValue() {
    return readRegister16(MIMIC_REG_ADC_VAL);
}

uint16_t MimicBase::getCpuCycles() {
    return readRegister16(MIMIC_REG_CPU_CYCLES);
}

// --- Common Registers & Flags ---

void MimicBase::updateGlobalFlags() {
    _wire->beginTransmission(_i2c_addr);
    _wire->write(MIMIC_REG_GLOBAL_FLAGS);
    _wire->write(_global_flags);
    _wire->endTransmission();
    
    // Wait briefly for PY32 to detect flag differences and stop/open the OPA
    delayMicroseconds(I2C_WRITE_DELAY_US);
}

void MimicBase::updateCommonRegisters() {
    _wire->beginTransmission(_i2c_addr);
    _wire->write(MIMIC_REG_COMMON_START);
    _wire->write((_shift_raw >> 8) & 0xFF);
    _wire->write(_shift_raw & 0xFF);
    _wire->endTransmission();
    
    // Added delay for timing safety
    delayMicroseconds(I2C_WRITE_DELAY_US);
}

void MimicBase::setOutputOpen(bool openEnabled) {
    if (openEnabled) _global_flags |= MIMIC_FLAG_OUT_OPEN;
    else _global_flags &= ~MIMIC_FLAG_OUT_OPEN;
    updateGlobalFlags();
}

void MimicBase::setOutputInverted(bool inverted) {
    if (inverted) _global_flags |= MIMIC_FLAG_INV_OUT;
    else _global_flags &= ~MIMIC_FLAG_INV_OUT;
    updateGlobalFlags();
}

void MimicBase::setOutputOffset(int16_t offsetMv) {
    _shift_raw = mvToRawSigned(offsetMv);
    updateCommonRegisters();
}

void MimicBase::writeModeAndPayload(uint8_t mode, const uint8_t *payload, uint8_t length) {
    _wire->beginTransmission(_i2c_addr);
    _wire->write(MIMIC_REG_MODE_SELECT); // Specify target address 0x10
    _wire->write(mode);                  // Write to 0x10. Subsequent bytes auto-increment to 0x11.
    
    // Transmit the payload in a single burst (MCU is idling, so buffer overflow will not occur)
    if (length > 0 && payload != nullptr) {
        for (uint8_t i = 0; i < length; i++) {
            _wire->write(payload[i]);           
        }
    }
    _wire->endTransmission();
    delayMicroseconds(I2C_WRITE_DELAY_US); 
}

// --- Conversion Utilities ---

uint16_t MimicBase::mvToRaw(uint16_t mv) {
    uint32_t raw = ((uint32_t)mv * 4096) / _vdd_mv;
    return (raw > 4095) ? 4095 : (uint16_t)raw;
}

int16_t MimicBase::mvToRawSigned(int16_t mv) {
    // Process as int32_t to support negative calculations safely
    int32_t raw = ((int32_t)mv * 4096) / (int32_t)_vdd_mv;
    return (int16_t)raw;
}

int16_t MimicBase::floatToQ15(float value) {
    if (value >= 1.0f) return 32767;    
    if (value <= -1.0f) return -32768;  
    return (int16_t)(value * 32768.0f);
}

void MimicBase::floatToPayloadQ15(float value, uint8_t* buffer, uint8_t offset) {
    int16_t q15 = floatToQ15(value);
    buffer[offset + 0] = (q15 >> 8) & 0xFF; // MSB
    buffer[offset + 1] = q15 & 0xFF;        // LSB
}

void MimicBase::rawToPayload16(uint16_t raw, uint8_t* buffer, uint8_t offset) {
    buffer[offset + 0] = (raw >> 8) & 0xFF;  // MSB
    buffer[offset + 1] = raw & 0xFF;         // LSB
}

void MimicBase::int16ToPayload(int32_t value, uint8_t* buffer, uint8_t offset) {
    // Safely clip to -32768 to +32767 in case of any overflow during correction calculations
    int16_t q15 = clamp16(value); 
    
    buffer[offset + 0] = (q15 >> 8) & 0xFF; // MSB
    buffer[offset + 1] = q15 & 0xFF;        // LSB
}

// -------------------------------------------------------------
// Functional Mode APIs
// -------------------------------------------------------------

void MimicBase::setBypassDSP() {
    writeModeAndPayload(MIMIC_MODE_ID_BYPASS_DSP, nullptr, 0);
}

// --- 1st Order Filters ---

void MimicBase::setFilter1stLPF(float fc) {
    uint8_t payload[2];
    float alpha = 1.0f - exp(-2.0f * PI * fc / _fs_hz);
    floatToPayloadQ15(alpha, payload, 0);
    writeModeAndPayload(MIMIC_MODE_ID_FILTER_1ST_LPF, payload, 2);
}

void MimicBase::setFilter1stHPF(float fc) {
    uint8_t payload[2];
    float alpha = 1.0f - exp(-2.0f * PI * fc / _fs_hz);
    floatToPayloadQ15(alpha, payload, 0);
    writeModeAndPayload(MIMIC_MODE_ID_FILTER_1ST_HPF, payload, 2);
}

// --- Biquad Filters ---

void MimicBase::sendBiquadCoeffs_Normalized(float b0, float b1, float b2, float a0, float a1, float a2, uint8_t filterType) {
    // 1. Convert coefficients into 32-bit integers (int32_t)
    int32_t a1_q32 = round((-a1 / a0) * 16384.0f);
    int32_t a2_q32 = round((-a2 / a0) * 16384.0f);
    int32_t b0_q32 = round((b0 / a0) * 16384.0f);
    int32_t b1_q32 = round((b1 / a0) * 16384.0f);
    int32_t b2_q32 = round((b2 / a0) * 16384.0f);

    // 2. Perform gain correction calculations safely in 32-bit space (exceeding 32768 here is fine)
    if (filterType == static_cast<uint8_t>(InternalFilterType::LPF)) {
        int32_t target_sum = 16384 - a1_q32 - a2_q32;
        int32_t current_sum = b0_q32 + b1_q32 + b2_q32;
        b1_q32 += (target_sum - current_sum); 
    } 
    else if (filterType == static_cast<uint8_t>(InternalFilterType::HPF)) {
        int32_t target_sum = 16384 + a1_q32 - a2_q32;
        int32_t current_sum = b0_q32 - b1_q32 + b2_q32;
        b1_q32 -= (target_sum - current_sum);
    }

    // 3. Write computed 32-bit integers into the payload.
    // (Internally clamped to -32768 ~ +32767 via clamp16() inside int16ToPayload)
    uint8_t payload[10];
    int16ToPayload(b0_q32, payload, 0);
    int16ToPayload(b1_q32, payload, 2);
    int16ToPayload(b2_q32, payload, 4);
    int16ToPayload(a1_q32, payload, 6);
    int16ToPayload(a2_q32, payload, 8);
    
    writeModeAndPayload(MIMIC_MODE_ID_FILTER_BIQUAD, payload, 10);
}

/**
 * @brief Unity-Gain Biquad (Executes full computation path but passes signal through unchanged)
 * Configures coefficients so that the transfer function H(z) = 1.
 * Used to compare signal quality against bypass mode while maintaining the computation load (212 cycles).
 */
void MimicBase::setFilterBiquadUnity() {
    // Coeffs mapped for: y[n] = 1.0*x[n] + 0.0*x[n-1] + 0.0*x[n-2] - 0.0*y[n-1] - 0.0*y[n-2]
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a0 = 1.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;

    // Route to common function (Internal LPF type used for measurement, converted to Q15 internally).
    // Normalized by a0=1.0, so b0 is internally converted to 0.5 (Q15: 0x4000), etc.
    sendBiquadCoeffs_Normalized(b0, b1, b2, a0, a1, a2, static_cast<uint8_t>(InternalFilterType::LPF));
}

void MimicBase::setFilterBiquadLPF(float fc, float Q) {
    float omega = 2.0f * PI * fc / _fs_hz;
    float sin_w = sin(omega);
    float cos_w = cos(omega);
    float alpha = sin_w / (2.0f * Q);

    // Audio EQ Cookbook LPF Formulas
    float b0 =  (1.0f - cos_w) / 2.0f;
    float b1 =   1.0f - cos_w;
    float b2 =  (1.0f - cos_w) / 2.0f;
    float a0 =   1.0f + alpha;
    float a1 =  -2.0f * cos_w;
    float a2 =   1.0f - alpha;

    // Scaled by 0.5 & converted to Q15 internally; handles DC gain variance from rounding errors.
    sendBiquadCoeffs_Normalized(b0, b1, b2, a0, a1, a2, static_cast<uint8_t>(InternalFilterType::LPF));
}

void MimicBase::setFilterBiquadHPF(float fc, float Q) {
    float omega = 2.0f * PI * fc / _fs_hz;
    float sin_w = sin(omega);
    float cos_w = cos(omega);
    float alpha = sin_w / (2.0f * Q);

    float b0 =  (1.0f + cos_w) / 2.0f;
    float b1 = -(1.0f + cos_w);
    float b2 =  (1.0f + cos_w) / 2.0f;
    float a0 =   1.0f + alpha;
    float a1 =  -2.0f * cos_w;
    float a2 =   1.0f - alpha;

    sendBiquadCoeffs_Normalized(b0, b1, b2, a0, a1, a2, static_cast<uint8_t>(InternalFilterType::HPF));
}

void MimicBase::setFilterBiquadBPF(float fc, float Q) {
    float omega = 2.0f * PI * fc / _fs_hz;
    float sin_w = sin(omega);
    float cos_w = cos(omega);
    float alpha = sin_w / (2.0f * Q);

    float b0 =   alpha;
    float b1 =   0.0f;
    float b2 =  -alpha;
    float a0 =   1.0f + alpha;
    float a1 =  -2.0f * cos_w;
    float a2 =   1.0f - alpha;

    sendBiquadCoeffs_Normalized(b0, b1, b2, a0, a1, a2, static_cast<uint8_t>(InternalFilterType::BPF));
}

void MimicBase::setFilterBiquadNotch(float fc, float Q) {
    float omega = 2.0f * PI * fc / _fs_hz;
    float sin_w = sin(omega);
    float cos_w = cos(omega);
    float alpha = sin_w / (2.0f * Q);

    float b0 =   1.0f;
    float b1 =  -2.0f * cos_w;
    float b2 =   1.0f;
    float a0 =   1.0f + alpha;
    float a1 =  -2.0f * cos_w;
    float a2 =   1.0f - alpha;

    sendBiquadCoeffs_Normalized(b0, b1, b2, a0, a1, a2, static_cast<uint8_t>(InternalFilterType::NOTCH));
}

// --- PGA ---

void MimicBase::setPGA(float gain, uint16_t refMv) {
    uint8_t payload[5]; 
    float fract;
    int8_t shift;
    
    calculateScaleAndShift(gain, &fract, &shift);
    
    floatToPayloadQ15(fract, payload, 0); 
    payload[2] = (uint8_t)shift;          
    rawToPayload16(mvToRaw(refMv), payload, 3); // Reference evaluation point
    
    writeModeAndPayload(MIMIC_MODE_ID_PGA, payload, 5);
}

// --- Nonlinear LUT ---

void MimicBase::setNonlinearLog() {
    uint8_t payload[1] = {0}; // 0: Log
    writeModeAndPayload(MIMIC_MODE_ID_NONLINEAR_LUT, payload, 1);
}

void MimicBase::setNonlinearAntilog() {
    uint8_t payload[1] = {1}; // 1: Antilog
    writeModeAndPayload(MIMIC_MODE_ID_NONLINEAR_LUT, payload, 1);
}

// --- Comparators ---

void MimicBase::setComparatorWindow(uint16_t upperMv, uint16_t lowerMv) {
    uint8_t payload[4];
    rawToPayload16(mvToRaw(upperMv), payload, 0);
    rawToPayload16(mvToRaw(lowerMv), payload, 2);
    writeModeAndPayload(MIMIC_MODE_ID_COMPARATOR_WIN, payload, 4);
}

void MimicBase::setComparatorSchmitt(uint16_t upperMv, uint16_t lowerMv) {
    uint8_t payload[4];
    rawToPayload16(mvToRaw(upperMv), payload, 0);
    rawToPayload16(mvToRaw(lowerMv), payload, 2);
    writeModeAndPayload(MIMIC_MODE_ID_COMPARATOR_SCHMITT, payload, 4);
}

// --- Waveform Analysis ---

void MimicBase::setEnvelopeFollower(float decayTimeUs, uint8_t polarity) {
    uint8_t payload[3]; 
    float tau = decayTimeUs / 1000000.0f;
    float decayFactor = (tau <= 0.0f) ? 0.0f : exp(-1.0f / (_fs_hz * tau));
    
    floatToPayloadQ15(decayFactor, payload, 0);
    payload[2] = polarity;
    writeModeAndPayload(MIMIC_MODE_ID_ENVELOPE_FOLLOWER, payload, 3);
}

void MimicBase::setPeakHold(uint8_t polarity) {
    uint8_t payload[3];

    floatToPayloadQ15(1.0f, payload, 0);
    payload[2] = polarity;
    writeModeAndPayload(MIMIC_MODE_ID_ENVELOPE_FOLLOWER, payload, 3);
}

// --- Sample & Hold ---

void MimicBase::setSampleAndHold(float freqHz) {
    uint8_t payload[4];

    // Safety guard: Limit frequencies below zero or exceeding system Fs
    if (freqHz <= 0.0f) freqHz = 0.1f; 
    if (freqHz > _fs_hz) freqHz = _fs_hz;

    // Calculate processing interval (Period) from target frequency (Hz)
    uint16_t periodSamples = (uint16_t)(_fs_hz / freqHz);
    if (periodSamples == 0) periodSamples = 1;

    rawToPayload16(periodSamples, payload, 0);
    rawToPayload16(1, payload, 2); // Track width = 1 sample (S&H definition)

    writeModeAndPayload(MIMIC_MODE_ID_SAMPLE_AND_HOLD, payload, 4);
}

// --- Track & Hold ---

void MimicBase::setTrackAndHold(float freqHz, float trackRatio) {
    uint8_t payload[4];

    if (freqHz <= 0.0f) freqHz = 0.1f;
    if (freqHz > _fs_hz) freqHz = _fs_hz;

    uint16_t periodSamples = (uint16_t)(_fs_hz / freqHz);
    if (periodSamples == 0) periodSamples = 1;

    // Determine Track samples based on ratio (0.0 ~ 1.0)
    if (trackRatio < 0.0f) trackRatio = 0.0f;
    if (trackRatio > 1.0f) trackRatio = 1.0f;
    
    uint16_t trackSamples = (uint16_t)(periodSamples * trackRatio);
    if (trackSamples == 0) trackSamples = 1; // Always track at least 1 sample

    rawToPayload16(periodSamples, payload, 0);
    rawToPayload16(trackSamples, payload, 2);

    writeModeAndPayload(MIMIC_MODE_ID_SAMPLE_AND_HOLD, payload, 4);
}

// --- Clipper ---

void MimicBase::setClipper(uint16_t upperMv, uint16_t lowerMv) {
    uint8_t payload[4];
    rawToPayload16(mvToRaw(upperMv), payload, 0);
    rawToPayload16(mvToRaw(lowerMv), payload, 2);
    writeModeAndPayload(MIMIC_MODE_ID_CLIPPER, payload, 4);
}

// --- Calculus ---

void MimicBase::setMathDerivative(float timeConstantUs) {
    uint8_t payload[3]; 
    float tau = timeConstantUs / 1000000.0f;
    float scale = tau * _fs_hz; 
    
    float fract;
    int8_t shift;
    
    calculateScaleAndShift(scale, &fract, &shift);
    
    floatToPayloadQ15(fract, payload, 0);
    payload[2] = (uint8_t)shift;
    writeModeAndPayload(MIMIC_MODE_ID_MATH_DERIVATIVE, payload, 3);
}

void MimicBase::setMathIntegral(float timeConstantUs, int8_t inputShift) {
    uint8_t payload[3]; 
    float tau = timeConstantUs / 1000.0f;
    float leakFactor = (tau <= 0.0f) ? 1.0f : exp(-1.0f / (_fs_hz * tau));
    
    floatToPayloadQ15(leakFactor, payload, 0);
    payload[2] = (uint8_t)inputShift; 
    
    writeModeAndPayload(MIMIC_MODE_ID_MATH_INTEGRAL, payload, 3);
}

void MimicBase::setRectifierHalf(bool polarity, uint16_t vrefMv) {
    // Half-wave rectification behaves similarly to an analog clipping circuit; delegate to setClipper
    if (polarity == MIMIC_POLALYTY_NEGATIVE) setClipper(vrefMv, 0);
    else setClipper(_vdd_mv, vrefMv);
}

void MimicBase::setRectifierFull(uint16_t vrefMv) {
    uint8_t payload[2];
    rawToPayload16(mvToRaw(vrefMv), payload, 0);
    writeModeAndPayload(MIMIC_MODE_ID_RECTIFIER_FULL, payload, 2);
}

void MimicBase::setSlewRateLimiter(float maxChangePerSampleRatio) {
    uint8_t payload[3];
    // Scale target ratio relative to ADC Full Scale (4096), matching DSP's internal Q15 shift (<<4)
    // E.g., 0.1 (10%) -> 409.6 * 16 = 6553.6 -> 6554
    float scaledChange = maxChangePerSampleRatio * 4096.0f * 16.0f;
    
    // Convert and store as Q15 (Add bounds-checking as needed)
    floatToPayloadQ15(scaledChange / 32768.0f, payload, 0); 
    payload[2] = 0;
    writeModeAndPayload(MIMIC_MODE_ID_SLEW_RATE_LIMITER, payload, 3);
}

void MimicBase::setDigitalDelay(uint16_t delaySamples) {
    uint8_t payload[2];
    rawToPayload16(delaySamples, payload, 0);
    writeModeAndPayload(MIMIC_MODE_ID_DELAY, payload, 2);
}

// =========================================================
// Mimic1x Implementation
// =========================================================

void Mimic1x::setHardwareServoEnabled(bool enabled) {
    if (enabled) _global_flags |= MIMIC_FLAG_SERVO_EN;
    else _global_flags &= ~MIMIC_FLAG_SERVO_EN;
    updateGlobalFlags();
}
