CLANG ?= clang
PYTHON ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

BUILD_DIR := build
GEN_DIR := $(BUILD_DIR)/gen
OBJ_DIR := $(BUILD_DIR)/obj

CORE_DIR := runtime/core
HAL_DIR := runtime/hal/posix_arm
LIBPEBBLE := third_party/pebble_sdk/libpebble.a
GEN_SCRIPT := tools/gen_syscall_table.py
GEN_HDR := $(GEN_DIR)/pebble_syscall_table_gen.h
GEN_SRC := $(GEN_DIR)/pebble_syscall_table_gen.c

CLANG_RES_DIR := $(shell $(CLANG) -print-resource-dir)
BASE_ARM_FLAGS := --target=arm-linux-gnueabihf -march=armv7ve -mthumb -mfpu=vfpv3-d16 -mfloat-abi=hard \
	-ffreestanding -fno-stack-protector -nostdinc -isystem $(CLANG_RES_DIR)/include -O2
CORE_CFLAGS := $(BASE_ARM_FLAGS) -I$(CORE_DIR)/include -I$(GEN_DIR) -Wall -Wextra -Werror
HAL_CFLAGS := $(CORE_CFLAGS) -DCONFIG_CANVAS_ROTATE_90 -I$(HAL_DIR)/canvas_shims -Ithird_party/halo_canvas
CANVAS_CFLAGS := $(BASE_ARM_FLAGS) -DCONFIG_CANVAS_ROTATE_90 -I$(HAL_DIR)/canvas_shims -Ithird_party/halo_canvas
ARM_LDFLAGS := --target=arm-linux-gnueabihf -fuse-ld=lld -nostdlib -static

CORE_SRCS := \
	$(CORE_DIR)/src/pebble_loader.c \
	$(CORE_DIR)/src/pebble_runtime.c \
	$(CORE_DIR)/src/pebble_stdlib.c \
	$(CORE_DIR)/src/pebble_res.c \
	$(CORE_DIR)/src/pebble_bitmap.c \
	$(CORE_DIR)/src/pebble_font.c \
	$(CORE_DIR)/src/pebble_font_dogica.c \
	$(CORE_DIR)/src/pebble_gfx.c \
	$(CORE_DIR)/src/pebble_persist.c \
	$(CORE_DIR)/src/pebble_timer.c \
	$(CORE_DIR)/src/pebble_ui.c

HAL_SRCS := \
	$(HAL_DIR)/pebble_hal_posix_arm.c \
	$(HAL_DIR)/runner_cli.c \
	$(HAL_DIR)/freestanding_rt.c

CORE_OBJS := $(patsubst $(CORE_DIR)/src/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))
HAL_OBJS := $(patsubst $(HAL_DIR)/%.c,$(OBJ_DIR)/%.o,$(HAL_SRCS))
ALL_OBJS := $(OBJ_DIR)/linux_arm_start.o $(CORE_OBJS) $(OBJ_DIR)/pebble_syscall_table_gen.o $(OBJ_DIR)/canvas.o $(HAL_OBJS)

RUNNER := $(BUILD_DIR)/pebble-runner

.PHONY: all gen runner test zephyr-gate clean

all: runner

gen: $(GEN_HDR) $(GEN_SRC)

$(GEN_HDR) $(GEN_SRC): $(GEN_SCRIPT) $(LIBPEBBLE)
	$(PYTHON) $(GEN_SCRIPT) --lib $(LIBPEBBLE) --out-dir $(GEN_DIR)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(CORE_DIR)/src/%.c $(GEN_HDR) | $(OBJ_DIR)
	$(CLANG) $(CORE_CFLAGS) -c $< -o $@

$(OBJ_DIR)/pebble_syscall_table_gen.o: $(GEN_SRC) $(GEN_HDR) | $(OBJ_DIR)
	$(CLANG) $(CORE_CFLAGS) -c $< -o $@

$(OBJ_DIR)/canvas.o: third_party/halo_canvas/canvas.c | $(OBJ_DIR)
	$(CLANG) $(CANVAS_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(HAL_DIR)/%.c $(GEN_HDR) | $(OBJ_DIR)
	$(CLANG) $(HAL_CFLAGS) -c $< -o $@

$(OBJ_DIR)/linux_arm_start.o: $(HAL_DIR)/linux_arm_start.S | $(OBJ_DIR)
	$(CLANG) --target=arm-linux-gnueabihf -march=armv7ve -mthumb -c $< -o $@

$(RUNNER): $(ALL_OBJS)
	$(CLANG) $(ARM_LDFLAGS) $(ALL_OBJS) -o $@

runner: $(RUNNER)

test: runner
	$(PYTHON) -m unittest discover -s tests -v

zephyr-gate:
	bash tools/run_zephyr_gate.sh all

clean:
	rm -rf $(BUILD_DIR)
