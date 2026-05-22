#include <Wire.h>
#include <Mimic.h>

// Communication error status definition
const uint8_t MIMIC_COMM_ERROR = 0xFF;

// Create an instance of Mimic0x
Mimic0x mimic(MIMIC_VDD_5V); 

void setup() {
  Serial.begin(115200);
  
  // Initialize I2C communication
  Wire.begin();

  // Initialize the library
  if (mimic.begin(&Wire)) {
    Serial.println("Mimic Library Initialized.");
  }
  
  // Set to bypass mode as an initial configuration example
  mimic.setBypassDSP();
}

void loop() {
  // Read status via the library
  uint8_t status = mimic.getStatus();

  // Guard clause: Handle communication error immediately
  if (status == MIMIC_COMM_ERROR) {
    Serial.println("Mimic: Communication Error.");
    delay(1000);
    return;
  }

  // Process successful communication
  Serial.print("Mimic Status: 0x");
  Serial.println(status, HEX);

  if (status & MIMIC_STATUS_SYSTEM_READY) {
    Serial.println("Mimic: System Ready!");
  }

  delay(1000);
}
