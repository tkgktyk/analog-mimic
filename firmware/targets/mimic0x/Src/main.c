/**
 ******************************************************************************
 * @file    main.c
 * @author  TAKAGI Katsuyuki
 * @brief   Main Application Entry Point, Core System Lifecycle Management, 
 *          and Background Non-Deterministic Parameter Processing Loop.
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

#include "py32f0xx_hal.h"
#include "mimic_device.h"

// =========================================================
// Main Routine
// =========================================================
int main(void)
{
    // 1. Initialize MCU Clocks, RAM Vector Table, and Hardware Peripherals
    Mimic_HW_Init();

    // 2. Initialize Analog Mimic Core Logic (I2C, DSP, Calibration, and initial Engine Start)
    MimicDevice_Init();

    // 3. Signal System Ready State
    MimicDevice_SetStatusFlag(MIMIC_STATUS_SYSTEM_READY);
    Mimic_SetReadyLED(true);

    // 4. Main Background Loop
    while (1) {
        MimicDevice_ProcessPendingTasks();

        // Execute WFI (Wait For Interrupt).
        // Sampling interrupts jump directly to the deterministic ISR via SRAM VTOR.
        __WFI();
    }
}

#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Mimic_HW_ErrorTrap();
}
#endif /* USE_FULL_ASSERT */

/**
 * @brief Override standard library initialization hooks to prevent 
 *        the inclusion of malloc, reent, and file system overloads.
 *        This saves substantial FLASH and reclaims Heap RAM.
 */
void __libc_init_array(void)
{
}
