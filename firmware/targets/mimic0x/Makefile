# Analog Mimic - mimic0x Firmware Makefile

# ==============================================================================
# 1. ハードウェアIDの定義 (ここを変更するだけで全てが連動します)
# ==============================================================================
# 2桁の16進数文字列 (00, 01, 0A, 10 等)
HW_ID ?= 01

# ==============================================================================
# 2. ビルドターゲット（出力ファイル名）の生成
# ==============================================================================
# ボード名: "MIMIC" + "01" -> MIMIC01
BOARD_NAME = MIMIC$(HW_ID)

# 出力ファイル名: analog_mimic_MIMIC01 (.elf, .hex などに使用)
TARGET = analog_mimic_$(BOARD_NAME)

# ==============================================================================
# 3. C言語（ファームウェア）へ引き渡すマクロ値の生成
# ==============================================================================
# ハードウェアバージョン (16進数): "0x" + "01" -> 0x01
HW_VERSION_HEX = 0x$(HW_ID)

# ファームウェアバージョン (10進数): Gitのコミット回数を0-255に丸める
FW_VERSION_DEC = $(shell expr `git rev-list --count HEAD` % 256)

# ========================================================
# Toolchain Configuration
# ========================================================
# By default, it assumes the ARM GCC toolchain is in your system PATH.
# To use a specific path, run: make GCC_PATH="/path/to/bin/"
GCC_PATH ?= 
ifdef GCC_PATH
  # Ensure trailing slash if GCC_PATH is provided
  PREFIX = $(GCC_PATH)/arm-none-eabi-
else
  PREFIX = arm-none-eabi-
endif

CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size

# Build directory
BUILD_DIR = build/$(HW_REV)

# ========================================================
# Library and Framework Paths (Adjust to your environment)
# ========================================================
CORE_DIR       = ../../core
ARDUINO_DIR       = ../../../software/Mimic
HAL_DRIVER_DIR = ../../lib/PY32F071_Firmware/Drivers/PY32F071_HAL_Driver
CMSIS_DIR      = ../../lib/PY32F071_Firmware/Drivers/CMSIS

# ========================================================
# Source Files
# ========================================================
C_SOURCES =  \
./Src/main.c \
./Src/mimic0x_hal_msp.c \
./Src/py32f071_it.c \
./Src/system_py32f071.c \
$(CORE_DIR)/Src/mimic_device.c \
$(CORE_DIR)/Src/mimic_dsp.c \
$(CORE_DIR)/Src/mimic_flash.c \
$(CORE_DIR)/Src/mimic_i2c.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_rcc.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_rcc_ex.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_cortex.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_gpio.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_adc.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_adc_ex.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_dac.c \
$(HAL_DRIVER_DIR)/Src/py32f071_ll_i2c.c \
$(HAL_DRIVER_DIR)/Src/py32f071_ll_rcc.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_tim.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_opa.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_tim_ex.c \
$(HAL_DRIVER_DIR)/Src/py32f071_hal_flash.c

# Startup routine
ASM_SOURCES =  \
startup_py32f071xx.s

# ========================================================
# Include Paths
# ========================================================
C_INCLUDES =  \
-I./Inc \
-I$(CORE_DIR)/Inc \
-I$(ARDUINO_DIR)/src \
-I$(HAL_DRIVER_DIR)/Inc \
-I$(CMSIS_DIR)/Include \
-I$(CMSIS_DIR)/Device/PY32F071/Include

# ========================================================
# Compile Definitions and Flags
# ========================================================
# C preprocessor macro definitions
C_DEFS =  \
-DUSE_HAL_DRIVER \
-DUSE_FULL_LL_DRIVER \
-DPY32F071xB \
-DTARGET_$(BOARD_NAME) \
-DMIMIC_HW_VERSION_VALUE=$(HW_VERSION_HEX) \
-DMIMIC_FW_VERSION_VALUE=$(FW_VERSION_DEC) \
-DARM_MATH_CM0PLUS

MCU = -mcpu=cortex-m0plus -mthumb

# Compilation flags
# [Change] Added -flto for Link Time Optimization
CFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) -O2 -Wall -fdata-sections -ffunction-sections -flto -MMD -MP

# Linker settings
LDSCRIPT = py32f071xb.ld
LIBS = -lc -lm -lnosys
# [Change] Added -flto for the linker to perform cross-file optimization
LDFLAGS = $(MCU) -specs=nano.specs -flto -T$(LDSCRIPT) $(LIBS) -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref

# ========================================================
# Build Rules
# ========================================================
.PHONY: all clean release

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

release:
	$(MAKE) clean
	$(MAKE) HW_REV=MIMIC00
	$(MAKE) HW_REV=MIMIC01

# Object generation
C_OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

ASM_OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR) 
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O ihex $< $@
	
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O binary -S $< $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	-rm -fR build

-include $(wildcard $(BUILD_DIR)/*.d)
