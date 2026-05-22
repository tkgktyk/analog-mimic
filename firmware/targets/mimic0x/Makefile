# Analog Mimic - mimic0x Firmware Makefile

TARGET_BASE = analog_mimic
HW_REV ?= MIMIC00
# shellコマンド(sed)を使って、HW_REVの「末尾1文字」を「X」に置換し、シリーズ名を自動生成する
# 例: MIMIC10 -> MIMIC1X, MIMIC01 -> MIMIC0X
SERIES = $(shell echo $(HW_REV) | sed 's/.$$/X/')
TARGET = $(TARGET_BASE)_$(HW_REV)

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
HAL_DRIVER_DIR = ../../lib/PY32F071_Firmware/Drivers/PY32F071_HAL_Driver
CMSIS_DIR      = ../../lib/PY32F071_Firmware/Drivers/CMSIS
DSP_FILTER_DIR = ../../lib/PY32F071_Firmware/Drivers/CMSIS/DSP_Lib/Source/FilteringFunctions

# ========================================================
# Source Files
# ========================================================
C_SOURCES =  \
./Src/main.c \
./Src/mimic0x_hal_msp.c \
./Src/py32f071_it.c \
./Src/system_py32f071.c \
$(CORE_DIR)/Src/mimic_dsp.c \
$(CORE_DIR)/Src/mimic_i2c.c \
$(CORE_DIR)/Src/mimic_device.c \
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
$(HAL_DRIVER_DIR)/Src/py32f071_hal_tim_ex.c

# Startup routine
ASM_SOURCES =  \
startup_py32f071xx.s

# ========================================================
# Include Paths
# ========================================================
C_INCLUDES =  \
-I./Inc \
-I$(CORE_DIR)/Inc \
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
-DHW_REV_$(HW_REV) \
-DSERIES_$(SERIES) \
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
