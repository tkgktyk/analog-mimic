#include <Wire.h>
#include <Mimic.h>

// --- Hardware Pin Assign ---
const int ADC_PIN = A1;  // MimicのDAC出力をこのピンに入力する

// --- Test Target DAC Codes (12-bit Resolution: 0 - 4095) ---
const uint16_t testCodes[3] = {409, 2048, 3686}; 

// --- Statistical Configuration ---
#define NUM_SAMPLES 4096 // 安定化のための平均化サンプル数（2^N通りで高速化）

Mimic0x mimic(MIMIC_VDD_5V);

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Arduino側の解像度設定
  analogWriteResolution(12); // DACを12bitに設定 (A0)
  analogReadResolution(12);  // ADCを12bitに設定 (A1)

  delay(2000);
  Serial.println("\n==========================================");
  Serial.println("  Analog Mimic Hardware Calibration Tool  ");
  Serial.println("==========================================");

  // Analog Mimicの初期化とBypassモード設定
  if (!mimic.begin(&Wire)) {
    Serial.println("Error: Mimic Init Failed.");
    while(1);
  }
  mimic.setBypassDSP();
  Serial.println("-> Analog Mimic set to BYPASS mode.");
  Serial.println("-> Please connect Arduino A0 (DAC) to Mimic ADC_IN");
  Serial.println("   and Mimic DAC_OUT to Arduino A1 (ADC).\n");
  
  delay(3000); // 接続安定待ち
}

void loop() {
  Serial.println("\n[Ready] Press Enter to START calibration...");
  Serial.println("---------------------------------------------------------");

  waitForEnter();
  runCalibration();
  commitCalibration();
}

/**
 * @brief Enterキー（改行コード '\n'）が押されるまで待機する関数
 */
void waitForEnter() {
  // 入力があるまで完全にブロックして待つ
  while (true) {
    if (Serial.available() > 0) {
      char c = Serial.read();

      // Enter（改行）を検知したら待機ループを抜ける
      if (c == '\n') {
        // 念のためバッファに残った余分な文字（'\r'など）をフラッシュ
        while (Serial.available() > 0) {
          Serial.read();
          delay(2);
        }
        break; 
      }
    }
    // マイコンがハングアップしないように極小のウェイトを入れるのが組み込みの定石
    delay(1); 
  }
}

void commitCalibration() {
  // 既存の補正値を取得（Q15から通常のfloat倍率に戻しておく）
  // ※getTwoByte から getTwoBytes に関数名を合わせました
  int gain_q15 = mimic.getTwoBytes(MIMIC_REG_NVM_GAIN_Q15);
  int current_offset = mimic.getTwoBytes(MIMIC_REG_NVM_OFFSET);
  
  float current_gain = (float)gain_q15 / 32768.0f;
  
  Serial.print("Current Gain (Float): "); Serial.print(gain_q15); Serial.print("("); Serial.print(current_gain, 5); Serial.println(")");
  Serial.print("Current Offset (LSB): "); Serial.println(current_offset);

  Serial.println("\nCommit current calibration data [y/N]");
  Serial.println("---------------------------------------------------------");

  // --- ユーザーからのキー入力を待機 ---
  char inputChar = '\0';
  
  while (true) {
    if (Serial.available() > 0) {
      char c = Serial.read();

      // Enter（改行コード）が押されたら入力を評価してループを抜ける
      if (c == '\n') {
        // 余分な改行文字（'\r'など）をフラッシュ
        while (Serial.available() > 0) {
          Serial.read();
          delay(2);
        }
        break; 
      } else if (c != '\r') {
        // 改行以外の文字（ユーザーが打った最初の1文字）を記録
        if (inputChar == '\0') {
          inputChar = c; 
        }
      }
    }
    delay(1);
  }

  // --- 入力された文字に応じた分岐処理 ---
  if (inputChar == 'y' || inputChar == 'Y') {
    Serial.println("\n[Writing] Saving final calibration parameters permanently to NVM...");
    
    // I2C経由でマイコンにフラッシュ保存を指示
    mimic.commitCalibrationToNvm();
    mimic.reloadCalibrationFromNvm();
    Serial.println(">> SUCCESS: Calibration permanently fixed to Flash memory!");
  } else {
    // 'n' や 'N'、あるいは何も入れずにEnterを押した場合はスキップ
    Serial.println("\n>> Skipped: Calibration data was NOT saved to NVM.");
  }
  Serial.println("==========================================");
}

