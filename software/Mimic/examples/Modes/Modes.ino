#include <Wire.h>
#include <Mimic.h>
#include <FspTimer.h>

// --- Hardware & Library Configuration ---
Mimic0x mimic(MIMIC_VDD_5V);
volatile uint16_t *const DAC_REG = (uint16_t *)0x4005E000;

// --- Waveform Configuration Parameters ---
volatile float waveFreq = 100.0; // Base waveform frequency (100Hz scale)
#define SINE_TABLE_SIZE 256

// Waveform amplitude constants (12-bit DAC: 0 - 4095)
const uint16_t WAVE_AMP_HIGH = 3072; // 4096 - 1024
const uint16_t WAVE_AMP_LOW  = 1024; // 0 + 1024
const uint16_t SINE_CENTER   = 2048;
const uint16_t SINE_VAL_AMP  = 1024;

// --- DSP Filter Cutoff Parameters ---
const float LPF_LIGHT_FC = waveFreq * 10; // 1kHz cutoff for light smoothing
const float LPF_HEAVY_FC = waveFreq;      // 100Hz cutoff for heavy smoothing
const float HPF_FC       = waveFreq * 10; // 1kHz cutoff for high-pass (differentiation)
const float BPF_FC       = waveFreq * 3;  // 300Hz cutoff for 3rd harmonic extraction
const float BPF_Q        = 5.0;           // Sharpness of band-pass extraction

// --- Special DSP Function Parameters ---
const float SH_FREQ_HZ = waveFreq * 10;   // S&H sampling frequency (10x coarser than waveform)
const float TH_DUTY_CYCLE = 0.5;          // Track time ratio (0.0 to 1.0)
const float VREF_MV = MIMIC_VDD_5V / 2;   // Center voltage reference for rectification
const float SLEW_LIMIT_VAL = 0.001f;      // Maximum rate of change per sample
const float ENVELOPE_TIME_US = 10000.0f;  // Decay time constant for envelope follower
const uint16_t SCHMITT_HIGH_MV = 3000;    // Upper threshold for Schmitt trigger
const uint16_t SCHMITT_LOW_MV = 2000;     // Lower threshold for Schmitt trigger

// --- State Management ---
enum DemoMode {
  MODE_PULSE_BYPASS = 0,
  MODE_PULSE_LPF_LIGHT,
  MODE_PULSE_LPF_HEAVY,
  MODE_PULSE_LPF2_BESSEL,
  MODE_PULSE_LPF2_BUTTERWORTH,
  MODE_PULSE_LPF2_CHEBYSHEV,
  MODE_PULSE_SLEW_LIMIT,
  MODE_PULSE_HPF,
  MODE_PULSE_HPF2_BESSEL,
  MODE_PULSE_HPF2_BUTTERWORTH,
  MODE_PULSE_HPF2_CHEBYSHEV,
  MODE_PULSE_BPF,
  MODE_SINE_BYPASS,
  MODE_SINE_BIQUAD_UNITY,
  MODE_SINE_SH,
  MODE_SINE_TH,
  MODE_SINE_ENVELOPE,
  MODE_SINE_RECTIFIER,
  MODE_SINE_LOG_AMP,
  MODE_SINE_SCHMITT,
  MAX_MODES
};

volatile DemoMode currentMode = MODE_PULSE_BYPASS;
volatile unsigned long sample_count = 0;
unsigned long samples_per_period;
FspTimer timer;
uint16_t sine_table[SINE_TABLE_SIZE];

// --- Timer Interrupt Service Routine ---
void timer_callback(timer_callback_args_t *args) {
  uint16_t out_val = 0;
  bool isSine = (currentMode >= MODE_SINE_BYPASS);
  float phase = (float)sample_count / samples_per_period;

  if (!isSine) {
    // Generate Square/Pulse Wave
    out_val = (phase < 0.5) ? WAVE_AMP_HIGH : WAVE_AMP_LOW;
  } else {
    // Generate Sine Wave
    int index = (int)(phase * SINE_TABLE_SIZE);
    out_val = sine_table[index];
  }

  *DAC_REG = out_val;

  sample_count++;
  if (sample_count >= samples_per_period) {
    sample_count = 0;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  analogWriteResolution(12);
  analogWrite(A0, 0); 

  delay(2000);
  Serial.println("\n=== Analog Mimic Multi-Mode Demo (100Hz Scale) ===");
  
  if (!mimic.begin(&Wire)) {
    Serial.println("Error: Mimic Init Failed.");
    while(1);
  }

  // Pre-calculate lookup table for sine wave
  for (int i = 0; i < SINE_TABLE_SIZE; i++) {
    float angle = 2.0 * PI * i / SINE_TABLE_SIZE;
    sine_table[i] = SINE_CENTER + (int)(SINE_VAL_AMP * sin(angle));
  }

  samples_per_period = (unsigned long)(MIMIC_ADC_SAMPLING_HZ / waveFreq);

  uint8_t timer_type = 0;
  int8_t timer_ch = FspTimer::get_available_timer(timer_type);
  
  timer.begin(TIMER_MODE_PERIODIC, timer_type, timer_ch, MIMIC_ADC_SAMPLING_HZ, 50, timer_callback);
  timer.setup_overflow_irq();
  timer.open();
  timer.start(); 

  Serial.println("Type 0-9 or a-j to select a mode.");
  setDemoMode(currentMode);
}

void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    int modeIndex = -1;

    if (c >= '0' && c <= '9') {
      modeIndex = c - '0';
    } else if (c >= 'a' && c <= 'z') {
      modeIndex = 10 + (c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
      modeIndex = 10 + (c - 'A');
    }

    if (modeIndex >= 0 && modeIndex < MAX_MODES) {
      currentMode = static_cast<DemoMode>(modeIndex);
      setDemoMode(currentMode);
    }
  }
}

