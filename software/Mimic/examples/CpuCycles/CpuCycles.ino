#include <Wire.h>
#include <Mimic.h>

// --- Constants ---
const uint8_t MIMIC_COMM_ERROR = 0xFF;

// Create an instance of Mimic0x (5V Operation)
Mimic0x mimic(MIMIC_VDD_5V); 

// =========================================================
// Wrapper Functions for Test Execution
// (Compliant with the latest refined API specifications)
// =========================================================

// --- 0. Bypass & Common ---
void testBypassDSP()      { mimic.setBypassDSP(); }
void testDelay()          { mimic.setDigitalDelay(50); } // 50 samples delay
void testOutputOffset()   { mimic.setBypassDSP(); mimic.setOutputOffset(1000); } // Shift +1000mV

// --- 1. Filtering (DC shift arguments are completely isolated) ---
void testFilter1stLPF()   { mimic.setFilter1stLPF(1000.0f); }
void testFilter1stHPF()   { mimic.setFilter1stHPF(1000.0f); }
void testFilterBqLPF()    { mimic.setFilterBiquadLPF(1000.0f); }
void testFilterBqHPF()    { mimic.setFilterBiquadHPF(1000.0f); }
void testFilterBqBPF()    { mimic.setFilterBiquadBPF(1000.0f); }
void testFilterBqNotch()  { mimic.setFilterBiquadNotch(1000.0f); }

// --- 2. PGA ---
void testPGA()            { mimic.setPGA(2.0f, 2500); } // Gain x2, reference point 2.5V

// --- 3. Nonlinear ---
void testNonlinearLog()   { mimic.setNonlinearLog(); }      // No arguments (LUT specified only)
void testNonlinearAlog()  { mimic.setNonlinearAntilog(); }  // No arguments (LUT specified only)

// --- 4. Comparators ---
void testCompWindow()     { mimic.setComparatorWindow(3000, 2000); } // 3V and 2V window
void testCompSchmitt()    { mimic.setComparatorSchmitt(3000, 2000); }

// --- 5. Dynamics & Waveform ---
void testPeakHold()       { mimic.setPeakHold(0); } // Positive polarity
void testEnvelope()       { mimic.setEnvelopeFollower(50.0f, 0); } // 50ms decay, positive polarity
void testClipper()        { mimic.setClipper(4000, 1000); } // Hard/Soft integrated (Upper 4V, Lower 1V)

// --- 6. Math & Special (Digital Functions) ---
void testMathDeriv()      { mimic.setMathDerivative(1.0f); }
void testMathIntegral()   { mimic.setMathIntegral(1.0f); }
void testRectHalf()       { mimic.setRectifierHalf(true, 2500); }
void testRectFull()       { mimic.setRectifierFull(2500); }
void testSlewRate()       { mimic.setSlewRateLimiter(0.1f); } // 10% change per sample
void testSampleHold()     { mimic.setSampleAndHold(10.0f); } // Timebase: 10ms period S&H
void testTrackHold()      { mimic.setTrackAndHold(10.0f, 0.5f); } // Timebase: 10ms period, 50% Track

// =========================================================
// Test Scenario Definition (Array of Function Pointers)
// =========================================================
struct ModeTest {
  const char* modeName;
  void (*setModeFunc)();
};

