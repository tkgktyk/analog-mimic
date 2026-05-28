/**
 ******************************************************************************
 * @file    mimic_flash.h
 * @author  TAKAGI Katsuyuki
 * @brief   
 *-----------------------------------------------------------------------------
 * Copyright (C) 2026 TAKAGI Katsuyuki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 ******************************************************************************
 */

#ifndef MIMIC_FLASH_H
#define MIMIC_FLASH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool MimicFlash_WriteGainQ15Data(uint16_t gain_q15);

uint16_t MimicFlash_ReadGainQ15Data(void);

/**
 * @brief  校正値を擬似EEPROM（フラッシュの1ページ）に保存するメインAPI
 * @param  target_flash_addr: 保存先フラッシュの先頭アドレス（128バイトのアライメントが必要）
 * @param  my_cal_data: 保存したい校正値データへのポインタ
 * @param  data_size: 校正値データのサイズ（最大128バイト）
 * @retval HAL_StatusTypeDef: 手順の成否
 */
bool MimicFlash_WriteOffsetData(int16_t offset);

int16_t MimicFlash_ReadOffsetData(void);

#ifdef __cplusplus
}
#endif

#endif // MIMIC_FLASH_H
