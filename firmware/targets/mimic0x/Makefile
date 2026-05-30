# Analog Mimic - mimic0x Firmware Makefile

# ==============================================================================
# 1. Hardware ID Definition (Modify this to update all related targets)
# ==============================================================================
# 2-digit hexadecimal string (e.g., 00, 01, 0A, 10)
HW_ID ?= 01

# ==============================================================================
# 2. Build Target Generation
# ==============================================================================
# Board name: "Mimic" + "01" -> Mimic01
BOARD_NAME = MIMIC$(HW_ID)

# Output filename prefix: analog_mimic_MIMIC01 (Used for .elf, .hex, etc.)
TARGET = analog_mimic_$(BOARD_NAME)

# ==============================================================================
# 3. Preprocessor Macro Values for Firmware
# ==============================================================================
# Hardware version (Hexadecimal): "0x" + "01" -> 0x01
HW_VERSION_HEX = 0x$(HW_ID)

# Firmware version (Decimal): Commit count modulo 256
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
BUILD_DIR = build/$(BOARD_NAME)

# ========================================================
# Library and Framework Paths (Adjust to your environment)
# ========================================================
CORE_DIR       = ../../core
ARDUINO_DIR    = ../../../../analog-mimic-arduino
HAL_DRIVER_DIR = ../../lib/PY32F071_Firmware/Drivers/PY32F071_HAL_Driver
CMSIS_DIR      = ../../lib/PY32F071_Firmware/Drivers/CMSIS

# ========================================================
# Source Files
# ========================================================
C_SOURCES = \
./Src/main.c \
./Src/mimic0x_msp.c \
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
ASM_SOURCES = \
startup_py32f071xx.s

# ========================================================
# Include Paths
# ========================================================
C_INCLUDES = \
-I./Inc \
-I$(CORE_DIR)/Inc \
-I$(HAL_DRIVER_DIR)/Inc \
-I$(CMSIS_DIR)/Include \
-I$(CMSIS_DIR)/Device/PY32F071/Include

# ========================================================
# Compile Definitions and Flags
# ========================================================
# C preprocessor macro definitions
C_DEFS = \
-DUSE_HAL_DRIVER \
-DUSE_FULL_LL_DRIVER \
-DPY32F071xB \
-DTARGET_$(BOARD_NAME) \
-DMIMIC_HW_VERSION_VALUE=$(HW_VERSION_HEX) \
-DMIMIC_FW_VERSION_VALUE=$(FW_VERSION_DEC) \
-DARM_MATH_CM0PLUS

MCU = -mcpu=cortex-m0plus -mthumb

# Compilation flags
# Enable Link Time Optimization (LTO) and section dead-stripping
CFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) -O2 -Wall -fdata-sections -ffunction-sections -flto -MMD -MP

# Linker settings
LDSCRIPT = py32f071xb.ld
LIBS = -lc -lm -lnosys
# Enable cross-file optimization via LTO during the linking phase
LDFLAGS = $(MCU) -specs=nano.specs -flto -T$(LDSCRIPT) $(LIBS) -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref

# ========================================================
# Build Rules
# ========================================================
.PHONY: all clean release deploy_header 00 01

all: deploy_header $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

# Automatically deploy the register definition header to the adjacent Arduino library repository
deploy_header:
	@mkdir -p $(ARDUINO_DIR)/src
	cp $(CORE_DIR)/Inc/mimic_registers.h $(ARDUINO_DIR)/src/mimic_registers.h
	@echo "✓ mimic_registers.h has been automatically deployed to $(ARDUINO_DIR)/src/"

00 01:
	$(MAKE) HW_ID=$@ all

release:
	$(MAKE) clean
	$(MAKE) 00
	$(MAKE) 01

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
