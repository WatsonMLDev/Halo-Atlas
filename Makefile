CLANG ?= clang
PYTHON ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

BUILD_DIR := build
GEN_DIR := $(BUILD_DIR)/gen
OBJ_DIR := $(BUILD_DIR)/obj

LIBPEBBLE := third_party/pebble_sdk/libpebble.a
GEN_SCRIPT := tools/gen_syscall_table.py
GEN_HDR := $(GEN_DIR)/pebble_syscall_table_gen.h
GEN_SRC := $(GEN_DIR)/pebble_syscall_table_gen.c

CLANG_RES_DIR := $(shell $(CLANG) -print-resource-dir)
BASE_ARM_FLAGS := --target=arm-linux-gnueabihf -march=armv7ve -mthumb -mfpu=vfpv3-d16 -mfloat-abi=hard \
	-ffreestanding -fno-stack-protector -nostdinc -isystem $(CLANG_RES_DIR)/include -O2
CORE_CFLAGS := $(BASE_ARM_FLAGS) -Icore/include -I$(GEN_DIR) -Wall -Wextra -Werror
HAL_CFLAGS := $(CORE_CFLAGS) -DCONFIG_CANVAS_ROTATE_90 -Ihal/posix_arm/canvas_shims -Ithird_party/halo_canvas
CANVAS_CFLAGS := $(BASE_ARM_FLAGS) -DCONFIG_CANVAS_ROTATE_90 -Ihal/posix_arm/canvas_shims -Ithird_party/halo_canvas
ARM_LDFLAGS := --target=arm-linux-gnueabihf -fuse-ld=lld -nostdlib -static

CORE_SRCS := \
	core/src/pebble_loader.c \
	core/src/pebble_runtime.c \
	core/src/pebble_stdlib.c \
	core/src/pebble_res.c \
	core/src/pebble_bitmap.c \
	core/src/pebble_font.c \
	core/src/pebble_font_dogica.c \
	core/src/pebble_gfx.c \
	core/src/pebble_persist.c \
	core/src/pebble_timer.c \
	core/src/pebble_ui.c

HAL_SRCS := \
	hal/posix_arm/pebble_hal_posix_arm.c \
	hal/posix_arm/runner_cli.c \
	hal/posix_arm/freestanding_rt.c

CANVAS_SRC := third_party/halo_canvas/canvas.c
HAL_ASMS := hal/posix_arm/linux_arm_start.S

RUNNER := $(BUILD_DIR)/pebble-runner

.PHONY: all gen runner test zephyr-gate clean

all: runner

gen: $(GEN_HDR) $(GEN_SRC)

$(GEN_HDR) $(GEN_SRC): $(GEN_SCRIPT) $(LIBPEBBLE)
	$(PYTHON) $(GEN_SCRIPT) --lib $(LIBPEBBLE) --out-dir $(GEN_DIR)

$(RUNNER): $(GEN_HDR) $(GEN_SRC) $(CORE_SRCS) $(HAL_SRCS) $(CANVAS_SRC) $(HAL_ASMS)
	@mkdir -p $(OBJ_DIR)
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_loader.c -o $(OBJ_DIR)/pebble_loader.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_runtime.c -o $(OBJ_DIR)/pebble_runtime.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_stdlib.c -o $(OBJ_DIR)/pebble_stdlib.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_res.c -o $(OBJ_DIR)/pebble_res.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_bitmap.c -o $(OBJ_DIR)/pebble_bitmap.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_font.c -o $(OBJ_DIR)/pebble_font.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_font_dogica.c -o $(OBJ_DIR)/pebble_font_dogica.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_gfx.c -o $(OBJ_DIR)/pebble_gfx.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_persist.c -o $(OBJ_DIR)/pebble_persist.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_timer.c -o $(OBJ_DIR)/pebble_timer.o
	$(CLANG) $(CORE_CFLAGS) -c core/src/pebble_ui.c -o $(OBJ_DIR)/pebble_ui.o
	$(CLANG) $(CORE_CFLAGS) -c $(GEN_SRC) -o $(OBJ_DIR)/pebble_syscall_table_gen.o
	$(CLANG) $(CANVAS_CFLAGS) -c $(CANVAS_SRC) -o $(OBJ_DIR)/canvas.o
	$(CLANG) $(HAL_CFLAGS) -c hal/posix_arm/pebble_hal_posix_arm.c -o $(OBJ_DIR)/pebble_hal_posix_arm.o
	$(CLANG) $(HAL_CFLAGS) -c hal/posix_arm/runner_cli.c -o $(OBJ_DIR)/runner_cli.o
	$(CLANG) $(HAL_CFLAGS) -c hal/posix_arm/freestanding_rt.c -o $(OBJ_DIR)/freestanding_rt.o
	$(CLANG) --target=arm-linux-gnueabihf -march=armv7ve -mthumb -c $(HAL_ASMS) -o $(OBJ_DIR)/linux_arm_start.o
	$(CLANG) $(ARM_LDFLAGS) \
		$(OBJ_DIR)/linux_arm_start.o \
		$(OBJ_DIR)/pebble_loader.o \
		$(OBJ_DIR)/pebble_runtime.o \
		$(OBJ_DIR)/pebble_stdlib.o \
		$(OBJ_DIR)/pebble_res.o \
		$(OBJ_DIR)/pebble_bitmap.o \
		$(OBJ_DIR)/pebble_font.o \
		$(OBJ_DIR)/pebble_font_dogica.o \
		$(OBJ_DIR)/pebble_gfx.o \
		$(OBJ_DIR)/pebble_persist.o \
		$(OBJ_DIR)/pebble_timer.o \
		$(OBJ_DIR)/pebble_ui.o \
		$(OBJ_DIR)/pebble_syscall_table_gen.o \
		$(OBJ_DIR)/canvas.o \
		$(OBJ_DIR)/pebble_hal_posix_arm.o \
		$(OBJ_DIR)/runner_cli.o \
		$(OBJ_DIR)/freestanding_rt.o \
		-o $(RUNNER)

runner: $(RUNNER)

test: runner
	$(PYTHON) -m unittest discover -s tests -v

zephyr-gate:
	bash tools/run_zephyr_gate.sh all

clean:
	rm -rf $(BUILD_DIR)