void runCalibration() {
  Serial.println("\n--- Starting High-Precision Measurement (Gain & Offset) ---");

  // 既存の補正値を取得（Q15から通常のfloat倍率に戻しておく）
  int gain_q15 = mimic.getTwoBytes(MIMIC_REG_NVM_GAIN_Q15);
  int current_offset = mimic.getTwoBytes(MIMIC_REG_NVM_OFFSET);
  
  float current_gain = (float)gain_q15 / 32768.0f;
  
  Serial.print("Current Gain (Float): "); Serial.print(gain_q15); Serial.print("("); Serial.print(current_gain, 5); Serial.println(")");
  Serial.print("Current Offset (LSB): "); Serial.println(current_offset);

  float sumX  = 0.0;
  float sumY  = 0.0;
  float sumXY = 0.0;
  float sumX2 = 0.0;

  for (int i = 0; i < 3; i++) {
    uint16_t targetCode = testCodes[i];
    analogWrite(A0, targetCode);
    delay(500); 

    uint64_t sum = 0;
    for (int s = 0; s < NUM_SAMPLES; s++) {
      sum += analogRead(ADC_PIN);
      delayMicroseconds(10);
    }
    
    float measuredValue = (float)sum / (float)NUM_SAMPLES;
    
    float x = (float)targetCode;
    float y = measuredValue;
    
    sumX  += x;
    sumY  += y;
    sumXY += x * y;
    sumX2 += x * x;

    Serial.print("Point ["); Serial.print(i + 1); Serial.print("] Target (X): "); Serial.print(targetCode);
    Serial.print("\t-> Measured (Y): "); Serial.println(measuredValue, 2);

    if (i < 2) {
      Serial.println("Press Enter to NEXT code...");
      waitForEnter();
    }
  }

  float n = 3.0;
  float denominator = (n * sumX2) - (sumX * sumX);
  
  if (abs(denominator) < 1e-5) {
    Serial.println("Error: Calibration math error (Denominator is zero).");
    return;
  }

  float a_slope  = ((n * sumXY) - (sumX * sumY)) / denominator; 
  float b_intercept = ((sumX2 * sumY) - (sumXY * sumX)) / denominator; 

  // =========================================================================
  // 4. 【改善】既存の補正値をベースに、今回の測定結果を合成する
  // =========================================================================
  
  // ① 新しいゲインの算出 (現在のゲイン × 今回必要な補正倍率)
  float finalGainCalibration = current_gain * (1.0f / a_slope);
  
  // ② 新しいオフセットの算出 (現在のオフセット ＋ 今回発生した切片ズレの逆相殺分)
  // 傾き(a_slope)による影響をキャンセルするために a_slope で割ります
  float delta_offset = -b_intercept / a_slope;
  int16_t finalOffsetCalibration = (int16_t)round((float)current_offset + delta_offset);

  Serial.println("\n==========================================");
  Serial.println("           CALIBRATION RESULTS            ");
  Serial.println("==========================================");
  Serial.print("Current Transfer Function: y = "); 
  Serial.print(a_slope, 5); Serial.print(" * x "); 
  if (b_intercept >= 0) Serial.print("+ ");
  Serial.println(b_intercept, 2);
  Serial.println("------------------------------------------");

  Serial.print("NEW Combined Gain   (float):   ");
  Serial.println(finalGainCalibration, 5);
  Serial.print("NEW Combined Offset (int16_t): ");
  Serial.print(finalOffsetCalibration); Serial.println(" LSB");
  
  Serial.println("\n--- How to Apply to your I2C Transmitter ---");
  Serial.print("  writeNvmGainCal("); Serial.print(finalGainCalibration, 5); Serial.println(");");
  Serial.print("  writeNvmOffsetCal("); Serial.print(finalOffsetCalibration); Serial.println(");");
  Serial.println("==========================================");

  // 新しい合成値をMimicに書き込み（上書き・更新）
  mimic.setGainCal(finalGainCalibration);
  mimic.setOffsetCal(finalOffsetCalibration);
}