void setDemoMode(DemoMode mode) {
  Serial.print("\n[Mode "); Serial.print(mode); Serial.print("] ");
  
  switch (mode) {
    case MODE_PULSE_BYPASS:
      Serial.println("PULSE -> BYPASS (Original Signal)");
      mimic.setBypassDSP();
      break;

    case MODE_PULSE_LPF_LIGHT:
      Serial.println("PULSE -> 1st-Order LPF (1kHz): Slightly rounds the edges");
      mimic.setFilter1stLPF(LPF_LIGHT_FC);
      break;

    case MODE_PULSE_LPF_HEAVY:
      Serial.println("PULSE -> 1st-Order LPF (100Hz): Heavily smooths the pulse wave");
      mimic.setFilter1stLPF(LPF_HEAVY_FC);
      break;

    case MODE_PULSE_LPF2_BESSEL:
      Serial.println("PULSE -> Biquad LPF (1kHz, Bessel): Smooth response with minimal ringing");
      mimic.setFilterBiquadLPF(LPF_LIGHT_FC, MIMIC_FILTER_Q_BESSEL);
      break;

    case MODE_PULSE_LPF2_BUTTERWORTH:
      Serial.println("PULSE -> Biquad LPF (1kHz, Butterworth): Maximally flat passband response");
      mimic.setFilterBiquadLPF(LPF_LIGHT_FC, MIMIC_FILTER_Q_BUTTERWORTH);
      break;

    case MODE_PULSE_LPF2_CHEBYSHEV:
      Serial.println("PULSE -> Biquad LPF (1kHz, Chebyshev): Sharp cutoff with passband ripple");
      mimic.setFilterBiquadLPF(LPF_LIGHT_FC, MIMIC_FILTER_Q_CHEBYSHEV);
      break;

    case MODE_PULSE_SLEW_LIMIT:
      Serial.println("PULSE -> Slew Rate Limiter: Converts square wave into triangle wave");
      mimic.setSlewRateLimiter(SLEW_LIMIT_VAL); 
      break;

    case MODE_PULSE_HPF:
      Serial.println("PULSE -> 1st-Order HPF (1kHz): High-pass differentiated spikes");
      mimic.setFilter1stHPF(HPF_FC);
      break;

    case MODE_PULSE_HPF2_BESSEL:
      Serial.println("PULSE -> Biquad HPF (1kHz, Bessel): Smooth phase high-pass filtering");
      mimic.setFilterBiquadHPF(HPF_FC, MIMIC_FILTER_Q_BESSEL);
      break;

    case MODE_PULSE_HPF2_BUTTERWORTH:
      Serial.println("PULSE -> Biquad HPF (1kHz, Butterworth): Maximally flat high-pass response");
      mimic.setFilterBiquadHPF(HPF_FC, MIMIC_FILTER_Q_BUTTERWORTH);
      break;

    case MODE_PULSE_HPF2_CHEBYSHEV:
      Serial.println("PULSE -> Biquad HPF (1kHz, Chebyshev): Sharp high-pass with ripple");
      mimic.setFilterBiquadHPF(HPF_FC, MIMIC_FILTER_Q_CHEBYSHEV);
      break;

    case MODE_PULSE_BPF:
      Serial.println("PULSE -> Biquad BPF (300Hz, Q=5.0): Extracts 3rd harmonic (300Hz sine wave)");
      mimic.setFilterBiquadBPF(BPF_FC, BPF_Q);
      break;

    case MODE_SINE_BYPASS:
      Serial.println("SINE  -> BYPASS (Original Signal)");
      mimic.setBypassDSP();
      break;

    case MODE_SINE_BIQUAD_UNITY:
      Serial.println("SINE  -> Unity-gain Biquad (Flat Bypass Mode)");
      mimic.setFilterBiquadUnity();
      break;

    case MODE_SINE_SH:
      Serial.print("SINE  -> Sample & Hold (");
      Serial.print(SH_FREQ_HZ);
      Serial.println(" Hz): Generates a step-like staircase waveform");
      mimic.setSampleAndHold(SH_FREQ_HZ); 
      break;

    case MODE_SINE_TH:
      Serial.print("SINE  -> Track & Hold (");
      Serial.print(SH_FREQ_HZ);
      Serial.print(" Hz, Track Duty: ");
      Serial.print(TH_DUTY_CYCLE * 100);
      Serial.println("%): Alternates between tracking and holding the input");
      mimic.setTrackAndHold(SH_FREQ_HZ, TH_DUTY_CYCLE); 
      break;

    case MODE_SINE_ENVELOPE:
      Serial.println("SINE  -> Envelope Follower: Extracts amplitude boundaries");
      mimic.setEnvelopeFollower(ENVELOPE_TIME_US, 0); 
      break;

    case MODE_SINE_RECTIFIER:
      Serial.println("SINE  -> Full-wave Rectifier: Inverts negative phases to double frequency");
      mimic.setRectifierFull(VREF_MV);
      break;

    case MODE_SINE_LOG_AMP:
      Serial.println("SINE  -> Log Amplifier: Logarithmic distortion via soft peak compression");
      mimic.setNonlinearLog(); 
      break;

    case MODE_SINE_SCHMITT:
      Serial.println("SINE  -> Schmitt Trigger: Hysteresis comparator window operation");
      mimic.setComparatorSchmitt(SCHMITT_HIGH_MV, SCHMITT_LOW_MV); 
      break;
      
    default:
      break;
  }
}