const ModeTest testModes[] = {
  {"BYPASS_DSP",         testBypassDSP},
  {"DELAY",              testDelay},
  {"OUTPUT_OFFSET",      testOutputOffset},
  {"FILTER_1ST_LPF",     testFilter1stLPF},
  {"FILTER_1ST_HPF",     testFilter1stHPF},
  {"FILTER_BIQUAD_LPF",  testFilterBqLPF},
  {"FILTER_BIQUAD_HPF",  testFilterBqHPF},
  {"FILTER_BIQUAD_BPF",  testFilterBqBPF},
  {"FILTER_BIQUAD_NOTCH",testFilterBqNotch},
  {"PGA",                testPGA},
  {"NONLINEAR_LOG",      testNonlinearLog},
  {"NONLINEAR_ANTILOG",  testNonlinearAlog},
  {"COMPARATOR_WIN",     testCompWindow},
  {"COMPARATOR_SCHMITT", testCompSchmitt},
  {"WAVE_PEAK_HOLD",     testPeakHold},
  {"WAVE_ENVELOPE",      testEnvelope},
  {"CLIPPER",            testClipper},
  {"MATH_DERIVATIVE",    testMathDeriv},
  {"MATH_INTEGRAL",      testMathIntegral},
  {"RECTIFIER_HALF",     testRectHalf},
  {"RECTIFIER_FULL",     testRectFull},
  {"SLEW_RATE_LIMITER",  testSlewRate},
  {"SAMPLE_AND_HOLD",    testSampleHold},
  {"TRACK_AND_HOLD",     testTrackHold}
};
const size_t NUM_MODES = sizeof(testModes) / sizeof(testModes[0]);

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Wait briefly for Serial Monitor to open
  delay(2000);
  Serial.println("\n=== Analog Mimic CPU Cycle Profiler ===");
  Serial.println("Running on fully refined DSP Architecture v2.0");

  // Initialize the library
  if (!mimic.begin(&Wire)) {
    Serial.println("Error: Could not initialize Mimic Library.");
    while (1);
  }
  Serial.println("Mimic Library Initialized.");
  
  // Read status via the library
  uint8_t status = mimic.getStatus();
  if (status == MIMIC_COMM_ERROR) {
    Serial.println("Mimic: Communication Error.");
    while (1);
  }

  Serial.print("Mimic Status: 0x");
  Serial.println(status, HEX);
  
  if (status & MIMIC_STATUS_SYSTEM_READY) {
    Serial.println("Mimic: System Ready!");
  }

  // Read initial CPU cycles immediately after boot (Bypass mode) to reset peak
  uint16_t initialCycles = mimic.getCpuCycles();
  Serial.print("Initial Boot-up Max Cycles: ");
  Serial.println(initialCycles);
  Serial.println("---------------------------------------");

  mimic.setOutputOpen(false);
}

void loop() {
  for (size_t i = 0; i < NUM_MODES; i++) {
    Serial.print("Testing Mode: ");
    Serial.println(testModes[i].modeName);

    // 1. Change mode and parameters (Triggers I2C communication)
    testModes[i].setModeFunc();
    delay(100);

    uint8_t status = mimic.getStatus(); 
    Serial.print("Mimic Status: 0x");
    Serial.println(status, HEX);

    // Read status flags from registration mode select
    uint8_t flags = mimic.getValue(MIMIC_REG_MODE_SELECT); 
    Serial.print("Mimic MODE: 0x");
    Serial.println(flags, HEX);

    // 2. Read CPU cycles immediately after mode transition
    // Measures how high the maximum cycle is pushed by I2C interrupt blocking
    uint16_t transitionCycles = mimic.getCpuCycles();

    // 3. Wait 1 second for the DSP to stabilize in the new mode
    // During this 1 second (~100,000 ADC interrupts), the pure peak load of the new mode is measured
    delay(1000);

    // 4. Read CPU cycles after 1 second (Pure steady-state load of the new mode)
    uint16_t steadyCycles = mimic.getCpuCycles();

    // Display Results
    Serial.print("  -> Transition/I2C Max Cycles: ");
    Serial.println(transitionCycles);
    Serial.print("  -> Steady State Max Cycles  : ");
    Serial.print(steadyCycles);

    Serial.println("---------------------------------------");

    // Reset common configurations (Delay/Offset) before the next test scenario
    mimic.setOutputOffset(0);
    delay(100);
  }

  // Complete profiling loop execution
  Serial.println("\n=== Profiling Complete ===");
  while (1); 
}
