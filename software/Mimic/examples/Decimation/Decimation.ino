#include <Wire.h>
#include <Mimic.h>
#include <FspTimer.h>

// --- Hardware & Library Configuration ---
Mimic0x mimic(MIMIC_VDD_5V);
volatile uint16_t *const DAC_REG = (uint16_t *)0x4005E000;

// --- Waveform Configuration Parameters ---
#define SINE_TABLE_SIZE 256
const uint16_t WAVE_AMP_HIGH = 3072;
const uint16_t WAVE_AMP_LOW  = 1024; 
const uint16_t SINE_CENTER   = 2048;
const uint16_t SINE_VAL_AMP  = 1024;

// DDS Variables
volatile uint32_t phase_accumulator = 0;
uint32_t phase_increment = 0;
FspTimer timer;
uint16_t sine_table[SINE_TABLE_SIZE];

// --- State Management ---
enum DemoMode {
  MODE_BANDWIDTH = 0,
  MODE_PRECISION,
  MODE_INTERPOLATION
};

volatile DemoMode currentMode = MODE_BANDWIDTH;
volatile int currentSubState = 1; // 1, 2, or 3
volatile bool isSineWave = true;

// --- Timer Interrupt Service Routine (DDS Oscillator) ---
void timer_callback(timer_callback_args_t *args) {
  phase_accumulator += phase_increment;
  uint16_t out_val;

  if (isSineWave) {
    uint32_t index = phase_accumulator >> 24;
    out_val = sine_table[index];
  } else {
    out_val = (phase_accumulator & 0x80000000) ? WAVE_AMP_LOW : WAVE_AMP_HIGH;
  }
  *DAC_REG = out_val;
}

// --- Waveform Frequency Updater ---
void updateDDSFrequency(float targetFreq) {
  double timerFreq = 192000.0;
  phase_increment = (uint32_t)((targetFreq * 4294967296.0) / timerFreq);
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  analogWriteResolution(12);
  analogWrite(A0, 0); 

  delay(2000);
  Serial.println("\n=== Analog Mimic: Manual Decimation Test ===");
  if (!mimic.begin(&Wire)) {
    Serial.println("Error: Mimic Init Failed.");
    while(1);
  }

  for (int i = 0; i < SINE_TABLE_SIZE; i++) {
    float angle = 2.0 * PI * i / SINE_TABLE_SIZE;
    sine_table[i] = SINE_CENTER + (int)(SINE_VAL_AMP * sin(angle));
  }

  uint8_t timer_type = 0;
  int8_t timer_ch = FspTimer::get_available_timer(timer_type);
  timer.begin(TIMER_MODE_PERIODIC, timer_type, timer_ch, 192000.0, 50, timer_callback);
  timer.setup_overflow_irq();
  timer.open();
  timer.start();

  printMenu();
  applyState(currentMode, currentSubState);
}

void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    bool changed = false;

    // アルファベットでモード切り替え (大文字小文字対応)
    if (c == 'a' || c == 'A') { currentMode = MODE_BANDWIDTH; changed = true; }
    else if (c == 'b' || c == 'B') { currentMode = MODE_PRECISION; changed = true; }
    else if (c == 'c' || c == 'C') { currentMode = MODE_INTERPOLATION; changed = true; }
    
    // 数字でサブ状態切り替え
    else if (c >= '1' && c <= '3') {
      currentSubState = c - '0'; // 1, 2, or 3
      changed = true;
    }

    if (changed) {
      applyState(currentMode, currentSubState);
    }
  }
}

void printMenu() {
  Serial.println("\n--- Commands ---");
  Serial.println("[Modes]");
  Serial.println("  'A' : Bandwidth & Anti-Alias (10kHz Sine)");
  Serial.println("  'B' : Precision Break (100Hz Pulse -> 5Hz LPF)");
  Serial.println("  'C' : Q16 Interpolation (10Hz Sine)");
  Serial.println("[Parameters]");
  Serial.println("  '1' : Condition 1 (Native / Minimum Decimation)");
  Serial.println("  '2' : Condition 2 (Medium Decimation)");
  Serial.println("  '3' : Condition 3 (Maximum Decimation)");
  Serial.println("----------------");
}

void applyState(DemoMode mode, int subState) {
  Serial.print("\n>>> Setting Mode ");
  
  switch (mode) {
    // =================================================================
    // Mode A: Bandwidth & Anti-Alias
    // =================================================================
    case MODE_BANDWIDTH:
      Serial.print("'A' (Bandwidth) - Param: ");
      isSineWave = true;
      updateDDSFrequency(10000.0); // 10kHz

      if (subState == 1) {
        Serial.println("1 (N=0, 192kHz)");
        mimic.setDecimation(0);
      } else if (subState == 2) {
        Serial.println("2 (N=2, 48kHz)");
        mimic.setDecimation(2);
      } else {
        Serial.println("3 (N=4, 12kHz)");
        mimic.setDecimation(4);
      }
      mimic.setBypassDSP();
      break;

    // =================================================================
    // Mode B: Precision Break (Biquad limits)
    // =================================================================
    case MODE_PRECISION:
      Serial.print("'B' (Precision) - Param: ");
      isSineWave = false; // 矩形波
      updateDDSFrequency(10.0); // ★100Hz から 1Hz のステップ信号に変更

      if (subState == 1) {
        Serial.println("1 (N=0, 192kHz) -> Expect: Math overflow, noisy/broken step response");
        mimic.setDecimation(0);
      } else if (subState == 2) {
        Serial.println("2 (N=5, 6kHz) -> Expect: Partially broken");
        mimic.setDecimation(5);
      } else {
        Serial.println("3 (N=10, 187.5Hz) -> Expect: Perfect analog-like RC charge curve");
        mimic.setDecimation(10);
      }
      mimic.setFilterBiquadLPF(5.0, MIMIC_FILTER_Q_BUTTERWORTH);
      break;

    // =================================================================
    // Mode C: Q16 Linear Interpolation
    // =================================================================
    case MODE_INTERPOLATION:
      Serial.print("'C' (Interpolation) - Param: ");
      isSineWave = true;
      updateDDSFrequency(10.0); // 10Hz LFO

      if (subState == 1) {
        Serial.println("1 (N=0, 192kHz Native) -> Perfect curve");
        mimic.setDecimation(0);
      } else if (subState == 2) {
        Serial.println("2 (N=6, 3kHz) -> Still smooth thanks to Q16");
        mimic.setDecimation(6);
      } else {
        Serial.println("3 (N=10, 187.5Hz) -> Ultimate test! Watch the Q16 slope work");
        mimic.setDecimation(10);
      }
      mimic.setBypassDSP();
      break;
  }
}
