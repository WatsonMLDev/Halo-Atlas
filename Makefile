CLANG ?= clang
PYTHON ?= python3

BUILD_DIR := build
GEN_DIR := $(BUILD_DIR)/gen
OBJ_DIR := $(BUILD_DIR)/obj

LIBPEBBLE := third_party/pebble_sdk/libpebble.a
GEN_SCRIPT := tools/gen_syscall_table.py
GEN_HDR := $(GEN_DIR)/pebble_syscall_table_gen.h
GEN_SRC := $(GEN_DIR)/pebble_syscall_table_gen.c

CLANG_RES_DIR := $(shell $(CLANG) -print-resource-dir)
ARM_CFLAGS := --target=arm-linux-gnueabihf -march=armv7ve -mthumb -mfpu=vfpv3-d16 -mfloat-abi=hard \
	-ffreestanding -fno-stack-protector -nostdinc -isystem $(CLANG_RES_DIR)/include \
	-Icore/include -I$(GEN_DIR) -O2 -Wall -Wextra -Werror
ARM_LDFLAGS := --target=arm-linux-gnueabihf -fuse-ld=lld -nostdlib -static

CORE_SRCS := core/src/pebble_loader.c core/src/pebble_runtime.c $(GEN_SRC)
HAL_SRCS := hal/posix_arm/pebble_hal_posix_arm.c hal/posix_arm/freestanding_rt.c
HAL_ASMS := hal/posix_arm/linux_arm_start.S

RUNNER := $(BUILD_DIR)/pebble-runner

.PHONY: all gen runner test clean

all: runner

gen: $(GEN_HDR) $(GEN_SRC)

$(GEN_HDR) $(GEN_SRC): $(GEN_SCRIPT) $(LIBPEBBLE)
	$(PYTHON) $(GEN_SCRIPT) --lib $(LIBPEBBLE) --out-dir $(GEN_DIR)

$(RUNNER): $(GEN_HDR) $(GEN_SRC) $(CORE_SRCS) $(HAL_SRCS) $(HAL_ASMS)
	@mkdir -p $(OBJ_DIR)
	$(CLANG) $(ARM_CFLAGS) -c core/src/pebble_loader.c -o $(OBJ_DIR)/pebble_loader.o
	$(CLANG) $(ARM_CFLAGS) -c core/src/pebble_runtime.c -o $(OBJ_DIR)/pebble_runtime.o
	$(CLANG) $(ARM_CFLAGS) -c $(GEN_SRC) -o $(OBJ_DIR)/pebble_syscall_table_gen.o
	$(CLANG) $(ARM_CFLAGS) -c hal/posix_arm/pebble_hal_posix_arm.c -o $(OBJ_DIR)/pebble_hal_posix_arm.o
	$(CLANG) $(ARM_CFLAGS) -c hal/posix_arm/freestanding_rt.c -o $(OBJ_DIR)/freestanding_rt.o
	$(CLANG) --target=arm-linux-gnueabihf -march=armv7ve -mthumb -c hal/posix_arm/linux_arm_start.S -o $(OBJ_DIR)/linux_arm_start.o
	$(CLANG) $(ARM_LDFLAGS) \
		$(OBJ_DIR)/linux_arm_start.o \
		$(OBJ_DIR)/pebble_loader.o \
		$(OBJ_DIR)/pebble_runtime.o \
		$(OBJ_DIR)/pebble_syscall_table_gen.o \
		$(OBJ_DIR)/pebble_hal_posix_arm.o \
		$(OBJ_DIR)/freestanding_rt.o \
		-o $(RUNNER)

runner: $(RUNNER)

test: runner
	$(PYTHON) -m unittest discover -s tests -v

clean:
	rm -rf $(BUILD_DIR)
