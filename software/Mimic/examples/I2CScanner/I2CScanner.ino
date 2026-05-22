#include <Wire.h>
#include "Mimic.h"

// --- Constants ---
const unsigned long SERIAL_BOOT_DELAY_MS = 3000;
const unsigned long POST_SCAN_DELAY_MS = 5000;

const uint8_t I2C_SCAN_START_ADDR = 1;
const uint8_t I2C_SCAN_END_ADDR = 127;

const int REQUEST_DATA_BYTES = 1;
const uint8_t MATCH_COUNT_IDEAL = 1;
const uint8_t MATCH_COUNT_FAULT_THRESHOLD = 100;

void setup() {
  Wire.begin();
  Serial.begin(115200);
  Serial.println("\nI2C Scanner");

  delay(SERIAL_BOOT_DELAY_MS);
  
  int deviceCount = 0;
  for (uint8_t address = I2C_SCAN_START_ADDR; address < I2C_SCAN_END_ADDR; address++) {
    Wire.beginTransmission(address);
    Serial.print("Scanning 0x"); 
    Serial.println(address, HEX);
    
    uint8_t error = Wire.endTransmission();
    Serial.print("Error code: "); 
    Serial.println(error);
    
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      Serial.println(address, HEX);
      deviceCount++;
    }
  }
  
  if (deviceCount == 0) {
    Serial.println("No I2C devices found\n");
  }
  
  // Wait after scan completes
  Serial.println("\nWaiting 5 seconds...");
  delay(POST_SCAN_DELAY_MS);

  // Read match count from PY32 register
  Serial.println("\n--- Reading Match Count from PY32 ---");
  
  // 1. Send the target register address
  Wire.beginTransmission(MIMIC_DEFAULT_I2C_ADDR);
  Wire.write(MIMIC_REG_CPU_CYCLES_L);
  uint8_t txError = Wire.endTransmission();
  
  // Guard clause: Handle transmission error immediately
  if (txError != 0) {
    Serial.print("=> Error: Could not connect to PY32 (0x40). Error code: ");
    Serial.println(txError);
    return;
  }

  // 2. Request data from target device
  Wire.requestFrom(MIMIC_DEFAULT_I2C_ADDR, REQUEST_DATA_BYTES);
  
  // Guard clause: Handle receive error immediately
  if (!Wire.available()) {
    Serial.println("=> Error: No data received from PY32.");
    return;
  }

  // Process received data
  uint8_t matchCount = Wire.read();
  Serial.print("=> debug_addr_match_count: ");
  Serial.println(matchCount);

  // Support for evaluating the result
  if (matchCount == MATCH_COUNT_IDEAL) {
    Serial.println("[RESULT] PY32 is perfectly fine! (Arduino UNO R4 Ghost Address Bug)");
  } else if (matchCount >= MATCH_COUNT_FAULT_THRESHOLD) {
    Serial.println("[RESULT] PY32 hardware is reacting to ALL addresses.");
  }
}

void loop() {
}
