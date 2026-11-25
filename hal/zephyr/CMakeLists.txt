# Copyright (c) 2026 Halo-Atlas Contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_PEBBLE_COMPAT)

get_filename_component(HALO_ATLAS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PEBBLE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(PEBBLE_GEN_HDR "${PEBBLE_GEN_DIR}/pebble_syscall_table_gen.h")
set(PEBBLE_GEN_SRC "${PEBBLE_GEN_DIR}/pebble_syscall_table_gen.c")

file(MAKE_DIRECTORY "${PEBBLE_GEN_DIR}")
execute_process(
	COMMAND "${PYTHON_EXECUTABLE}"
		"${HALO_ATLAS_ROOT}/tools/gen_syscall_table.py"
		--lib "${HALO_ATLAS_ROOT}/third_party/pebble_sdk/libpebble.a"
		--out-dir "${PEBBLE_GEN_DIR}"
		--objdump "${CMAKE_OBJDUMP}"
	RESULT_VARIABLE PEBBLE_GEN_RC
)
if(NOT PEBBLE_GEN_RC EQUAL 0)
	message(FATAL_ERROR "gen_syscall_table.py failed with exit code ${PEBBLE_GEN_RC}")
endif()

zephyr_library()

zephyr_include_directories(
	include
	"${HALO_ATLAS_ROOT}/core/include"
	"${PEBBLE_GEN_DIR}"
)

set(PEBBLE_CORE_SRCS
	"${HALO_ATLAS_ROOT}/core/src/pebble_loader.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_runtime.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_stdlib.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_res.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_bitmap.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_font.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_font_dogica.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_gfx.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_persist.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_timer.c"
	"${HALO_ATLAS_ROOT}/core/src/pebble_ui.c"
	"${PEBBLE_GEN_SRC}"
)

zephyr_library_sources(
	${PEBBLE_CORE_SRCS}
	pebble_hal_zephyr.c
)

zephyr_library_compile_options(-Wall -Wextra)

if(CONFIG_PEBBLE_COMPAT_KEEP_ENTRYPOINTS)
	zephyr_ld_options(
		-Wl,--undefined=pebble_zephyr_hal_init
		-Wl,--undefined=pebble_runtime_run
	)
endif()

endif() # CONFIG_PEBBLE_COMPAT
