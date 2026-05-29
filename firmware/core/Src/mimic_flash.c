#include "mimic_flash.h"
#include <string.h>
#include "py32f0xx_hal.h"
#include "mimic_registers.h"

#define MIMIC_FLASH_DEFAULT_VALUE_TWO_BYTES 0xFFFF

#define MIMIC_FLASH_CALIB_PAGE_ADDR  ((FLASH_END) - (FLASH_PAGE_SIZE) + 1)

// 必ず偶数アドレスに配置すること
#define MIMIC_FLASH_GAIN_Q15_ADDR ((MIMIC_REG_NVM_GAIN_Q15) - (MIMIC_REG_NVM_START))
#define MIMIC_FLASH_OFFSET_ADDR ((MIMIC_REG_NVM_OFFSET) - (MIMIC_REG_NVM_START))

/**
 * @brief  [内部実装] 書き込み準備：指定アドレスのフラッシュ1ページ分をRAMバッファにコピーする
 */
static void ReadPage(uint32_t addr, uint8_t *ram_page_buffer)
{
  /* フラッシュから直接RAMバッファへ128バイト丸ごとコピー */
  memcpy(ram_page_buffer, (const uint8_t *)addr, FLASH_PAGE_SIZE);
}

/**
 * @brief  [内部実装] 書き込み完了：割り込みを禁止し、ページ消去とRAMバッファの一括書き込みを行う
 */
static bool WritePage(uint32_t addr, const uint8_t *ram_page_buffer)
{
  HAL_StatusTypeDef status = HAL_OK;
  uint32_t PAGEError = 0;
  FLASH_EraseInitTypeDef EraseInitStruct = {0};

  /* 消去設定のビルド */
  EraseInitStruct.TypeErase   = FLASH_TYPEERASE_PAGEERASE;        /* ページ消去を選択 [cite: 104] */
  EraseInitStruct.PageAddress = addr;                /* 対象アドレス [cite: 104] */
  EraseInitStruct.NbPages     = 1;                                /* 1ページのみ消去 [cite: 104] */

  /* ================== クリティカルセクション開始 ================== */
  uint32_t __primask_save = __get_PRIMASK();
  __disable_irq(); /* 割り込みを禁止 */

  /* 1. フラッシュのロック解除 */
  HAL_FLASH_Unlock();

  /* 2. 対象ページの消去を実行 */
  if (HAL_FLASHEx_Erase(&EraseInitStruct, &PAGEError) == HAL_OK)
  {
    /* 3. 消去が成功したら、RAMで更新が終わった128バイトデータを一括書き込み */
    /* PY32F071の仕様により FLASH_TYPEPROGRAM_PAGE（128バイト）固定 */
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_PAGE, addr, (uint32_t *)ram_page_buffer);
  }
  else
  {
    status = HAL_ERROR;
  }

  /* 4. フラッシュを再ロック */
  HAL_FLASH_Lock();

  __set_PRIMASK(__primask_save);
  /* ================== クリティカルセクション終了 ================== */

  return (status == HAL_OK);
}

/**
 * @brief キャリブレーションデータをフラッシュに書き込む共通実装関数 (内部関数)
 * * @param page_offset ページ先頭からのオフセットバイト数 (MIMIC_FLASH_GAIN_ADDR など)
 * @param data        書き込みたいデータへのポインタ
 * @param size        データのバイトサイズ (sizeof(uint16_t) など)
 * @return true       書き込み成功
 * @return false      書き込み失敗
 */
static bool WriteCalibrationData(uint32_t page_offset, const void *data, size_t size) {
    /* 修正：32bit型で配列を宣言し、要素数は 1/4 にする */
    uint32_t ram_page_buffer[FLASH_PAGE_SIZE / 4];
    uint32_t addr = MIMIC_FLASH_CALIB_PAGE_ADDR;

    /* 1. 現在のフラッシュ内容をRAMバッファに直接コピー */
    ReadPage(addr, (uint8_t *)ram_page_buffer);

    /* 2. 引数で指定された位置に、指定サイズ分のデータを直接上書き */
    // 安全のため、バッファオーバーラン防止のガードを入れておくと完璧です
    if (page_offset + size <= FLASH_PAGE_SIZE) {
        uint8_t *byte_ptr = (uint8_t *)ram_page_buffer;
        memcpy(&byte_ptr[page_offset], data, size);
    } else {
        return false; // ページサイズをはみ出す場合は書き込まずにエラー
    }

    /* 3. 割り込みを禁止し、消去からフラッシュ書き込みまでを一気に行う */
    return WritePage(addr, (const uint8_t *)ram_page_buffer);
}

bool MimicFlash_WriteGainQ15Data(uint16_t gain) {
    return WriteCalibrationData(MIMIC_FLASH_GAIN_Q15_ADDR, &gain, sizeof(gain));
}

uint16_t MimicFlash_ReadGainQ15Data(void) {
  /* 改善: Flash領域からポインタ経由で直接値を読み出す */
  uint16_t gain_q15 = *((uint16_t *)(MIMIC_FLASH_CALIB_PAGE_ADDR + MIMIC_FLASH_GAIN_Q15_ADDR));
  if (gain_q15 == MIMIC_FLASH_DEFAULT_VALUE_TWO_BYTES) {
    gain_q15 = 1 << 15;
  }
  return gain_q15;
}

bool MimicFlash_WriteOffsetData(int16_t offset) {
    return WriteCalibrationData(MIMIC_FLASH_OFFSET_ADDR, &offset, sizeof(offset));
}

int16_t MimicFlash_ReadOffsetData(void) {
  /* 改善: Flash領域からポインタ経由で直接値を読み出す */
  uint16_t raw = *((uint16_t *)(MIMIC_FLASH_CALIB_PAGE_ADDR + MIMIC_FLASH_OFFSET_ADDR));
  if (raw == MIMIC_FLASH_DEFAULT_VALUE_TWO_BYTES) {
    raw = 0;
  }
  return (int16_t)raw;
}
